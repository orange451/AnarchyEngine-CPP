#include "ide/IdeExplorer.hpp"
#include "ChangeHistoryService.hpp"
#include "ide/PropertiesPanel.hpp"
#include "ide/PropertySheet.hpp"
#include "SelectionService.hpp"

#include "AssetInstances.hpp"
#include "Camera.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Brush.hpp"
#include "Game.hpp"
#include "Gui.hpp"
#include "Light.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "ModuleScript.hpp"
#include "PhysicsObject.hpp"
#include "Enum.hpp"
#include "Script.hpp"
#include "SoundEmitter.hpp"
#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

using engine_core::DataModel;
using engine_core::Game;
using engine_core::InstanceId;

// A class the panel has never heard of, with a number and a boolean. Its rows
// come from the class registry like any other.
class Probe : public DataModel {
public:
    Probe(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "Probe"; }
    double speed = 0;
    bool flag = false;
};

bool ReadSpeed(DataModel&, DataModel& object, engine_core::LuaSlot& out) {
    auto* probe = dynamic_cast<Probe*>(&object);
    if (probe == nullptr) {
        return false;
    }
    out.kind = engine_core::LuaSlot::Kind::Number;
    out.number = probe->speed;
    return true;
}

bool WriteSpeed(DataModel&, DataModel& object, engine_core::LuaSlot& in) {
    auto* probe = dynamic_cast<Probe*>(&object);
    if (probe == nullptr) {
        return false;
    }
    probe->speed = in.number;
    return true;
}

bool ReadFlag(DataModel&, DataModel& object, engine_core::LuaSlot& out) {
    auto* probe = dynamic_cast<Probe*>(&object);
    if (probe == nullptr) {
        return false;
    }
    out.kind = engine_core::LuaSlot::Kind::Bool;
    out.flag = probe->flag;
    return true;
}

bool WriteFlag(DataModel&, DataModel& object, engine_core::LuaSlot& in) {
    auto* probe = dynamic_cast<Probe*>(&object);
    if (probe == nullptr) {
        return false;
    }
    probe->flag = in.flag;
    return true;
}

void RegisterProbe() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    const engine_core::LuaField fields[] = {
        engine_core::lua_property("Speed", "number", true, ReadSpeed, WriteSpeed),
        engine_core::lua_property("Flag", "boolean", true, ReadFlag, WriteFlag),
    };
    engine_core::register_lua_class("Probe", "DataModel", fields, 2);
}

// Two classes with groups. GroupKid adds to GroupBase's "Look", registers
// Alpha again under "Extra", and has one field with no group.
class GroupBase : public Probe {
public:
    using Probe::Probe;
    const char* class_name() const override { return "GroupBase"; }
};

class GroupKid : public Probe {
public:
    using Probe::Probe;
    const char* class_name() const override { return "GroupKid"; }
};

void RegisterGroups() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    using engine_core::lua_group;
    using engine_core::lua_property;
    const engine_core::LuaField base[] = {
        lua_group("Look"),
        lua_property("Zeta", "number", true, ReadSpeed, WriteSpeed),
        lua_property("Alpha", "number", true, ReadSpeed, WriteSpeed),
        lua_group("Feel"),
        lua_property("Mid", "boolean", true, ReadFlag, WriteFlag),
    };
    engine_core::register_lua_class("GroupBase", "DataModel", base, static_cast<int>(std::size(base)));
    const engine_core::LuaField kid[] = {
        lua_property("Loose", "number", true, ReadSpeed, WriteSpeed),
        lua_group("Look"),
        lua_property("Kappa", "number", true, ReadSpeed, WriteSpeed),
        lua_group("Extra"),
        lua_property("Alpha", "number", true, ReadSpeed, WriteSpeed),
        lua_property("Beta", "number", true, ReadSpeed, WriteSpeed),
    };
    engine_core::register_lua_class("GroupKid", "GroupBase", kid, static_cast<int>(std::size(kid)));
}

const char* GroupOf(const std::vector<engine_core::LuaField>& fields, const char* name) {
    for (const engine_core::LuaField& field : fields) {
        if (field.name != nullptr && std::string_view(field.name) == name) {
            return field.group != nullptr ? field.group : "";
        }
    }
    return "missing";
}

void TestGroupMarkersRegister() {
    RegisterGroups();
    std::vector<engine_core::LuaField> members;
    engine_core::lua_class_members("GroupKid", members);
    Expect(std::string_view(GroupOf(members, "Zeta")) == "Look", "a field takes the marker above it");
    Expect(std::string_view(GroupOf(members, "Mid")) == "Feel", "a later marker starts a new group");
    Expect(std::string_view(GroupOf(members, "Loose")).empty(), "a field above any marker has no group");
    Expect(std::string_view(GroupOf(members, "Alpha")) == "Extra", "a replacing field takes its new group");
    for (const engine_core::LuaField& field : members) {
        Expect(!field.group_marker, "markers are not members");
        Expect(field.name == nullptr ||
                   (std::string_view(field.name) != "Look" && std::string_view(field.name) != "Feel" &&
                    std::string_view(field.name) != "Extra"),
               "no member is named after a group");
    }
    std::vector<engine_core::LuaField> own;
    engine_core::lua_class_own_members("GroupKid", own);
    Expect(own.size() == 4, "own members are the four fields, no markers");
}

engine_core::LuaSlot ColorSlot(engine_core::ColorRgb color) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Color;
    slot.color = color;
    return slot;
}

// A class with one Color3, Tint, whose writes are recorded for undo as a
// registry property's are.
class Swatch : public DataModel {
public:
    Swatch(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "Swatch"; }

    engine_core::ColorRgb tint() const { return tint_; }

    void set_tint(engine_core::ColorRgb color) {
        // A Color3 has no alpha.
        color.a = 1.f;
        if (color.r == tint_.r && color.g == tint_.g && color.b == tint_.b) {
            return;
        }
        const engine_core::ColorRgb previous = tint_;
        tint_ = color;
        note_property_change("Tint", ColorSlot(previous), ColorSlot(color));
    }

private:
    engine_core::ColorRgb tint_{1.f, 1.f, 1.f, 1.f};
};

bool ReadTint(DataModel&, DataModel& object, engine_core::LuaSlot& out) {
    auto* swatch = dynamic_cast<Swatch*>(&object);
    if (swatch == nullptr) {
        return false;
    }
    out = ColorSlot(swatch->tint());
    return true;
}

bool WriteTint(DataModel&, DataModel& object, engine_core::LuaSlot& in) {
    auto* swatch = dynamic_cast<Swatch*>(&object);
    if (swatch == nullptr) {
        return false;
    }
    swatch->set_tint(in.color);
    return true;
}

void RegisterSwatch() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    const engine_core::LuaField fields[] = {
        engine_core::lua_property("Tint", "Color3", true, ReadTint, WriteTint),
    };
    engine_core::register_lua_class("Swatch", "DataModel", fields, 1);
}

engine_core::LuaSlot Vector3Slot(engine_core::Vec3 value) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Vec3;
    slot.vec = value;
    return slot;
}

// A class with one Vector3, Position, and no other property, whose writes are
// recorded for undo as a registry property's are.
class Marker : public DataModel {
public:
    Marker(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "Marker"; }

    engine_core::Vec3 position() const { return position_; }

    void set_position(float x, float y, float z) {
        if (x == position_.x && y == position_.y && z == position_.z) {
            return;
        }
        const engine_core::Vec3 previous = position_;
        position_ = engine_core::Vec3{x, y, z};
        note_property_change("Position", Vector3Slot(previous), Vector3Slot(position_));
    }

private:
    engine_core::Vec3 position_{};
};

bool ReadMarkerPosition(DataModel&, DataModel& object, engine_core::LuaSlot& out) {
    auto* marker = dynamic_cast<Marker*>(&object);
    if (marker == nullptr) {
        return false;
    }
    out = Vector3Slot(marker->position());
    return true;
}

bool WriteMarkerPosition(DataModel&, DataModel& object, engine_core::LuaSlot& in) {
    auto* marker = dynamic_cast<Marker*>(&object);
    if (marker == nullptr) {
        return false;
    }
    marker->set_position(in.vec.x, in.vec.y, in.vec.z);
    return true;
}

void RegisterMarker() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    const engine_core::LuaField fields[] = {
        engine_core::lua_property("Position", "Vector3", true, ReadMarkerPosition, WriteMarkerPosition),
    };
    engine_core::register_lua_class("Marker", "Instance", fields, 1);
}

constexpr double kWidth = 900;
constexpr double kHeight = 600;

// An explorer and the Properties page side by side over one place. Writes
// run on this thread, standing in for the simulation thread.
struct Rig {
    Game game;
    InstanceId a = 0;
    InstanceId b = 0;
    InstanceId script = 0;
    InstanceId folder = 0;
    InstanceId marker1 = 0;
    InstanceId marker2 = 0;
    std::shared_ptr<ide::IdeExplorer> explorer;
    ide::PropertiesPanel panel;
    std::shared_ptr<jadefx::Scene> scene;
    double clock = 0;

    Rig() {
        a = add<engine_core::GameObject>("A");
        b = add<engine_core::GameObject>("B");
        script = add<engine_core::Script>("Main");
        folder = add<engine_core::Folder>("Stuff");
        RegisterMarker();
        marker1 = add<Marker>("M1");
        marker2 = add<Marker>("M2");
        ide::ExplorerHost host;
        host.enabled = [](engine_core::InstanceAction) { return true; };
        host.rename = [this](InstanceId id, std::string name) { game.set_name(id, name); };
        explorer = jadefx::make<ide::IdeExplorer>(game, "Explorer", host);
        panel.bind(game, game.selection(), game.history());
        auto split = jadefx::make<jadefx::SplitPane>();
        split->getItems().add(explorer);
        split->getItems().add(panel.dock_widget());
        split->setDividerPositions({0.4});
        split->setPrefWidthRatio(1);
        split->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(split, kWidth, kHeight);
        // Building the place is not an edit anyone should undo.
        game.history().reset_waypoints();
        frame();
    }

    template <typename T>
    InstanceId add(const char* name) {
        T& made = game.create<T>();
        game.set_name(made.id(), name);
        game.set_parent(made.id(), game.scene_service("Workspace"));
        return made.id();
    }

    void frame() {
        clock += 0.05;
        scene->layout(kWidth, kHeight, clock);
    }

    void select(std::vector<InstanceId> ids) {
        game.selection().set(std::move(ids));
        frame();
    }

