#include "ide/IdeExplorer.hpp"
#include "ide/PropertiesPanel.hpp"
#include "ide/PropertySheet.hpp"

#include "DataModel.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "Script.hpp"
#include "TestTriangle.hpp"
#include "jadefx/jadefx.hpp"

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

constexpr double kWidth = 900;
constexpr double kHeight = 600;

// An explorer and the Properties page side by side over one place. Writes
// run on this thread, standing in for the simulation thread.
struct Rig {
    DataModel model;
    InstanceId a = 0;
    InstanceId b = 0;
    InstanceId script = 0;
    InstanceId folder = 0;
    InstanceId tri1 = 0;
    InstanceId tri2 = 0;
    std::shared_ptr<ide::IdeExplorer> explorer;
    ide::PropertiesPanel panel;
    std::shared_ptr<jadefx::Scene> scene;
    double clock = 0;

    Rig() {
        a = add<engine_core::GameObject>("A");
        b = add<engine_core::GameObject>("B");
        script = add<engine_core::Script>("Main");
        folder = add<engine_core::Folder>("Stuff");
        tri1 = add<engine_core::TestTriangle>("T1");
        tri2 = add<engine_core::TestTriangle>("T2");
        ide::ExplorerHost host;
        host.enabled = [](std::string_view) { return true; };
        host.rename = [this](InstanceId id, std::string name) { model.set_name(id, name); };
        explorer = jadefx::make<ide::IdeExplorer>(model, "Explorer", host);
        panel.bind(model, model.selection(), model.history());
        auto split = jadefx::make<jadefx::SplitPane>();
        split->getItems().add(explorer);
        split->getItems().add(panel.dock_widget());
        split->setDividerPositions({0.4});
        split->setPrefWidthRatio(1);
        split->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(split, kWidth, kHeight);
        // Building the place is not an edit anyone should undo.
        model.history().reset_waypoints();
        frame();
    }

    template <typename T>
    InstanceId add(const char* name) {
        T& made = model.create<T>();
        model.set_name(made.id(), name);
        model.set_parent(made.id(), model.id());
        model.history().end_gesture();
        return made.id();
    }

    void frame() {
        clock += 0.05;
        scene->layout(kWidth, kHeight, clock);
    }

    void select(std::vector<InstanceId> ids) {
        model.selection().set(std::move(ids));
        frame();
    }

    jadefx::TextField* field(const std::string& property, int part = 0) {
        return dynamic_cast<jadefx::TextField*>(panel.editor(property, part));
    }

    std::string text(const std::string& property, int part = 0) {
        jadefx::TextField* box = field(property, part);
        return box != nullptr ? box->getText() : std::string("<no field>");
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
        auto* tri = dynamic_cast<engine_core::TestTriangle*>(model.instance(id));
        return tri != nullptr ? tri->position() : engine_core::Vec3{};
    }

    bool hasRow(const std::string& name) const { return panel.sheet().find(name) != nullptr; }

    bool mixed(const std::string& name) const {
        const ide::PropertyRow* row = panel.sheet().find(name);
        return row != nullptr && row->mixed;
    }

