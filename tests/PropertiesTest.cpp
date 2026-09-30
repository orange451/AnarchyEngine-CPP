#include "ide/IdeExplorer.hpp"
#include "ChangeHistoryService.hpp"
#include "ide/PropertiesPanel.hpp"
#include "ide/PropertySheet.hpp"
#include "SelectionService.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"
#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cstdio>
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
        game.history().end_gesture();
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
    Expect(rig.hasRow("Position"), "R1 a GameObject shows Position");
    Expect(!rig.hasRow("Source") && !rig.hasRow("Changed") && !rig.hasRow("Transform"),
           "R1 Source, signals, and unknown types have no row");

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
    Expect(!rig.hasRow("Position"), "R3 Position is not on a Script");
    Expect(!rig.hasRow("Enabled"), "R3 Enabled is not on a GameObject");
    Expect(rig.panel.editor("Position") == nullptr, "R3 no Position widget");
    // ClassName differs, so it is mixed and blank.
    Expect(rig.mixed("ClassName") && rig.text("ClassName").empty(), "R3 differing classes are mixed");

    // Two classes that each have a Position of the same type share its row.
    rig.select({rig.marker1, rig.a});
    Expect(rig.hasRow("Position"), "R3 a field both classes have, with one type, shows");
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

    // Escape puts the color back and writes nothing.
    rig.click(picker);
    rig.frame();
    Expect(picker->isShowing(), "a click opens the chooser");
    Pick(*picker, 0, 255, 0);
    rig.key(jadefx::Key::Escape);
    rig.frame();
    Expect(!picker->isShowing() && SameColor(a->tint(), 0.f, 0.f, 1.f), "Escape writes nothing");
    Expect(picker->getValue().toHex() == "#0000ff", "and shows the color again");

    // Enter keeps the new color, as one undo step.
    rig.click(picker);
    rig.frame();
    Expect(picker->isShowing(), "a click opens it again");
    Pick(*picker, 0, 255, 0);
    rig.frame();
    Expect(picker->getValue().toHex() == "#00ff00", "the picker follows the chooser");
    Expect(SameColor(a->tint(), 0.f, 0.f, 1.f), "the color is not written while the chooser is open");
    rig.key(jadefx::Key::Enter);
    rig.frame();
    Expect(SameColor(a->tint(), 0.f, 1.f, 0.f), "closing on a new color writes it");
    Expect(rig.undoDepth() == 1, "one waypoint");

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
    Expect(!rig.hasRow("Position"), "R9 a Script hides Position");
    rig.game.destroy(rig.script);
    rig.frame();
    Expect(rig.panel.sheet().ids.size() == 2, "R9 the destroyed instance leaves");
    Expect(rig.hasRow("Position"), "R9 the intersection widens again");
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

int main() {
    TestR1SingleSelectionEditsName();
    TestR2MixedNameWritesEveryone();
    TestMixedBlankIsNotEmptyName();
    TestR3IntersectionOnly();
    TestR4SameValueIsNotMixed();
    TestR5Vector3PerAxis();
    TestFocusSelectsWhole();
    TestTabWalksFields();
    TestPositionAxisColors();
    TestColorPicker();
    TestR6ParentReference();
    TestReferenceRows();
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