    jadefx::TextField* field(const std::string& property, int part = 0) {
        return dynamic_cast<jadefx::TextField*>(panel.editor(property, part));
    }

    // A field's text, or a reference's Name on its button.
    std::string text(const std::string& property, int part = 0) {
        if (jadefx::TextField* box = field(property, part)) {
            return box->getText();
        }
        auto* button = dynamic_cast<jadefx::Button*>(panel.editor(property, part));
        return button != nullptr ? button->getText() : std::string("<no field>");
    }

    void click(jadefx::Node* node) {
        Expect(node != nullptr && node->isVisible(), "the widget to click is on screen");
        if (node == nullptr) {
            return;
        }
        const double x = node->getAbsoluteX() + node->getWidth() * 0.5;
        const double y = node->getAbsoluteY() + node->getHeight() * 0.5;
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
    }

    // Clicks into the field, selects what is there, and types over it.
    void typeInto(const std::string& property, const std::string& typed, int part = 0) {
        jadefx::TextField* box = field(property, part);
        click(box);
        Expect(box != nullptr && box->isFocused(), "a click focuses the field");
        if (box == nullptr) {
            return;
        }
        box->selectAll();
        if (!typed.empty()) {
            scene->noteText(typed);
        }
    }

    void key(int code) { scene->noteKey(code, true, false, 0); }

    void enter() {
        key(jadefx::Key::Enter);
        frame();
    }

    // A click on empty explorer space takes focus off the panel.
    void clickAway() {
        const double x = explorer->getAbsoluteX() + 20;
        const double y = explorer->getAbsoluteY() + explorer->getHeight() - 10;
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
        frame();
    }

    jadefx::Node* explorerRow(const std::string& name) {
        for (jadefx::Node* candidate : explorer->getElementsByClassName("tree-cell")) {
            for (jadefx::Node* label : candidate->getElementsByClassName("tree-cell-label")) {
                auto* text = dynamic_cast<jadefx::Label*>(label);
                if (text != nullptr && text->getText() == name) {
                    return candidate;
                }
            }
        }
        return nullptr;
    }

    void clickExplorer(const std::string& name) {
        jadefx::Node* row = explorerRow(name);
        Expect(row != nullptr, "the explorer shows the row to pick");
        if (row == nullptr) {
            return;
        }
        const double x = row->getAbsoluteX() + row->getWidth() * 0.3;
        const double y = row->getAbsoluteY() + row->getHeight() * 0.5;
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
        frame();
        frame();
    }

    engine_core::Vec3 position(InstanceId id) {
        auto* marker = dynamic_cast<Marker*>(game.instance(id));
        return marker != nullptr ? marker->position() : engine_core::Vec3{};
    }

    bool hasRow(const std::string& name) const { return panel.sheet().find(name) != nullptr; }

    bool mixed(const std::string& name) const {
        const ide::PropertyRow* row = panel.sheet().find(name);
        return row != nullptr && row->mixed;
    }

    int undoDepth() {
        int depth = 0;
        while (game.history().can_undo().first && depth < 100) {
            game.history().undo();
            ++depth;
        }
        for (int step = 0; step < depth; ++step) {
            game.history().redo();
        }
        return depth;
    }
};

void TestR1SingleSelectionEditsName() {
    Rig rig;
    rig.select({rig.a});
    Expect(rig.text("Name") == "A", "R1 the Name field shows the Name");
    Expect(rig.text("ClassName") == "GameObject", "R1 ClassName is shown");
    auto* cls = rig.field("ClassName");
    Expect(cls != nullptr && !cls->isEditable(), "R1 ClassName is read-only");
    Expect(rig.hasRow("Transform") && !rig.hasRow("Position"), "R1 a GameObject shows Transform, not Position");
    Expect(!rig.hasRow("Source") && !rig.hasRow("Changed"),
           "R1 Source and signals have no row");

    rig.typeInto("Name", "Hero");
    Expect(rig.game.name(rig.a) == "A", "R1 typing alone does not write");
    rig.enter();
    Expect(rig.game.name(rig.a) == "Hero", "R1 Enter writes the Name");
    rig.frame();
    Expect(rig.explorerRow("Hero") != nullptr, "R1 the explorer label follows");
    Expect(rig.text("Name") == "Hero", "R1 the field shows the written Name");
    const auto undo = rig.game.history().can_undo();
    Expect(undo.first && undo.second == "Set Name", "R1 the waypoint is Set Name");

    // Leaving the field commits what was typed.
    rig.typeInto("Name", "Blurred");
    rig.frame();
    rig.clickAway();
    Expect(rig.game.name(rig.a) == "Blurred", "R1 focus leaving the field commits");
}

void TestR2MixedNameWritesEveryone() {
    Rig rig;
    rig.select({rig.a, rig.b});
    Expect(rig.hasRow("Name") && rig.mixed("Name"), "R2 the Name row is mixed");
    Expect(rig.text("Name").empty(), "R2 a mixed Name is blank, not the first Name");

    rig.typeInto("Name", "C");
    rig.enter();
    Expect(rig.game.name(rig.a) == "C" && rig.game.name(rig.b) == "C", "R2 both Names are C");
    Expect(rig.text("Name") == "C" && !rig.mixed("Name"), "R2 the row now agrees");
    Expect(rig.undoDepth() == 1, "R2 one waypoint for both writes");

    rig.game.history().undo();
    rig.frame();
    Expect(rig.game.name(rig.a) == "A" && rig.game.name(rig.b) == "B", "R2 one undo restores A and B");
    Expect(rig.text("Name").empty() && rig.mixed("Name"), "R2 after undo the row is mixed again");
}

void TestMixedBlankIsNotEmptyName() {
    Rig rig;
    rig.select({rig.a, rig.b});
    // Focus the blank field and leave it: nothing was typed, nothing is written.
    rig.click(rig.field("Name"));
    rig.frame();
    rig.clickAway();
    rig.select({rig.a, rig.b});
    Expect(rig.game.name(rig.a) == "A" && rig.game.name(rig.b) == "B", "blank focus-leave writes nothing");
    Expect(!rig.game.history().can_undo().first, "blank focus-leave leaves no waypoint");

    // Enter on the pristine blank field is a no-op too.
    rig.click(rig.field("Name"));
    rig.enter();
    Expect(rig.game.name(rig.a) == "A" && rig.game.name(rig.b) == "B", "blank Enter writes nothing");

    // Typing and then erasing it is an explicit empty Name.
    rig.typeInto("Name", "x");
    rig.key(jadefx::Key::Backspace);
    Expect(rig.text("Name").empty(), "the typed text was erased");
    rig.enter();
    Expect(rig.game.name(rig.a).empty() && rig.game.name(rig.b).empty(), "a cleared field sets an empty Name");
}

void TestR3IntersectionOnly() {
    Rig rig;
    rig.select({rig.a, rig.script});
    Expect(rig.hasRow("Name"), "R3 Name is shared");
    Expect(rig.hasRow("Parent"), "R3 Parent is shared");
    Expect(!rig.hasRow("Transform"), "R3 Transform is not on a Script");
    Expect(!rig.hasRow("Enabled"), "R3 Enabled is not on a GameObject");
    Expect(rig.panel.editor("Transform") == nullptr, "R3 no Transform widget");
    // ClassName differs, so it is mixed and blank.
    Expect(rig.mixed("ClassName") && rig.text("ClassName").empty(), "R3 differing classes are mixed");

    // Two classes that each have a Transform of the same type share its row.
    const InstanceId camera = rig.add<engine_core::Camera>("Cam");
    rig.select({camera, rig.a});
    Expect(rig.hasRow("Transform"), "R3 a field both classes have, with one type, shows");
}

bool Near(float a, float b) { return a > b - 0.001f && a < b + 0.001f; }

// A Transform is its name and an arrow, then a Position line and an
// Orientation line, each X, Y, and Z. One part writes only that part.
void TestTransformRow() {
    Rig rig;
    auto* part = rig.game.game_object(rig.a);
    part->set_transform(engine_core::matrix4_translation(1.f, 2.f, 3.f));
    rig.game.history().reset_waypoints();
    rig.select({rig.a});
    Expect(rig.text("Transform", 0) == "1" && rig.text("Transform", 1) == "2" && rig.text("Transform", 2) == "3",
           "Transform's Position line shows the translation");
    Expect(rig.text("Transform", 3) == "0" && rig.text("Transform", 4) == "0" && rig.text("Transform", 5) == "0",
           "Transform's Orientation line shows no turn");
    const char* colors[3] = {"--ide-properties-x-color", "--ide-properties-y-color", "--ide-properties-z-color"};
    for (int axis = 0; axis < ide::kTransformParts; ++axis) {
        jadefx::TextField* box = rig.field("Transform", axis);
        Expect(box != nullptr && box->getStyle().find(colors[axis % 3]) != std::string::npos,
               "Position's and Orientation's X, Y, and Z are red, green, and blue");
    }

    rig.typeInto("Transform", "5", 1);
    rig.enter();
    engine_core::Vec3 at = part->position();
    Expect(at.x == 1.f && at.y == 5.f && at.z == 3.f, "a Position axis writes only that axis");

    rig.typeInto("Transform", "90", 4);
    rig.enter();
    const engine_core::Vec3 turned = ide::transform_orientation(part->transform());
    at = part->position();
    Expect(Near(turned.x, 0.f) && Near(turned.y, 90.f) && Near(turned.z, 0.f), "an Orientation axis turns it");
    Expect(at.x == 1.f && at.y == 5.f && at.z == 3.f, "turning keeps the Position");
    Expect(rig.text("Transform", 4) == "90", "Orientation reads back as typed");
    Expect(Near(part->transform().m[8], 1.f), "90 about Y turns the back axis to +X");

    rig.typeInto("Transform", "abc", 3);
    rig.enter();
    Expect(rig.panel.status() == "Orientation must be a number" && rig.text("Transform", 3) == "0",
           "a bad angle is refused and shows the value again");

    // Two that differ in one part leave only that part blank.
    rig.select({rig.a, rig.b});
    Expect(rig.text("Transform", 0).empty() && rig.text("Transform", 4).empty() && rig.text("Transform", 3) == "0",
           "a Transform blanks only the parts that differ");

    // The arrow folds the row, and a fold outlasts the selection.
    rig.select({rig.a});
    jadefx::Node* arrow = rig.panel.editor("Transform", ide::kTransformParts);
    rig.click(arrow);
    rig.frame();
    Expect(!rig.field("Transform", 0)->isVisible() && !rig.field("Transform", 5)->isVisible(),
           "folded, the Position and Orientation lines are hidden");
    rig.select({rig.b});
    rig.select({rig.a});
    Expect(!rig.field("Transform", 0)->isVisible(), "the fold outlasts the selection");
    rig.click(rig.panel.editor("Transform", ide::kTransformParts));
    rig.frame();
    Expect(rig.field("Transform", 0)->isVisible() && rig.field("Transform", 5)->isVisible(),
           "clicking the arrow again opens it");
}