    int undoDepth() {
        int depth = 0;
        while (model.history().can_undo().first && depth < 100) {
            model.history().undo();
            ++depth;
        }
        for (int step = 0; step < depth; ++step) {
            model.history().redo();
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
    Expect(rig.hasRow("Color"), "R1 a GameObject shows Color");
    Expect(!rig.hasRow("Source") && !rig.hasRow("Changed") && !rig.hasRow("Transform"),
           "R1 Source, signals, and unknown types have no row");

    rig.typeInto("Name", "Hero");
    Expect(rig.model.name(rig.a) == "A", "R1 typing alone does not write");
    rig.enter();
    Expect(rig.model.name(rig.a) == "Hero", "R1 Enter writes the Name");
    rig.frame();
    Expect(rig.explorerRow("Hero") != nullptr, "R1 the explorer label follows");
    Expect(rig.text("Name") == "Hero", "R1 the field shows the written Name");
    const auto undo = rig.model.history().can_undo();
    Expect(undo.first && undo.second == "Set Name", "R1 the waypoint is Set Name");

    // Leaving the field commits what was typed.
    rig.typeInto("Name", "Blurred");
    rig.frame();
    rig.clickAway();
    Expect(rig.model.name(rig.a) == "Blurred", "R1 focus leaving the field commits");
}

void TestR2MixedNameWritesEveryone() {
    Rig rig;
    rig.select({rig.a, rig.b});
    Expect(rig.hasRow("Name") && rig.mixed("Name"), "R2 the Name row is mixed");
    Expect(rig.text("Name").empty(), "R2 a mixed Name is blank, not the first Name");

    rig.typeInto("Name", "C");
    rig.enter();
    Expect(rig.model.name(rig.a) == "C" && rig.model.name(rig.b) == "C", "R2 both Names are C");
    Expect(rig.text("Name") == "C" && !rig.mixed("Name"), "R2 the row now agrees");
    Expect(rig.undoDepth() == 1, "R2 one waypoint for both writes");

    rig.model.history().undo();
    rig.frame();
    Expect(rig.model.name(rig.a) == "A" && rig.model.name(rig.b) == "B", "R2 one undo restores A and B");
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
    Expect(rig.model.name(rig.a) == "A" && rig.model.name(rig.b) == "B", "blank focus-leave writes nothing");
    Expect(!rig.model.history().can_undo().first, "blank focus-leave leaves no waypoint");

    // Enter on the pristine blank field is a no-op too.
    rig.click(rig.field("Name"));
    rig.enter();
    Expect(rig.model.name(rig.a) == "A" && rig.model.name(rig.b) == "B", "blank Enter writes nothing");

    // Typing and then erasing it is an explicit empty Name.
    rig.typeInto("Name", "x");
    rig.key(jadefx::Key::Backspace);
    Expect(rig.text("Name").empty(), "the typed text was erased");
    rig.enter();
    Expect(rig.model.name(rig.a).empty() && rig.model.name(rig.b).empty(), "a cleared field sets an empty Name");
}

void TestR3IntersectionOnly() {
    Rig rig;
    rig.select({rig.a, rig.script});
    Expect(rig.hasRow("Name"), "R3 Name is shared");
    Expect(rig.hasRow("Parent"), "R3 Parent is shared");
    Expect(!rig.hasRow("Color"), "R3 Color is not on a Script");
    Expect(!rig.hasRow("Enabled"), "R3 Enabled is not on a GameObject");
    Expect(rig.panel.editor("Color") == nullptr, "R3 no Color widget");
    // ClassName differs, so it is mixed and blank.
    Expect(rig.mixed("ClassName") && rig.text("ClassName").empty(), "R3 differing classes are mixed");

    rig.select({rig.tri1, rig.a});
    Expect(!rig.hasRow("Position") && !rig.hasRow("Color"), "R3 fields of one class only do not show");
}

void TestR4SameValueIsNotMixed() {
    Rig rig;
    rig.model.set_name(rig.a, "X");
    rig.model.set_name(rig.b, "X");
    rig.select({rig.a, rig.b});
    Expect(!rig.mixed("Name") && rig.text("Name") == "X", "R4 equal Names show the Name");
    Expect(rig.text("ClassName") == "GameObject", "R4 equal classes show the class");
}

void TestR5Vector3PerAxis() {
    Rig rig;
    auto* t1 = dynamic_cast<engine_core::TestTriangle*>(rig.model.instance(rig.tri1));
    auto* t2 = dynamic_cast<engine_core::TestTriangle*>(rig.model.instance(rig.tri2));
    t1->set_position(1, 2, 3);
    t2->set_position(1, 9, 3);
    rig.model.history().reset_waypoints();
    rig.select({rig.tri1, rig.tri2});
    Expect(rig.text("Position", 0) == "1", "R5 X agrees");
    Expect(rig.text("Position", 1).empty(), "R5 Y is mixed and blank");
    Expect(rig.text("Position", 2) == "3", "R5 Z agrees");

    rig.typeInto("Position", "5", 1);
    rig.enter();
    const engine_core::Vec3 p1 = rig.position(rig.tri1);
    const engine_core::Vec3 p2 = rig.position(rig.tri2);
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
    Expect(rig.position(rig.tri1).x == 7 && rig.position(rig.tri2).x == 1, "R5 a Z edit keeps each X");
    Expect(rig.position(rig.tri1).z == 8 && rig.position(rig.tri2).z == 8, "R5 Z is written to both");

    // Not a number: nothing is written and the field goes back.
    rig.typeInto("Position", "abc", 2);
    rig.enter();
    Expect(rig.position(rig.tri1).z == 8, "R5 a bad number does not write");
    Expect(rig.text("Position", 2) == "8", "R5 a bad number shows the value again");
    Expect(!rig.panel.status().empty(), "R5 a bad number says why");
}

void TestR6ParentReference() {
    Rig rig;
    rig.select({rig.a});
    const std::string root = rig.model.name(0);
    Expect(rig.text("Parent") == root + " (DataModel)", "R6 the Parent shows Name (Class)");

    // Pick: the next explorer click is the value, and the selection comes back.
    rig.click(rig.panel.editor("Parent", 1));
    rig.frame();
    Expect(rig.panel.picking() && rig.panel.pick_property() == "Parent", "R6 Pick waits for a click");
    rig.clickExplorer("Stuff");
    Expect(!rig.panel.picking(), "R6 the click ends the pick");
    Expect(rig.model.parent(rig.a) == rig.folder, "R6 the picked instance is the Parent");
    Expect(rig.model.selection().get() == std::vector<InstanceId>{rig.a}, "R6 the selection comes back");
    rig.frame();
    Expect(rig.text("Parent") == "Stuff (Folder)", "R6 the row shows the new Parent");
    Expect(rig.model.history().can_undo().second == "Set Parent", "R6 a pick is one Set Parent");

    // A cycle is refused: the folder cannot go under its own child.
    rig.select({rig.folder});
    rig.click(rig.panel.editor("Parent", 1));
    rig.frame();
    rig.clickExplorer("A");
    Expect(rig.model.parent(rig.folder) == 0, "R6 a descendant cannot be the Parent");
    rig.frame();
    Expect(!rig.panel.status().empty(), "R6 the refusal is shown");
    // And an instance cannot be its own Parent.
    Expect(!ide::apply_edit(rig.model, {rig.folder}, {"Parent", ide::PropertyKind::Ref, {"", false, 0, {}, rig.folder}, -1})
                .written,
           "R6 self is refused");

    // Mixed parents are blank until a pick sets them all.
    rig.select({rig.a, rig.b});
    Expect(rig.mixed("Parent") && rig.text("Parent").empty(), "R6 mixed parents are blank");
    rig.click(rig.panel.editor("Parent", 1));
    rig.frame();
    rig.clickExplorer("Stuff");
    Expect(rig.model.parent(rig.a) == rig.folder && rig.model.parent(rig.b) == rig.folder, "R6 a pick sets both");
    rig.frame();
    Expect(!rig.mixed("Parent") && rig.text("Parent") == "Stuff (Folder)", "R6 the row agrees");

    // Clear sets nil on every selected instance.
    rig.click(rig.panel.editor("Parent", 2));
    rig.frame();
    Expect(rig.model.parent(rig.a) == DataModel::kNoParent && rig.model.parent(rig.b) == DataModel::kNoParent,
           "R6 Clear sets nil");
    Expect(rig.text("Parent").empty(), "R6 nil shows empty");

    // Pick again, then Pick a second time cancels.
    rig.click(rig.panel.editor("Parent", 1));
    rig.frame();
    rig.click(rig.panel.editor("Parent", 1));
    rig.frame();
    Expect(!rig.panel.picking(), "R6 a second Pick cancels");
}

void TestR7MidEditIsNotClobbered() {
    Rig rig;
    rig.select({rig.a});
    rig.typeInto("Name", "Typed");
    rig.frame();
    rig.model.set_name(rig.a, "Outside");
    rig.frame();
    Expect(rig.text("Name") == "Typed", "R7 an outside write does not overwrite typing");
    rig.key(jadefx::Key::Escape);
    rig.frame();
    Expect(rig.text("Name") == "Outside", "R7 Escape resyncs to the world");
    Expect(rig.model.name(rig.a) == "Outside", "R7 Escape writes nothing");

    rig.typeInto("Name", "Mine");
    rig.model.set_name(rig.a, "Again");
    rig.frame();
    Expect(rig.text("Name") == "Mine", "R7 still not overwritten");
    rig.enter();
    Expect(rig.model.name(rig.a) == "Mine" && rig.text("Name") == "Mine", "R7 commit writes and resyncs");

    // Not focused: an outside write shows at once, and agreement is live.
    rig.clickAway();
    rig.select({rig.a, rig.b});
    Expect(rig.text("Name").empty(), "R7 differing Names are blank");
    rig.model.set_name(rig.b, "Mine");
    rig.frame();
    Expect(rig.text("Name") == "Mine", "R7 Names that come to agree show");
    rig.model.set_name(rig.b, "Other");
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
    Expect(!rig.hasRow("Color"), "R9 a Script hides Color");
    rig.model.destroy(rig.script);
    rig.frame();
    Expect(rig.panel.sheet().ids.size() == 2, "R9 the destroyed instance leaves");
    Expect(rig.hasRow("Color"), "R9 the intersection widens again");
}

void TestBooleanAndNumber() {
    RegisterProbe();
    Rig rig;
    Probe& p1 = rig.model.create<Probe>();
    Probe& p2 = rig.model.create<Probe>();
    rig.model.set_parent(p1.id(), 0);
    rig.model.set_parent(p2.id(), 0);
    p1.flag = true;
    p1.speed = 1.5;
    p2.speed = 1.5;
    rig.model.history().reset_waypoints();
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
    const ide::PropertySheet sheet = ide::read_sheet(rig.model, ids);
    const ide::PropertyRow* speed = sheet.find("Speed");
    Expect(speed != nullptr && speed->mixed && speed->value.number == 0, "a mixed number hides the first value");
    Expect(sheet.rows.size() >= 3 && sheet.rows[0].name == "Name" && sheet.rows[1].name == "Parent" &&
               sheet.rows[2].name == "ClassName",
           "Instance rows lead in a fixed order");
}

void TestMixedEnabledIsOneWaypoint() {
    Rig rig;
    const InstanceId other = rig.add<engine_core::Script>("Other");
    auto* first = dynamic_cast<engine_core::LuaSource*>(rig.model.instance(rig.script));
    auto* second = dynamic_cast<engine_core::LuaSource*>(rig.model.instance(other));
    second->set_enabled(false);
    rig.model.history().reset_waypoints();
    rig.select({rig.script, other});
    auto* check = dynamic_cast<jadefx::CheckBox*>(rig.panel.editor("Enabled"));
    Expect(check != nullptr && check->isIndeterminate() && !check->isSelected(),
           "mixed Enabled is indeterminate, not false");
    rig.click(check);
    rig.frame();
    Expect(first->enabled() && second->enabled(), "a click enables both");
    Expect(rig.model.history().can_undo().second == "Set Enabled" && rig.undoDepth() == 1,
           "the click is one Set Enabled waypoint");
    rig.model.history().undo();
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
    Expect(rig.model.name(rig.a) == "A" && !rig.model.history().can_undo().first,
           "undoing back to the start is not an edit");
    rig.clickAway();
    Expect(!rig.panel.field_undo(false), "an unfocused page does not take the chord");
}

void TestPlayEditIsUndoable() {
    Rig rig;
    rig.model.capture_place();
    rig.model.start_simulation();
    rig.select({rig.a, rig.b});
    rig.typeInto("Name", "Play");
    rig.enter();
    Expect(rig.model.name(rig.a) == "Play" && rig.model.name(rig.b) == "Play", "a play edit writes both");
    Expect(rig.model.history().can_undo().first, "a play edit is recorded");
    rig.model.history().undo();
    rig.frame();
    Expect(rig.model.name(rig.a) == "A" && rig.model.name(rig.b) == "B", "and undoes in the session");
    rig.model.stop_simulation();
}

}  // namespace

int main() {
    TestR1SingleSelectionEditsName();
    TestR2MixedNameWritesEveryone();
    TestMixedBlankIsNotEmptyName();
    TestR3IntersectionOnly();
    TestR4SameValueIsNotMixed();
    TestR5Vector3PerAxis();
    TestR6ParentReference();
    TestR7MidEditIsNotClobbered();
    TestR8NoSelection();
    TestR9DestroyedLeavesIntersection();
    TestBooleanAndNumber();
    TestMixedEnabledIsOneWaypoint();
    TestFieldUndoThenPlace();
    TestPlayEditIsUndoable();
    if (gFailures == 0) {
        std::printf("properties tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failed\n", gFailures);
    return 1;
}