void TestTransformOrientationKeepsScale() {
    engine_core::Matrix4 scaled = engine_core::matrix4_identity();
    scaled.m[0] = 2.f;
    scaled.m[12] = 7.f;
    const engine_core::Matrix4 turned = ide::transform_with_orientation(scaled, engine_core::Vec3{0.f, 90.f, 0.f});
    Expect(Near(turned.m[2], -2.f) && Near(turned.m[0], 0.f), "the X axis turns and keeps its length");
    Expect(turned.m[12] == 7.f && turned.m[15] == 1.f, "the translation stays");
    const engine_core::Vec3 back = ide::transform_orientation(turned);
    Expect(Near(back.x, 0.f) && Near(back.y, 90.f) && Near(back.z, 0.f), "scale does not change the Orientation");
}

void TestR4SameValueIsNotMixed() {
    Rig rig;
    rig.game.set_name(rig.a, "X");
    rig.game.set_name(rig.b, "X");
    rig.select({rig.a, rig.b});
    Expect(!rig.mixed("Name") && rig.text("Name") == "X", "R4 equal Names show the Name");
    Expect(rig.text("ClassName") == "GameObject", "R4 equal classes show the class");
}

void TestR5Vector3PerAxis() {
    Rig rig;
    auto* t1 = dynamic_cast<Marker*>(rig.game.instance(rig.marker1));
    auto* t2 = dynamic_cast<Marker*>(rig.game.instance(rig.marker2));
    t1->set_position(1, 2, 3);
    t2->set_position(1, 9, 3);
    rig.game.history().reset_waypoints();
    rig.select({rig.marker1, rig.marker2});
    Expect(rig.text("Position", 0) == "1", "R5 X agrees");
    Expect(rig.text("Position", 1).empty(), "R5 Y is mixed and blank");
    Expect(rig.text("Position", 2) == "3", "R5 Z agrees");

    rig.typeInto("Position", "5", 1);
    rig.enter();
    const engine_core::Vec3 p1 = rig.position(rig.marker1);
    const engine_core::Vec3 p2 = rig.position(rig.marker2);
    Expect(p1.x == 1 && p1.y == 5 && p1.z == 3, "R5 the first is (1,5,3)");
    Expect(p2.x == 1 && p2.y == 5 && p2.z == 3, "R5 the second is (1,5,3)");
    Expect(rig.text("Position", 1) == "5", "R5 Y now shows 5");
    Expect(rig.undoDepth() == 1, "R5 one waypoint");

    // One axis keeps each instance's other components, even mixed ones.
    t1->set_position(7, 5, 3);
    rig.frame();
    Expect(rig.text("Position", 0).empty(), "R5 X is now mixed");
    rig.typeInto("Position", "8", 2);
    rig.enter();
    Expect(rig.position(rig.marker1).x == 7 && rig.position(rig.marker2).x == 1, "R5 a Z edit keeps each X");
    Expect(rig.position(rig.marker1).z == 8 && rig.position(rig.marker2).z == 8, "R5 Z is written to both");

    // Not a number: nothing is written and the field goes back.
    rig.typeInto("Position", "abc", 2);
    rig.enter();
    Expect(rig.position(rig.marker1).z == 8, "R5 a bad number does not write");
    Expect(rig.text("Position", 2) == "8", "R5 a bad number shows the value again");
    Expect(!rig.panel.status().empty(), "R5 a bad number says why");
}

bool SelectedWhole(const jadefx::TextField* box) {
    return box != nullptr && std::min(box->getAnchor(), box->getCaretPosition()) == 0 &&
           std::max(box->getAnchor(), box->getCaretPosition()) == box->getLength();
}

void TestFocusSelectsWhole() {
    Rig rig;
    rig.game.set_name(rig.a, "LongerName");
    rig.select({rig.a});
    jadefx::TextField* name = rig.field("Name");
    // Clicks far enough apart that none is a double click.
    auto clickAt = [&rig, name](double across) {
        const double x = name->getAbsoluteX() + name->getWidth() * across;
        const double y = name->getAbsoluteY() + name->getHeight() * 0.5;
        rig.scene->noteButton(0, true, x, y);
        rig.scene->noteButton(0, false, x, y);
    };
    clickAt(0.5);
    Expect(name->isFocused() && SelectedWhole(name), "a click into a field selects its value");
    rig.scene->noteText("Q");
    Expect(name->getText() == "Q", "so typing replaces it");
    rig.key(jadefx::Key::Escape);
    rig.frame();
    Expect(!name->isFocused() && name->getText() == "LongerName", "Escape drops the typing");

    clickAt(0.2);
    Expect(name->isFocused() && SelectedWhole(name), "each click that focuses the field selects it");
    clickAt(0.05);
    Expect(name->isFocused() && name->getAnchor() == name->getCaretPosition(),
           "a click in the focused field places the caret");
    rig.scene->noteWindowFocus(false);
    rig.scene->noteWindowFocus(true);
    Expect(name->isFocused() && name->getAnchor() == name->getCaretPosition(),
           "focus coming back with the window keeps the caret");
    rig.clickAway();

    // A press that drags selects what it dragged over.
    const double x = name->getAbsoluteX() + 6;
    const double y = name->getAbsoluteY() + name->getHeight() * 0.5;
    rig.scene->noteButton(0, true, x, y);
    rig.scene->noteMove(x + 24, y);
    rig.scene->noteButton(0, false, x + 24, y);
    Expect(name->isFocused() && !name->getSelectedText().empty() && !SelectedWhole(name),
           "a drag that focuses the field keeps its own selection");
    rig.clickAway();
}

void TestTabWalksFields() {
    Rig rig;
    auto* marker = dynamic_cast<Marker*>(rig.game.instance(rig.marker1));
    marker->set_position(1, 2, 3);
    rig.game.history().reset_waypoints();
    rig.select({rig.marker1});
    jadefx::TextField* name = rig.field("Name");
    jadefx::TextField* axes[3] = {rig.field("Position", 0), rig.field("Position", 1), rig.field("Position", 2)};
    auto tab = [&rig](bool back) {
        rig.scene->noteKey(jadefx::Key::Tab, true, false, back ? jadefx::Key::ModShift : 0);
        rig.frame();
    };

    rig.click(name);
    tab(false);
    Expect(axes[0]->isFocused() && SelectedWhole(axes[0]), "Tab skips Parent and read-only ClassName to X");
    rig.scene->noteText("5");
    tab(false);
    Expect(axes[1]->isFocused() && SelectedWhole(axes[1]), "Tab goes from X to Y and selects it");
    Expect(rig.position(rig.marker1).x == 5 && rig.text("Position", 0) == "5", "Tab writes the X typed");
    Expect(rig.undoDepth() == 1, "as one waypoint");
    tab(false);
    Expect(axes[2]->isFocused() && SelectedWhole(axes[2]), "Tab goes from Y to Z");
    tab(true);
    Expect(axes[1]->isFocused() && SelectedWhole(axes[1]), "Shift+Tab goes back to Y");
    tab(false);
    tab(false);
    Expect(name->isFocused() && SelectedWhole(name), "Tab from the last field wraps to the first");
    tab(true);
    Expect(axes[2]->isFocused(), "Shift+Tab from the first field wraps to the last");

    // A value that changes under a field selected whole stays selected whole.
    marker->set_position(5, 2, 7.5f);
    rig.frame();
    Expect(axes[2]->getText() == "7.5" && SelectedWhole(axes[2]), "a new value in a selected field is selected");
    rig.clickAway();
    const engine_core::Vec3 kept = rig.position(rig.marker1);
    Expect(kept.x == 5 && kept.y == 2 && kept.z == 7.5f, "moving through fields writes nothing");
}

void TestPositionAxisColors() {
    Rig rig;
    rig.select({rig.marker1});
    const char* colors[3] = {"--ide-properties-x-color", "--ide-properties-y-color", "--ide-properties-z-color"};
    for (int axis = 0; axis < 3; ++axis) {
        jadefx::TextField* box = rig.field("Position", axis);
        Expect(box != nullptr && box->getStyle().find(colors[axis]) != std::string::npos,
               "Position's X, Y, and Z are red, green, and blue");
    }
    Expect(rig.field("Name")->getStyle().find("--ide-field-color") != std::string::npos,
           "other fields keep the field color");

    // Every Vector3 is tinted, whatever its name.
    const InstanceId body = rig.add<engine_core::PhysicsObject>("Body");
    rig.select({body});
    for (const char* name : {"Velocity", "AngularVelocity", "Size"}) {
        for (int axis = 0; axis < 3; ++axis) {
            jadefx::TextField* box = rig.field(name, axis);
            Expect(box != nullptr && box->getStyle().find(colors[axis]) != std::string::npos,
                   (std::string(name) + "'s X, Y, and Z are red, green, and blue").c_str());
        }
    }
}

bool SameColor(const engine_core::ColorRgb& color, float r, float g, float b) {
    auto near = [](float x, float y) { return x > y - 0.002f && x < y + 0.002f; };
    return near(color.r, r) && near(color.g, g) && near(color.b, b) && color.a == 1.f;
}

// Moves the chooser's red, green, and blue sliders, as a drag does, so the picker hears of it.
void Pick(jadefx::ColorPicker& picker, int r, int g, int b) {
    const char* names[3] = {"red", "green", "blue"};
    const int values[3] = {r, g, b};
    for (int index = 0; index < 3; ++index) {
        for (jadefx::Node* node : picker.getColorChooser().getElementsByClassName(names[index])) {
            if (auto* slider = dynamic_cast<jadefx::Slider*>(node)) {
                slider->setValue(values[index]);
                break;
            }
        }
    }
}

// A Color3 is a color picker. Closing the chooser on a new color writes it to every selected instance.
void TestColorPicker() {
    RegisterSwatch();
    Rig rig;
    const InstanceId first = rig.add<Swatch>("S1");
    const InstanceId second = rig.add<Swatch>("S2");
    auto* a = dynamic_cast<Swatch*>(rig.game.instance(first));
    auto* b = dynamic_cast<Swatch*>(rig.game.instance(second));
    a->set_tint(engine_core::ColorRgb{1.f, 0.f, 0.f, 1.f});
    b->set_tint(engine_core::ColorRgb{1.f, 0.f, 0.f, 1.f});
    rig.game.history().reset_waypoints();
    rig.select({first});
    auto* picker = dynamic_cast<jadefx::ColorPicker*>(rig.panel.editor("Tint"));
    Expect(picker != nullptr && picker->isVisible(), "Tint is a color picker");
    if (picker == nullptr) {
        return;
    }
    Expect(!picker->isDisabled() && picker->getValue().toHex() == "#ff0000", "the picker shows the color");
    Expect(!picker->getColorChooser().isShowAlpha(), "a Color3 has no alpha to pick");

    a->set_tint(engine_core::ColorRgb{0.f, 0.f, 1.f, 1.f});
    rig.game.history().reset_waypoints();
    rig.frame();
    Expect(picker->getValue().toHex() == "#0000ff", "a color set elsewhere shows at once");

    // Escape puts the color back and records nothing.
    rig.click(picker);
    rig.frame();
    Expect(picker->isShowing(), "a click opens the chooser");
    Pick(*picker, 0, 255, 0);
    rig.frame();
    Expect(SameColor(a->tint(), 0.f, 1.f, 0.f), "the instance takes the color while the chooser is open");
    rig.key(jadefx::Key::Escape);
    rig.frame();
    Expect(!picker->isShowing() && SameColor(a->tint(), 0.f, 0.f, 1.f), "Escape puts the color back");
    Expect(picker->getValue().toHex() == "#0000ff", "and shows the color again");
    Expect(rig.undoDepth() == 0, "and records nothing");

    // The instance follows every color the drag passes through; the history hears of none of them.
    rig.click(picker);
    rig.frame();
    Expect(picker->isShowing(), "a click opens it again");
    Pick(*picker, 255, 0, 255);
    rig.frame();
    Expect(SameColor(a->tint(), 1.f, 0.f, 1.f), "the instance follows the drag");
    Pick(*picker, 0, 255, 0);
    rig.frame();
    Expect(picker->getValue().toHex() == "#00ff00", "the picker follows the chooser");
    Expect(SameColor(a->tint(), 0.f, 1.f, 0.f), "and so does the instance");
    Expect(rig.undoDepth() == 0, "nothing is recorded while the chooser is open");
    // Enter keeps the new color, as one undo step from the color before the chooser opened.
    rig.key(jadefx::Key::Enter);
    rig.frame();
    Expect(SameColor(a->tint(), 0.f, 1.f, 0.f), "closing on a new color keeps it");
    Expect(rig.undoDepth() == 1, "one waypoint");
    rig.game.history().undo();
    rig.frame();
    Expect(SameColor(a->tint(), 0.f, 0.f, 1.f), "undo goes back to the color before the chooser opened");
    rig.game.history().redo();
    rig.frame();
    Expect(SameColor(a->tint(), 0.f, 1.f, 0.f), "and redo to the pick");
    Expect(picker->getValue().toHex() == "#00ff00", "which the picker shows");

    // Two instances that disagree are mixed, and a pick writes both.
    rig.select({first, second});
    Expect(rig.mixed("Tint"), "different colors are mixed");
    rig.click(picker);
    rig.frame();
    Pick(*picker, 255, 255, 0);
    rig.key(jadefx::Key::Enter);
    rig.frame();
    Expect(SameColor(a->tint(), 1.f, 1.f, 0.f) && SameColor(b->tint(), 1.f, 1.f, 0.f), "a mixed pick writes both");
    Expect(!rig.mixed("Tint") && picker->getValue().toHex() == "#ffff00", "and they agree after");

    // A mixed row keeps a color out of sight. Picking that same color still writes it.
    b->set_tint(engine_core::ColorRgb{1.f, 1.f, 1.f, 1.f});
    rig.game.history().reset_waypoints();
    rig.frame();
    Expect(rig.mixed("Tint"), "mixed again");
    const jadefx::Color hidden = picker->getValue();
    rig.click(picker);
    rig.frame();
    Pick(*picker, static_cast<int>(hidden.r * 255 + 0.5f), static_cast<int>(hidden.g * 255 + 0.5f),
         static_cast<int>(hidden.b * 255 + 0.5f) == 0 ? 1 : 0);
    Pick(*picker, static_cast<int>(hidden.r * 255 + 0.5f), static_cast<int>(hidden.g * 255 + 0.5f),
         static_cast<int>(hidden.b * 255 + 0.5f));
    rig.key(jadefx::Key::Enter);
    rig.frame();
    Expect(!rig.mixed("Tint") && SameColor(b->tint(), hidden.r, hidden.g, hidden.b),
           "picking the hidden color of a mixed row writes it");
    Expect(rig.undoDepth() == 1, "as one waypoint");

    // Opening and closing with no pick writes nothing, even over a mixed row.
    b->set_tint(engine_core::ColorRgb{1.f, 1.f, 1.f, 1.f});
    rig.game.history().reset_waypoints();
    rig.frame();
    rig.click(picker);
    rig.frame();
    rig.clickAway();
    Expect(!picker->isShowing() && rig.mixed("Tint") && rig.undoDepth() == 0, "a look inside writes nothing");

    // A drag over a mixed row shows on both; Escape gives each its own color back.
    const engine_core::ColorRgb own_a = a->tint();
    rig.click(picker);
    rig.frame();
    // Neither instance holds blue, so the pick is a change for both.
    Pick(*picker, 0, 0, 255);
    rig.frame();
    Expect(!SameColor(own_a, 0.f, 0.f, 1.f), "the first instance did not already hold blue");
    Expect(SameColor(a->tint(), 0.f, 0.f, 1.f) && SameColor(b->tint(), 0.f, 0.f, 1.f),
           "a drag over a mixed row shows on every instance");
    rig.key(jadefx::Key::Escape);
    rig.frame();
    Expect(SameColor(a->tint(), own_a.r, own_a.g, own_a.b) && SameColor(b->tint(), 1.f, 1.f, 1.f),
           "Escape gives each its own color back");
    Expect(rig.mixed("Tint") && rig.undoDepth() == 0, "still mixed, and nothing recorded");

    // A chooser open when the selection changes closes, and the color lands on what was selected.
    rig.click(picker);
    rig.frame();
    Pick(*picker, 0, 255, 255);
    rig.select({second});
    Expect(!picker->isShowing(), "a new selection closes the chooser");
    Expect(SameColor(a->tint(), 0.f, 1.f, 1.f) && SameColor(b->tint(), 0.f, 1.f, 1.f),
           "and the color lands on what was selected");
    rig.click(picker);
    rig.frame();
    Pick(*picker, 255, 0, 255);
    rig.select({rig.script});
    Expect(rig.panel.editor("Tint") == nullptr && SameColor(b->tint(), 1.f, 0.f, 1.f),
           "a row that goes away keeps its pick too");
}

void TestR6ParentReference() {
    Rig rig;
    rig.select({rig.a});
    const std::string root = rig.game.name(rig.game.scene_service("Workspace"));
    Expect(rig.text("Parent") == root, "R6 the Parent shows its Name alone");
    Expect(rig.panel.editor("Parent", 2) == nullptr, "R6 the Name is the Pick button, next to Clear");

    // Clicking the Name picks: the next explorer click is the value, and the selection comes back.
    rig.click(rig.panel.editor("Parent", 0));
    rig.frame();
    Expect(rig.panel.picking() && rig.panel.pick_property() == "Parent", "R6 Pick waits for a click");
    Expect(rig.panel.status() == "Click an instance in an explorer or the Assets pane to set Parent. Click Parent again to cancel.",
           "R6 the status line says where to click");
    rig.clickExplorer("Stuff");
    Expect(!rig.panel.picking(), "R6 the click ends the pick");
    Expect(rig.game.parent(rig.a) == rig.folder, "R6 the picked instance is the Parent");
    Expect(rig.game.selection().get() == std::vector<InstanceId>{rig.a}, "R6 the selection comes back");
    rig.frame();
    Expect(rig.text("Parent") == "Stuff", "R6 the row shows the new Parent");
    Expect(rig.game.history().can_undo().second == "Set Parent", "R6 a pick is one Set Parent");

    // A cycle is refused: the folder cannot go under its own child.
    rig.select({rig.folder});
    rig.click(rig.panel.editor("Parent", 0));
    rig.frame();
    rig.clickExplorer("A");
    Expect(rig.game.parent(rig.folder) == rig.game.scene_service("Workspace"), "R6 a descendant cannot be the Parent");
    rig.frame();
    Expect(!rig.panel.status().empty(), "R6 the refusal is shown");
    // And an instance cannot be its own Parent.
    Expect(!ide::apply_edit(rig.game, {rig.folder}, {"Parent", ide::PropertyKind::Ref, {"", false, 0, {}, rig.folder}, -1})
                .written,
           "R6 self is refused");

    // Mixed parents are blank until a pick sets them all.
    rig.select({rig.a, rig.b});
    Expect(rig.mixed("Parent") && rig.text("Parent").empty(), "R6 mixed parents are blank");
    rig.click(rig.panel.editor("Parent", 0));
    rig.frame();
    rig.clickExplorer("Stuff");
    Expect(rig.game.parent(rig.a) == rig.folder && rig.game.parent(rig.b) == rig.folder, "R6 a pick sets both");
    rig.frame();
    Expect(!rig.mixed("Parent") && rig.text("Parent") == "Stuff", "R6 the row agrees");

    // Clear sets nil on every selected instance.
    rig.click(rig.panel.editor("Parent", 1));
    rig.frame();
    Expect(rig.game.parent(rig.a) == DataModel::kNoParent && rig.game.parent(rig.b) == DataModel::kNoParent,
           "R6 Clear sets nil");
    Expect(rig.text("Parent").empty(), "R6 nil shows empty");

    // Pick again, then Pick a second time cancels.
    rig.click(rig.panel.editor("Parent", 0));
    rig.frame();
    rig.click(rig.panel.editor("Parent", 0));
    rig.frame();
    Expect(!rig.panel.picking(), "R6 a second click on the Name cancels");
}

void TestR7MidEditIsNotClobbered() {
    Rig rig;
    rig.select({rig.a});
    rig.typeInto("Name", "Typed");
    rig.frame();
    rig.game.set_name(rig.a, "Outside");
    rig.frame();
    Expect(rig.text("Name") == "Typed", "R7 an outside write does not overwrite typing");
    rig.key(jadefx::Key::Escape);
    rig.frame();
    Expect(rig.text("Name") == "Outside", "R7 Escape resyncs to the world");
    Expect(rig.game.name(rig.a) == "Outside", "R7 Escape writes nothing");

    rig.typeInto("Name", "Mine");
    rig.game.set_name(rig.a, "Again");
    rig.frame();
    Expect(rig.text("Name") == "Mine", "R7 still not overwritten");
    rig.enter();
    Expect(rig.game.name(rig.a) == "Mine" && rig.text("Name") == "Mine", "R7 commit writes and resyncs");

    // Not focused: an outside write shows at once, and agreement is live.
    rig.clickAway();
    rig.select({rig.a, rig.b});
    Expect(rig.text("Name").empty(), "R7 differing Names are blank");
    rig.game.set_name(rig.b, "Mine");
    rig.frame();
    Expect(rig.text("Name") == "Mine", "R7 Names that come to agree show");
    rig.game.set_name(rig.b, "Other");
    rig.frame();
    Expect(rig.text("Name").empty(), "R7 and snap back to blank when they differ");
}

void TestR8NoSelection() {
    Rig rig;
    rig.select({});
    Expect(rig.panel.sheet().rows.empty(), "R8 no rows");
    bool shown = false;
    for (jadefx::Node* node : rig.panel.dock_widget()->getElementsByClassName("properties-empty")) {
        auto* label = dynamic_cast<jadefx::Label*>(node);
        shown = shown || (label != nullptr && label->isVisible() && label->getText() == "No selection");
    }
    Expect(shown, "R8 the page says No selection");
    rig.select({0x7fff1234u});
    Expect(rig.panel.sheet().ids.empty(), "R8 a dead id is no selection");
}

void TestR9DestroyedLeavesIntersection() {
    Rig rig;
    rig.select({rig.a, rig.b, rig.script});
    Expect(!rig.hasRow("Transform"), "R9 a Script hides Transform");
    rig.game.destroy(rig.script);
    rig.frame();
    Expect(rig.panel.sheet().ids.size() == 2, "R9 the destroyed instance leaves");
    Expect(rig.hasRow("Transform"), "R9 the intersection widens again");
}

void TestBooleanAndNumber() {
    RegisterProbe();
    Rig rig;
    Probe& p1 = rig.game.create<Probe>();
    Probe& p2 = rig.game.create<Probe>();
    rig.game.set_parent(p1.id(), rig.game.scene_service("Workspace"));
    rig.game.set_parent(p2.id(), rig.game.scene_service("Workspace"));
    p1.flag = true;
    p1.speed = 1.5;
    p2.speed = 1.5;
    rig.game.history().reset_waypoints();
    rig.select({p1.id(), p2.id()});
    Expect(rig.hasRow("Flag") && rig.hasRow("Speed"), "registered properties show without panel changes");
    auto* check = dynamic_cast<jadefx::CheckBox*>(rig.panel.editor("Flag"));
    Expect(check != nullptr && check->isIndeterminate() && !check->isSelected(), "mixed boolean is indeterminate");
    rig.click(check);
    rig.frame();
    Expect(p1.flag && p2.flag, "a click on a mixed box sets every one");
    Expect(check != nullptr && check->isSelected() && !check->isIndeterminate(), "the box now shows checked");

    Expect(rig.text("Speed") == "1.5", "an equal number shows");
    rig.typeInto("Speed", "abc");
    rig.enter();
    Expect(p1.speed == 1.5 && p2.speed == 1.5, "a bad number writes nothing");
    rig.typeInto("Speed", " 2.25 ");
    rig.enter();
    Expect(p1.speed == 2.25 && p2.speed == 2.25, "a number is parsed on commit");
    Expect(rig.text("Speed") == "2.25", "the field shows it");

    // Read-side unit checks on the pure model.
    std::vector<InstanceId> ids{p1.id(), p2.id()};
    p2.speed = 3;
    const ide::PropertySheet sheet = ide::read_sheet(rig.game, ids);
    Expect(sheet.find("Archivable") == nullptr, "the panel never lists Archivable");
    Expect(ide::read_sheet(rig.game, ids, true).find("Archivable") != nullptr, "MCP still reads Archivable");
    const ide::PropertyRow* speed = sheet.find("Speed");
    Expect(speed != nullptr && speed->mixed && speed->value.number == 0, "a mixed number hides the first value");
    Expect(sheet.rows.size() >= 3 && sheet.rows[0].name == "Name" && sheet.rows[1].name == "Parent" &&
               sheet.rows[2].name == "ClassName",
           "Instance rows lead in a fixed order");
}

// Enabled is Script's. A ModuleScript runs only through require and has none,
// so a selection with one in it shows no Enabled row.
void TestModuleScriptHasNoEnabled() {
    Rig rig;
    const InstanceId module = rig.add<engine_core::ModuleScript>("Mod");
    const ide::PropertySheet script_sheet = ide::read_sheet(rig.game, {rig.script});
    Expect(script_sheet.find("Enabled") != nullptr, "a Script shows Enabled");
    const ide::PropertySheet module_sheet = ide::read_sheet(rig.game, {module});
    Expect(module_sheet.find("Enabled") == nullptr, "a ModuleScript has no Enabled");
    Expect(module_sheet.find("ClassName") != nullptr && module_sheet.find("ClassName")->value.text == "ModuleScript",
           "the ModuleScript sheet is its own");
    const ide::PropertySheet both = ide::read_sheet(rig.game, {rig.script, module});
    Expect(both.find("Enabled") == nullptr, "Script and ModuleScript share no Enabled");
}

void TestMixedEnabledIsOneWaypoint() {
    Rig rig;
    const InstanceId other = rig.add<engine_core::Script>("Other");
    auto* first = dynamic_cast<engine_core::Script*>(rig.game.instance(rig.script));
    auto* second = dynamic_cast<engine_core::Script*>(rig.game.instance(other));
    second->set_enabled(false);
    rig.game.history().reset_waypoints();
    rig.select({rig.script, other});
    auto* check = dynamic_cast<jadefx::CheckBox*>(rig.panel.editor("Enabled"));
    Expect(check != nullptr && check->isIndeterminate() && !check->isSelected(),
           "mixed Enabled is indeterminate, not false");
    rig.click(check);
    rig.frame();
    Expect(first->enabled() && second->enabled(), "a click enables both");
    Expect(rig.game.history().can_undo().second == "Set Enabled" && rig.undoDepth() == 1,
           "the click is one Set Enabled waypoint");
    rig.game.history().undo();
    rig.frame();
    Expect(first->enabled() && !second->enabled(), "one undo puts both back");
    Expect(check != nullptr && check->isIndeterminate(), "and the box is mixed again");
}

void TestFieldUndoThenPlace() {
    Rig rig;
    rig.select({rig.a});
    rig.typeInto("Name", "Q");
    Expect(rig.panel.field_undo(false), "the field's own typing undoes first");
    Expect(rig.text("Name") == "A", "undo restores the field text");
    Expect(rig.panel.field_undo(true), "redo is the field's too");
    Expect(rig.text("Name") == "Q", "redo brings the typing back");
    rig.panel.field_undo(false);
    Expect(!rig.panel.field_undo(false), "a spent field hands undo to the place");
    rig.enter();
    Expect(rig.game.name(rig.a) == "A" && !rig.game.history().can_undo().first,
           "undoing back to the start is not an edit");
    rig.clickAway();
    Expect(!rig.panel.field_undo(false), "an unfocused page does not take the chord");
}

void TestPlayEditIsUndoable() {
    Rig rig;
    rig.game.capture_place();
    rig.game.start_simulation();
    rig.select({rig.a, rig.b});
    rig.typeInto("Name", "Play");
    rig.enter();
    Expect(rig.game.name(rig.a) == "Play" && rig.game.name(rig.b) == "Play", "a play edit writes both");
    Expect(rig.game.history().can_undo().first, "a play edit is recorded");
    rig.game.history().undo();
    rig.frame();
    Expect(rig.game.name(rig.a) == "A" && rig.game.name(rig.b) == "B", "and undoes in the session");
    rig.game.stop_simulation();
}

// Built directly by class, not through lua_create_instance: that name maps to
// a factory registered in ScriptBindings.cpp, which only this test's small
// binary has no other reason to link in.
InstanceId MakeAsset(Game& game, const char* klass, const char* name, InstanceId parent) {
    const std::string_view kind = klass;
    DataModel* object = nullptr;
    if (kind == "Texture") {
        object = &game.create<engine_core::Texture>();
    } else if (kind == "Mesh") {
        object = &game.create<engine_core::Mesh>();
    } else if (kind == "Material") {
        object = &game.create<engine_core::Material>();
    } else if (kind == "Sound") {
        object = &game.create<engine_core::Sound>();
    }
    game.set_name(object->id(), name);
    game.set_parent(object->id(), parent);
    return object->id();
}

const ide::PropertyRow* RowNamed(const ide::PropertySheet& sheet, const char* name) {
    for (const ide::PropertyRow& row : sheet.rows) {
        if (row.name == name) {
            return &row;
        }
    }
    return nullptr;
}

ide::PropertyEdit RefEdit(const char* property, InstanceId id) {
    return {property, ide::PropertyKind::Ref, {"", false, 0, {}, id}, -1};
}

void TestReferenceRows() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    Game game;
    const InstanceId brick = MakeAsset(game, "Texture", "Brick", game.service("Textures"));
    const InstanceId rock = MakeAsset(game, "Mesh", "Rock", game.service("Meshes"));
    const InstanceId wall = MakeAsset(game, "Material", "Wall", game.service("Materials"));

    const ide::PropertySheet before = ide::read_sheet(game, {wall});
    const ide::PropertyRow* row = RowNamed(before, "DiffuseTexture");
    Expect(row != nullptr && row->kind == ide::PropertyKind::Ref, "a reference is a Ref row");
    Expect(row != nullptr && row->writable && row->value.nil_ref(), "it starts nil and writable");

    const ide::EditResult set = ide::apply_edit(game, {wall}, RefEdit("DiffuseTexture", brick));
    Expect(set.written == 1 && !set.rejected, "a Texture is taken");
    const ide::PropertySheet after = ide::read_sheet(game, {wall});
    row = RowNamed(after, "DiffuseTexture");
    Expect(row != nullptr && row->label == "Brick", "the row shows the Texture's name");
    Expect(row != nullptr && row->path == "Game.Assets.Textures.Brick", "and its path on hover");

    const ide::EditResult wrong = ide::apply_edit(game, {wall}, RefEdit("DiffuseTexture", rock));
    Expect(wrong.rejected && wrong.error == "DiffuseTexture must be a Texture", "a Mesh is refused, saying why");
    Expect(wrong.written == 0, "a refusal writes nothing");

    const ide::EditResult cleared =
        ide::apply_edit(game, {wall}, RefEdit("DiffuseTexture", engine_core::DataModel::kNoParent));
    Expect(cleared.written == 1, "nil clears");
    // Kept in a local: row points into the sheet, which must outlive the check.
    const ide::PropertySheet cleared_sheet = ide::read_sheet(game, {wall});
    row = RowNamed(cleared_sheet, "DiffuseTexture");
    Expect(row != nullptr && row->value.nil_ref(), "the row is nil again");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

double Reflectivity(Game& game, InstanceId id) {
    auto* material = dynamic_cast<engine_core::Material*>(game.instance(id));
    return material != nullptr ? material->reflectivity() : -1;
}

// Material's Reflectivity and Transparency, and Camera's FieldOfView, register
// a range, so each is a slider beside its field.
void TestSliderRows() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    Rig rig;
    const InstanceId wall = MakeAsset(rig.game, "Material", "Wall", rig.game.service("Materials"));
    const InstanceId floor = MakeAsset(rig.game, "Material", "Floor", rig.game.service("Materials"));
    const InstanceId camera = rig.add<engine_core::Camera>("Cam");
    rig.game.history().reset_waypoints();

    const ide::PropertySheet sheet = ide::read_sheet(rig.game, {wall});
    const ide::PropertyRow* reflectivity = RowNamed(sheet, "Reflectivity");
    Expect(reflectivity != nullptr && reflectivity->slider() && reflectivity->slider_min == 0 &&
               reflectivity->slider_max == 1 && reflectivity->value.number == 0.5,
           "Reflectivity is a 0..1 slider at 0.5");
    const ide::PropertyRow* transparency = RowNamed(sheet, "Transparency");
    Expect(transparency != nullptr && transparency->slider() && transparency->value.number == 0,
           "Transparency is a slider at 0");
    const ide::PropertyRow* color = RowNamed(sheet, "Color");
    Expect(color != nullptr && color->kind == ide::PropertyKind::Color3 && color->value.color.r == 1 &&
               color->value.color.g == 1 && color->value.color.b == 1,
           "Color is a white Color3");
    const ide::PropertySheet camera_sheet = ide::read_sheet(rig.game, {camera});
    const ide::PropertyRow* fov = RowNamed(camera_sheet, "FieldOfView");
    Expect(fov != nullptr && fov->slider() && fov->slider_min == engine_core::Camera::kMinFieldOfView &&
               fov->slider_max == engine_core::Camera::kMaxFieldOfView,
           "FieldOfView is a slider over its own range");
    const ide::PropertyRow* name = RowNamed(camera_sheet, "Name");
    Expect(name != nullptr && !name->slider(), "a row without a range is no slider");

    rig.select({wall, floor});
    auto* slider = dynamic_cast<jadefx::Slider*>(rig.panel.editor("Reflectivity", 1));
    Expect(slider != nullptr && slider->isVisible(), "the slider is on screen");
    Expect(rig.text("Reflectivity") == "0.5", "the field beside it shows the value");
    Expect(slider != nullptr && slider->getValue() == 0.5, "and so does the thumb");

    // The field goes past the end; the thumb stops there.
    rig.typeInto("Reflectivity", "2");
    rig.enter();
    Expect(Reflectivity(rig.game, wall) == 2 && Reflectivity(rig.game, floor) == 2,
           "a typed number past the end writes every one");
    Expect(slider != nullptr && slider->getValue() == 1, "the thumb rests at the end");
    Expect(rig.undoDepth() == 1, "the typed number is one undo step");

    // A click on the middle of the track jumps there and writes on release, rounded.
    rig.click(slider);
    rig.frame();
    const double clicked = Reflectivity(rig.game, wall);
    Expect(clicked > 0.4 && clicked < 0.6 && clicked == std::round(clicked * 100) / 100,
           "a track click writes where it landed, to a hundredth");
    Expect(Reflectivity(rig.game, floor) == clicked, "to every selected one");
    Expect(rig.text("Reflectivity") == ide::format_number(clicked), "the field follows the thumb");
    Expect(rig.undoDepth() == 2, "the click is one more undo step");

    // A press that does not move the thumb writes nothing.
    rig.click(slider);
    rig.frame();
    Expect(rig.undoDepth() == 2, "a click where the thumb is writes nothing");

    // A drag shows on the instances as it goes, and is one undo step from where it began.
    if (slider != nullptr) {
        const double before = Reflectivity(rig.game, wall);
        const double y = slider->getAbsoluteY() + slider->getHeight() * 0.5;
        const double left = slider->getAbsoluteX();
        const double width = slider->getWidth();
        rig.scene->noteButton(0, true, left + width * 0.5, y);
        rig.scene->noteMove(left + width * 0.8, y);
        rig.frame();
        const double dragged = Reflectivity(rig.game, wall);
        Expect(dragged > before + 0.1 && Reflectivity(rig.game, floor) == dragged,
               "the instances follow the thumb while it is held");
        Expect(rig.undoDepth() == 2, "and nothing is recorded yet");
        rig.scene->noteMove(left + width * 0.2, y);
        rig.frame();
        const double released = Reflectivity(rig.game, wall);
        Expect(released < before - 0.1, "they follow it back the other way");
        rig.scene->noteButton(0, false, left + width * 0.2, y);
        rig.frame();
        Expect(Reflectivity(rig.game, wall) == released, "letting go keeps where it stopped");
        Expect(rig.undoDepth() == 3, "as one undo step");
        rig.game.history().undo();
        rig.frame();
        Expect(Reflectivity(rig.game, wall) == before && Reflectivity(rig.game, floor) == before,
               "undo goes back to where the drag began");
        rig.game.history().redo();
        rig.frame();
        Expect(Reflectivity(rig.game, wall) == released, "and redo to where it stopped");

        // A drag that comes back to where it began records nothing.
        rig.scene->noteButton(0, true, left + width * 0.2, y);
        rig.scene->noteMove(left + width * 0.7, y);
        rig.frame();
        rig.scene->noteMove(left + width * 0.2, y);
        rig.scene->noteButton(0, false, left + width * 0.2, y);
        rig.frame();
        Expect(Reflectivity(rig.game, wall) == released && rig.undoDepth() == 3,
               "a drag back to where it began writes nothing");
    }

    // An arrow key moves it a twentieth and writes at once.
    const double keyed = Reflectivity(rig.game, wall);
    const int depth = rig.undoDepth();
    rig.key(jadefx::Key::Right);
    rig.frame();
    Expect(std::fabs(Reflectivity(rig.game, wall) - (keyed + 0.05)) < 1e-9, "Right moves up a twentieth");
    Expect(rig.undoDepth() == depth + 1, "each key is its own step");

    // Mixed values rest the thumb at the start and blank the field.
    auto* floor_material = dynamic_cast<engine_core::Material*>(rig.game.instance(floor));
    floor_material->set_reflectivity(0.1);
    rig.frame();
    Expect(rig.mixed("Reflectivity") && rig.text("Reflectivity").empty(), "a mixed slider row blanks its field");
    Expect(slider != nullptr && slider->getValue() == 0, "and rests its thumb at the start");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

void TestServiceRowsReadOnly() {
    Game game;
    const ide::PropertySheet sheet = ide::read_sheet(game, {game.service("Textures")});
    const ide::PropertyRow* name = RowNamed(sheet, "Name");
    const ide::PropertyRow* parent = RowNamed(sheet, "Parent");
    Expect(name != nullptr && !name->writable, "a game service's Name is read-only");
    Expect(parent != nullptr && !parent->writable, "a game service's Parent is read-only");
}

// instance_drag_ids never throws on malformed text: a drop is ignored rather
// than crashing the app.
void TestInstanceDragIds() {
    Expect(ide::instance_drag_text({3, 42}) == "3,42", "ids join with commas");
    Expect(ide::instance_drag_ids("3,42") == std::vector<InstanceId>{3, 42}, "and split back the same way");
    Expect(ide::instance_drag_ids("").empty(), "empty text is no ids");
    Expect(ide::instance_drag_ids("12,x").empty(), "a non-numeric part is ignored");
    Expect(ide::instance_drag_ids("abc").empty(), "non-numeric text is ignored");
    Expect(ide::instance_drag_ids("99999999999999999999").empty(), "an id too big to fit is ignored");
    Expect(ide::instance_drag_ids("7") == std::vector<InstanceId>{7}, "a single id parses");
    Expect(ide::instance_drag_ids("1,,2").empty(), "an empty part is ignored");
}

}  // namespace

// PhysicsObject's Shape is an Enum row, a dropdown of Enum.PhysicsShape's
// items, and its Mesh row shows only while Shape is Hull.
void TestEnumRowAndShownWhen() {
    Rig rig;
    const InstanceId body = rig.add<engine_core::PhysicsObject>("Body");
    const InstanceId other = rig.add<engine_core::PhysicsObject>("Other");
    rig.select({body});
    const ide::PropertyRow* shape = rig.panel.sheet().find("Shape");
    Expect(shape != nullptr && shape->kind == ide::PropertyKind::Enum, "Shape is an Enum row");
    Expect(shape != nullptr && shape->enum_type == &engine_core::physics_shape_enum(), "of Enum.PhysicsShape");
    Expect(shape != nullptr && shape->value.number == 0, "Box by default");
    auto* choice = dynamic_cast<jadefx::ComboBox*>(rig.panel.editor("Shape"));
    Expect(choice != nullptr, "an Enum row is a dropdown");
    Expect(choice != nullptr && choice->getItems().items() ==
                                    std::vector<std::string>{"Box", "Sphere", "Capsule", "Hull", "Custom",
                                                             "Cylinder", "Cone", "Wedge"},
           "listing the items in value order");
    Expect(choice != nullptr && choice->getSelectionIndex() == 0, "Box is picked");
    Expect(!rig.hasRow("Mesh"), "a Box has no Mesh row");
    Expect(ide::read_sheet(rig.game, {body}, true).find("Mesh") != nullptr, "MCP still sees the hidden row");

    ide::PropertyEdit edit;
    edit.property = "Shape";
    edit.kind = ide::PropertyKind::Enum;
    edit.value.number = 3;
    const ide::EditResult result = ide::apply_edit(rig.game, {body}, edit);
    Expect(result.written == 1 && !result.rejected, "a pick writes the item");
    rig.frame();
    rig.frame();
    Expect(rig.hasRow("Mesh"), "a Hull shows its Mesh row");
    choice = dynamic_cast<jadefx::ComboBox*>(rig.panel.editor("Shape"));
    Expect(choice != nullptr && choice->getSelectionIndex() == 3, "the dropdown follows the write");

    edit.value.number = 4;
    ide::apply_edit(rig.game, {body}, edit);
    rig.frame();
    rig.frame();
    Expect(rig.hasRow("Mesh"), "a Custom shows its Mesh row too");
    edit.value.number = 3;
    ide::apply_edit(rig.game, {body}, edit);
    rig.frame();

    edit.value.number = 9;
    const ide::EditResult refused = ide::apply_edit(rig.game, {body}, edit);
    Expect(refused.written == 0, "a value with no item is refused");

    // Two bodies, one a Hull: Shape is mixed, and Mesh shows only if both are Hulls.
    rig.select({body, other});
    Expect(rig.mixed("Shape"), "different Shapes are mixed");
    Expect(!rig.hasRow("Mesh"), "Mesh hides unless every selected body is a Hull");
}

InstanceId BodyMesh(Game& game, InstanceId body) {
    auto* object = dynamic_cast<engine_core::PhysicsObject*>(game.instance(body));
    return object != nullptr ? object->mesh().id : 0;
}

// A reference to an asset class, as a PhysicsObject's Mesh, opens the asset
// picker the Prefab editor's slots use, not a pick from the selection.
void TestAssetReferencePicker() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    Rig rig;
    const InstanceId body = rig.add<engine_core::PhysicsObject>("Body");
    const InstanceId rock = MakeAsset(rig.game, "Mesh", "Rock", rig.game.service("Meshes"));
    const InstanceId lid = MakeAsset(rig.game, "Mesh", "Lid", rig.game.service("Meshes"));
    MakeAsset(rig.game, "Material", "Wall", rig.game.service("Materials"));
    ide::PropertyEdit hull;
    hull.property = "Shape";
    hull.kind = ide::PropertyKind::Enum;
    hull.value.number = 3;
    ide::apply_edit(rig.game, {body}, hull);
    rig.game.history().reset_waypoints();
    rig.select({body});
    rig.frame();

    rig.click(rig.panel.editor("Mesh", 0));
    rig.frame();
    Expect(rig.panel.asset_picking() && !rig.panel.picking(), "Mesh opens the asset picker, not a selection pick");
    Expect(rig.panel.asset_pick_row(rock) != nullptr && rig.panel.asset_pick_row(lid) != nullptr,
           "listing every Mesh");
    Expect(rig.panel.asset_pick_row(0) == nullptr, "and no None while Mesh is nil");

    rig.click(rig.panel.asset_pick_row(rock));
    rig.frame();
    rig.frame();
    Expect(!rig.panel.asset_picking(), "a pick closes it");
    Expect(BodyMesh(rig.game, body) == rock, "and writes the Mesh");
    Expect(rig.text("Mesh") == "Rock", "which the row shows");
    Expect(rig.undoDepth() == 1, "as one undo step");

    rig.click(rig.panel.editor("Mesh", 0));
    rig.frame();
    Expect(rig.panel.asset_pick_row(0) != nullptr, "a set Mesh offers None");
    rig.panel.asset_pick_field()->setText("li");
    rig.frame();
    Expect(rig.panel.asset_pick_row(rock) == nullptr && rig.panel.asset_pick_row(lid) != nullptr,
           "typing filters the list");
    rig.panel.asset_pick_field()->setText("");
    rig.frame();
    rig.click(rig.panel.asset_pick_row(0));
    rig.frame();
    rig.frame();
    Expect(BodyMesh(rig.game, body) == 0, "None clears the Mesh");

    // A reference to a class that is not an asset still picks from the selection.
    rig.click(rig.panel.editor("GameObject", 0));
    rig.frame();
    Expect(rig.panel.picking() && !rig.panel.asset_picking(), "GameObject picks from the selection");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

// A single Texture, Material, or Sound has a Preview section; a Sound's is a
// Play button. Anything else, or more than one, has none.
void TestAssetPreview() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    Rig rig;
    const InstanceId brick = MakeAsset(rig.game, "Texture", "Brick", rig.game.service("Textures"));
    const InstanceId wall = MakeAsset(rig.game, "Material", "Wall", rig.game.service("Materials"));
    const InstanceId boom = MakeAsset(rig.game, "Sound", "Boom", rig.game.service("Audio"));
    rig.frame();

    rig.select({brick});
    Expect(rig.panel.preview_class() == "Texture", "a Texture has a preview");
    Expect(rig.panel.sound_control(0) == nullptr, "with no transport");
    rig.select({wall});
    Expect(rig.panel.preview_class() == "Material", "a Material has a preview");
    rig.select({boom});
    rig.frame();
    Expect(rig.panel.preview_class() == "Sound", "a Sound has a preview");
    const char* names[4] = {"Play", "Pause", "Resume", "Stop"};
    for (int part = 0; part < 4; ++part) {
        auto* button = dynamic_cast<jadefx::Button*>(rig.panel.sound_control(part));
        Expect(button != nullptr && button->isVisible() && button->getText() == names[part],
               "Play, Pause, Resume, and Stop, in order");
        // Stopped, with no Path: nothing applies.
        Expect(button != nullptr && button->isDisabled(), "each disabled while there is nothing to play");
    }
    jadefx::Node* track = rig.panel.sound_control(4);
    auto* time = dynamic_cast<jadefx::Label*>(rig.panel.sound_control(5));
    Expect(track != nullptr && track->isVisible() && track->isDisabled(), "a track, disabled while stopped");
    Expect(time != nullptr && time->getText() == "0:00 / 0:00", "and the time beside it");
    // A SoundEmitter has the same transport, for its Sound.
    const InstanceId speaker = rig.add<engine_core::SoundEmitter>("Speaker");
    rig.select({speaker});
    rig.frame();
    Expect(rig.panel.preview_class() == "Sound", "a SoundEmitter has a Sound's preview");
    auto* emitter_play = dynamic_cast<jadefx::Button*>(rig.panel.sound_control(0));
    Expect(emitter_play != nullptr && emitter_play->isVisible() && emitter_play->isDisabled(),
           "with Play disabled while it has no Sound");
    ide::apply_edit(rig.game, {speaker}, RefEdit("Sound", boom));
    rig.frame();
    Expect(rig.panel.preview_class() == "Sound", "and still once it has one");
    rig.select({brick, wall});
    Expect(rig.panel.preview_class().empty(), "two assets have no preview");
    rig.select({rig.a});
    rig.frame();
    Expect(rig.panel.preview_class().empty(), "nor does a GameObject");
    Expect(track != nullptr && !track->isVisible(), "and the transport goes");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

// Two seconds of silence as a 16-bit mono WAV, for the preview to play.
void WriteSilentWav(const std::filesystem::path& file) {
    const std::uint32_t rate = 8000;
    const std::uint32_t bytes = rate * 2 * 2;
    std::ofstream out(file, std::ios::binary);
    auto u32 = [&out](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&out](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(bytes);
    const std::vector<char> silence(bytes, 0);
    out.write(silence.data(), silence.size());
}

// Deleting what the preview plays, a Sound, an emitter's Sound, or the
// emitter, stops it, and so does stop_sound, which a test starting or
// stopping calls. Skipped where no audio device opens.
void TestSoundPreviewStops() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    Rig rig;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "anarchy-properties-sound";
    std::filesystem::create_directories(root);
    WriteSilentWav(root / "hush.wav");
    rig.game.set_resources_root(root);
    auto make_sound = [&rig](const char* name) {
        const InstanceId id = MakeAsset(rig.game, "Sound", name, rig.game.service("Audio"));
        dynamic_cast<engine_core::Sound*>(rig.game.instance(id))->set_path("hush.wav");
        return id;
    };
    const InstanceId alone = make_sound("Alone");
    const InstanceId heard = make_sound("Heard");
    const InstanceId kept = make_sound("Kept");
    const InstanceId speaker = rig.add<engine_core::SoundEmitter>("Speaker");
    const InstanceId other = rig.add<engine_core::SoundEmitter>("Other");
    ide::apply_edit(rig.game, {speaker}, RefEdit("Sound", heard));
    ide::apply_edit(rig.game, {other}, RefEdit("Sound", kept));

    // A Sound's TimeLength is a read-only row, read from its file.
    const ide::PropertySheet sheet = ide::read_sheet(rig.game, {alone});
    const ide::PropertyRow* length = RowNamed(sheet, "TimeLength");
    Expect(length != nullptr && !length->writable, "a Sound shows TimeLength, read-only");
    Expect(length != nullptr && std::fabs(length->value.number - 2.0) < 0.01, "as its file's two seconds");

    auto play = [&rig](InstanceId id) {
        rig.select({id});
        rig.frame();
        rig.click(rig.panel.sound_control(0));
        rig.frame();
        return rig.panel.sound_live();
    };
    if (!play(alone)) {
        std::fprintf(stderr, "skip TestSoundPreviewStops: no audio device (%s)\n", rig.panel.status().c_str());
        engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
        return;
    }
    rig.game.destroy(alone);
    rig.frame();
    Expect(!rig.panel.sound_live(), "deleting the Sound playing stops it");

    Expect(play(speaker), "an emitter's Sound plays");
    rig.game.destroy(heard);
    rig.frame();
    Expect(!rig.panel.sound_live(), "deleting the emitter's Sound stops it");

    Expect(play(other), "another emitter's Sound plays");
    rig.game.destroy(other);
    rig.frame();
    Expect(!rig.panel.sound_live(), "deleting the emitter stops it");

    Expect(play(kept), "a Sound plays");
    rig.panel.stop_sound();
    rig.frame();
    Expect(!rig.panel.sound_live(), "stop_sound stops it, as Test and Stop do");
    auto* stop = dynamic_cast<jadefx::Button*>(rig.panel.sound_control(3));
    Expect(stop != nullptr && stop->isDisabled(), "and the transport reads stopped");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

// Rows taller than the page scroll in a ScrollPane: the wheel moves them, and
// tabbing to a field out of view scrolls just far enough to show it.
void TestRowsScroll() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    Rig rig;
    const InstanceId wall = MakeAsset(rig.game, "Material", "Wall", rig.game.service("Materials"));
    rig.select({wall});
    auto frame = [&rig] {
        rig.clock += 0.05;
        rig.scene->layout(kWidth, 220, rig.clock);
    };
    frame();
    frame();
    jadefx::ScrollPane* scroll = rig.panel.scroll_pane();
    Expect(scroll != nullptr, "the rows are in a scroll pane");
    if (scroll == nullptr) {
        return;
    }
    Expect(scroll->getContentBounds().height > scroll->getViewportBounds().height + 1,
           "a Material's rows and preview run past a short page");
    Expect(scroll->getVvalue() == scroll->getVmin(), "which starts at the top");

    jadefx::Node* name = rig.panel.editor("Name");
    rig.scene->noteScroll(name->getAbsoluteX() + 4, name->getAbsoluteY() + 4, 0, -3);
    frame();
    Expect(scroll->getVvalue() > scroll->getVmin(), "the wheel scrolls it");

    scroll->setVvalue(scroll->getVmin());
    frame();
    rig.click(rig.field("Name"));
    frame();
    // Shift+Tab from the first field wraps to the last, at the bottom.
    rig.scene->noteKey(jadefx::Key::Tab, true, false, jadefx::Key::ModShift);
    frame();
    jadefx::TextField* last = rig.field("HeightStrength");
    Expect(last != nullptr && last->isFocused(), "Shift+Tab wraps to HeightStrength");
    Expect(scroll->getVvalue() > scroll->getVmin(), "and scrolls down to it");
    const double view_top = scroll->getAbsoluteY();
    const double view_bottom = view_top + scroll->getViewportBounds().height;
    Expect(last != nullptr && last->getAbsoluteY() >= view_top - 0.5 &&
               last->getAbsoluteY() + last->getHeight() <= view_bottom + 0.5,
           "showing it whole");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

// Clicking a category's header folds it: its rows go, what was typed in them
// is kept, Tab passes them over, and it stays folded for the next selection.
std::vector<std::string> RowOrder(const ide::PropertySheet& sheet) {
    std::vector<std::string> names;
    for (const ide::PropertyRow& row : sheet.rows) {
        names.push_back(row.group + ":" + row.name);
    }
    return names;
}

void TestSheetGroupOrder() {
    RegisterGroups();
    Rig rig;
    const InstanceId kid = rig.add<GroupKid>("Kid");
    const std::vector<std::string> expected = {
        "Instance:Name", "Instance:Parent", "Instance:ClassName", "Look:Zeta", "Look:Kappa",
        "Extra:Alpha",   "Extra:Beta",      "Feel:Mid",           "Data:Loose",
    };
    Expect(RowOrder(ide::read_sheet(rig.game, {kid})) == expected,
           "Instance, then groups by first appearance with base rows first, then Data");

    // Mixed classes: the first instance's groups, no repeated header.
    const InstanceId base = rig.add<GroupBase>("Base");
    const std::vector<std::string> mixed_expected = {
        "Instance:Name", "Instance:Parent", "Instance:ClassName", "Look:Zeta", "Look:Alpha", "Feel:Mid",
    };
    Expect(RowOrder(ide::read_sheet(rig.game, {base, kid})) == mixed_expected,
           "a mixed selection uses the first class's groups");
}

void TestGroupTitlesAndEmptyGroups() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    RegisterGroups();
    Rig rig;
    const InstanceId kid = rig.add<GroupKid>("Kid");
    const InstanceId base = rig.add<GroupBase>("Base");
    rig.select({kid});
    rig.frame();
    const std::vector<std::string> titles = {"Instance", "Look", "Extra", "Feel", "Data"};
    Expect(rig.panel.group_titles() == titles, "one header per group, in sheet order");

    rig.click(rig.panel.group_header("Look"));
    rig.frame();
    Expect(!rig.panel.editor("Zeta")->isVisible(), "a named group folds");
    rig.select({base, kid});
    rig.frame();
    const std::vector<std::string> mixed_titles = {"Instance", "Look", "Feel"};
    Expect(rig.panel.group_titles() == mixed_titles, "a group with no rows left has no header");
    Expect(!rig.panel.editor("Zeta")->isVisible(), "Look stays folded across selections");
    rig.select({kid});
    rig.frame();
    Expect(!rig.panel.editor("Kappa")->isVisible(), "and when coming back");
    rig.click(rig.panel.group_header("Look"));
    rig.frame();
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

// The sheet's group:name rows for one instance, without the Instance rows.
std::vector<std::string> GroupedRows(Rig& rig, InstanceId id) {
    std::vector<std::string> out;
    for (const ide::PropertyRow& row : ide::read_sheet(rig.game, {id}).rows) {
        if (row.group != "Instance") {
            out.push_back(row.group + ":" + row.name);
        }
    }
    return out;
}

bool Has(const std::vector<std::string>& rows, const char* row) {
    return std::find(rows.begin(), rows.end(), row) != rows.end();
}

void TestClassGroups() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    Rig rig;
    const std::vector<std::string> material = {
        "Surface:DiffuseTexture", "Surface:NormalTexture", "Surface:RoughnessTexture", "Surface:MetalnessTexture",
        "Modifier:Color", "Modifier:Transparency", "Modifier:Roughness", "Modifier:Metalness",
        "Modifier:Reflectivity", "Modifier:EmissiveTexture", "Modifier:Emissive",
        "Terrain:TextureScale", "Terrain:BlendSharpness", "Terrain:HeightTexture", "Terrain:HeightStrength",
    };
    Expect(GroupedRows(rig, MakeAsset(rig.game, "Material", "M", rig.game.service("Materials"))) == material,
           "Material's groups");

    const std::vector<std::string> point = GroupedRows(rig, rig.add<engine_core::PointLight>("Lamp"));
    Expect(point.size() >= 3 && point[0] == "Behavior:Prefab" && point[1] == "Behavior:Enabled" &&
               point[2] == "Behavior:Shadows",
           "a PointLight opens with Prefab, Enabled, Shadows");
    Expect(Has(point, "Light:Color"), "a light's own Color is in Light");

    const std::vector<std::string> brush = GroupedRows(rig, rig.add<engine_core::Brush>("Block"));
    Expect(Has(brush, "Physics:Anchored"), "Brush's Anchored, registered again, stays in Physics");
    for (const std::string& row : brush) {
        Expect(row.rfind("Data:", 0) != 0, "Brush has nothing left in Data");
    }

    const std::vector<std::string> pane = GroupedRows(rig, rig.add<engine_core::Pane>("Box"));
    Expect(Has(pane, "Layout:Size"), "Pane's Size stays in Layout");
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

void TestGroupsFold() {
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    Rig rig;
    const InstanceId wall = MakeAsset(rig.game, "Material", "Wall", rig.game.service("Materials"));
    const InstanceId floor = MakeAsset(rig.game, "Material", "Floor", rig.game.service("Materials"));
    rig.select({wall});
    rig.frame();
    jadefx::ScrollPane* scroll = rig.panel.scroll_pane();
    const double open_height = scroll->getContentBounds().height;
    Expect(rig.panel.editor("Roughness")->isVisible(), "Modifier's rows show");

    rig.typeInto("Roughness", "0.25");
    rig.click(rig.panel.group_header("Modifier"));
    rig.frame();
    rig.frame();
    Expect(!rig.panel.editor("Roughness")->isVisible() && !rig.panel.editor("Color")->isVisible(),
           "clicking Modifier folds its rows");
    Expect(rig.panel.editor("Name")->isVisible(), "and leaves Instance's");
    auto* material = dynamic_cast<engine_core::Material*>(rig.game.instance(wall));
    Expect(material != nullptr && std::fabs(material->roughness() - 0.25) < 1e-6, "keeping what was typed");
    Expect(scroll->getContentBounds().height < open_height, "the page gets shorter");

    rig.click(rig.field("Name"));
    rig.scene->noteKey(jadefx::Key::Tab, true, false, 0);
    rig.frame();
    Expect(rig.field("TextureScale") != nullptr && rig.field("TextureScale")->isFocused(),
           "Tab passes over the folded fields to Terrain's first");

    rig.select({floor});
    rig.frame();
    Expect(!rig.panel.editor("Roughness")->isVisible(), "another selection keeps Modifier folded");
    rig.click(rig.panel.group_header("Modifier"));
    rig.frame();
    Expect(rig.panel.editor("Roughness")->isVisible(), "and a second click opens it");

    const double with_preview = scroll->getContentBounds().height;
    rig.click(rig.panel.group_header("Preview"));
    rig.frame();
    rig.frame();
    Expect(scroll->getContentBounds().height < with_preview - 100, "the Preview folds too");
    rig.click(rig.panel.group_header("Preview"));
    rig.frame();
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
}

int main() {
    TestGroupMarkersRegister();
    TestSheetGroupOrder();
    TestGroupTitlesAndEmptyGroups();
    TestClassGroups();
    TestGroupsFold();
    TestRowsScroll();
    TestAssetPreview();
    TestSoundPreviewStops();
    TestEnumRowAndShownWhen();
    TestAssetReferencePicker();
    TestR1SingleSelectionEditsName();
    TestR2MixedNameWritesEveryone();
    TestMixedBlankIsNotEmptyName();
    TestR3IntersectionOnly();
    TestR4SameValueIsNotMixed();
    TestR5Vector3PerAxis();
    TestFocusSelectsWhole();
    TestTabWalksFields();
    TestPositionAxisColors();
    TestTransformRow();
    TestTransformOrientationKeepsScale();
    TestColorPicker();
    TestR6ParentReference();
    TestReferenceRows();
    TestSliderRows();
    TestServiceRowsReadOnly();
    TestInstanceDragIds();
    TestR7MidEditIsNotClobbered();
    TestR8NoSelection();
    TestR9DestroyedLeavesIntersection();
    TestBooleanAndNumber();
    TestMixedEnabledIsOneWaypoint();
    TestModuleScriptHasNoEnabled();
    TestFieldUndoThenPlace();
    TestPlayEditIsUndoable();
    if (gFailures == 0) {
        std::printf("properties tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failed\n", gFailures);
    return 1;
}
