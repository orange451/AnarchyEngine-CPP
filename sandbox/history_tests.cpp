#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "Events.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"
#include "TaskScheduler.hpp"
#include "TestTriangle.hpp"
#include "ide/InputRouter.hpp"
#include "types.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

engine_core::ColorRgb rgb(float r, float g, float b) {
    engine_core::ColorRgb color;
    color.r = r;
    color.g = g;
    color.b = b;
    color.a = 1.f;
    return color;
}

bool same_color(engine_core::ColorRgb a, engine_core::ColorRgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool same_transform(const engine_core::Transform& a, const engine_core::Transform& b) {
    return std::memcmp(a.m, b.m, sizeof(a.m)) == 0;
}

struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

void close_gesture(engine_core::DataModel& model) { model.history().end_gesture(); }

engine_core::GameObject& make_part(engine_core::DataModel& model, const char* name) {
    engine_core::GameObject& part = model.create<engine_core::GameObject>();
    model.set_name(part.id(), name);
    model.set_parent(part.id(), model.id());
    return part;
}

}  // namespace

TEST_CASE("H1 edit create undo restores the same id", "[H1][history]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = model.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    model.set_name(id, "Brick");
    const engine_core::Transform placed = engine_core::transform_translation(1.f, 2.f, 3.f);
    part.set_transform(placed);
    close_gesture(model);

    REQUIRE(model.history().can_undo().first);
    model.history().undo();
    REQUIRE_FALSE(model.alive(id));
    REQUIRE_FALSE(model.history().can_undo().first);
    REQUIRE(model.history().can_redo().first);

    model.history().redo();
    REQUIRE(model.alive(id));
    REQUIRE(model.name(id) == "Brick");
    const engine_core::GameObject* restored = model.game_object(id);
    REQUIRE(restored != nullptr);
    REQUIRE(same_transform(restored->transform(), placed));
}

TEST_CASE("H2 three color recordings and a new edit clears redo", "[H2][history]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = make_part(model, "Brick");
    close_gesture(model);
    model.history().reset_waypoints();

    const engine_core::ColorRgb a = rgb(1.f, 0.f, 0.f);
    const engine_core::ColorRgb b = rgb(0.f, 1.f, 0.f);
    const engine_core::ColorRgb c = rgb(0.f, 0.f, 1.f);
    const engine_core::ColorRgb d = rgb(1.f, 1.f, 0.f);
    part.set_color(a);
    close_gesture(model);
    part.set_color(b);
    close_gesture(model);
    part.set_color(c);
    close_gesture(model);

    model.history().undo();
    model.history().undo();
    REQUIRE(same_color(part.color(), a));
    REQUIRE(model.history().can_redo().first);

    const auto redo_before = model.history().can_redo();
    const auto empty = model.history().try_begin_recording("Empty");
    REQUIRE(empty.has_value());
    model.history().finish_recording(*empty, engine_core::FinishRecordingOperation::Commit);
    REQUIRE(model.history().can_redo() == redo_before);

    part.set_color(d);
    close_gesture(model);
    REQUIRE_FALSE(model.history().can_redo().first);
    model.history().redo();
    REQUIRE(same_color(part.color(), d));
}

TEST_CASE("H3 one recording coalesces a drag", "[H3][history]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = make_part(model, "Brick");
    close_gesture(model);
    model.history().reset_waypoints();
    const engine_core::Transform home = part.transform();

    const auto recording = model.history().try_begin_recording("Move");
    REQUIRE(recording.has_value());
    for (int step = 0; step < 50; ++step) {
        part.set_transform(engine_core::transform_translation(static_cast<float>(step), 0.f, 0.f));
    }
    model.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);

    model.history().undo();
    REQUIRE(same_transform(part.transform(), home));
    REQUIRE_FALSE(model.history().can_undo().first);
    REQUIRE(model.history().can_redo().second == "Move");
}

TEST_CASE("H4 implicit names coalesce until the gesture ends", "[H4][history]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = make_part(model, "Brick");
    close_gesture(model);
    model.history().reset_waypoints();

    model.history().set_pending_gesture("Rename");
    model.set_name(part.id(), "A");
    model.set_name(part.id(), "B");
    close_gesture(model);

    REQUIRE(model.history().can_undo().second == "Rename");
    model.history().undo();
    REQUIRE(model.name(part.id()) == "Brick");
    REQUIRE_FALSE(model.history().can_undo().first);
    model.history().redo();
    REQUIRE(model.name(part.id()) == "B");
}

TEST_CASE("H5 play script writes stay off the edit stack", "[H5][history]") {
    SimRole role;
    engine_core::DataModel model;
    engine_core::TaskScheduler scheduler;
    engine_core::ScriptRuntime runtime;
    scheduler.reserve(8);
    model.attach_scheduler(&scheduler);
    runtime.attach(model, scheduler);

    engine_core::GameObject& part = make_part(model, "Brick");
    const engine_core::ColorRgb authored = rgb(1.f, 0.f, 0.f);
    part.set_color(authored);
    part.set_linear_velocity(4.f, 0.f, 0.f);
    engine_core::Script& script = model.create<engine_core::Script>();
    model.set_name(script.id(), "Paint");
    script.set_source(R"(
        local part = game:FindFirstChild("Brick")
        for _ = 1, 100 do
            part.Color = {r = 0, g = 0, b = 1, a = 1}
        end
    )");
    model.set_parent(script.id(), model.id());
    close_gesture(model);
    const auto edit = model.history().can_undo();
    REQUIRE(edit.first);

    model.start_simulation();
    const auto during = model.history().can_undo();
    scheduler.run_phase(engine_core::Phase::Heartbeat, 1.0 / 60.0);
    model.events().drain();
    runtime.heartbeat(1.0 / 60.0);
    model.events().drain();
    model.integrate_simulated(1.0);

    REQUIRE(runtime.last_error().empty());
    REQUIRE(same_color(part.color(), rgb(0.f, 0.f, 1.f)));
    REQUIRE(model.history().can_undo() == during);
    REQUIRE_FALSE(during.first);

    model.stop_simulation();
    REQUIRE(same_color(part.color(), authored));
    REQUIRE(same_transform(part.transform(), engine_core::transform_identity()));
    REQUIRE(model.history().can_undo() == edit);
}

TEST_CASE("H6 a play recording undoes, then stop drops it", "[H6][history]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = make_part(model, "Brick");
    const engine_core::ColorRgb authored = rgb(1.f, 0.f, 0.f);
    part.set_color(authored);
    close_gesture(model);
    const auto edit = model.history().can_undo();

    model.start_simulation();
    const auto recording = model.history().try_begin_recording("Play");
    REQUIRE(recording.has_value());
    part.set_color(rgb(0.f, 0.f, 1.f));
    model.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
    REQUIRE(model.history().can_undo().second == "Play");

    model.history().undo();
    REQUIRE(same_color(part.color(), authored));
    REQUIRE(model.history().can_redo().second == "Play");

    const auto again = model.history().try_begin_recording("Play");
    REQUIRE(again.has_value());
    part.set_color(rgb(0.f, 1.f, 0.f));
    model.history().finish_recording(*again, engine_core::FinishRecordingOperation::Commit);
    REQUIRE(same_color(part.color(), rgb(0.f, 1.f, 0.f)));

    model.stop_simulation();
    REQUIRE(same_color(part.color(), authored));
    REQUIRE(model.history().can_undo() == edit);
    REQUIRE_FALSE(model.history().can_redo().first);

    model.start_simulation();
    REQUIRE_FALSE(model.history().can_undo().first);
    model.stop_simulation();
    REQUIRE(model.history().can_undo() == edit);
}

TEST_CASE("H7 cancel restores the destroyed part and leaves the stacks", "[H7][history]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = make_part(model, "Brick");
    const engine_core::InstanceId id = part.id();
    close_gesture(model);
    const auto undo_before = model.history().can_undo();
    const auto redo_before = model.history().can_redo();

    const auto recording = model.history().try_begin_recording("Delete");
    REQUIRE(recording.has_value());
    model.destroy(id);
    REQUIRE_FALSE(model.alive(id));
    model.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Cancel);

    REQUIRE(model.alive(id));
    REQUIRE(model.name(id) == "Brick");
    REQUIRE(model.history().can_undo() == undo_before);
    REQUIRE(model.history().can_redo() == redo_before);
    REQUIRE_FALSE(model.history().is_recording_in_progress());
}

TEST_CASE("H8 a second begin does not replace the open recording", "[H8][history]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = make_part(model, "Brick");
    close_gesture(model);
    model.history().reset_waypoints();
    const engine_core::ColorRgb original = part.color();

    const auto first = model.history().try_begin_recording("Paint", "Paint");
    REQUIRE(first.has_value());
    REQUIRE_FALSE(model.history().try_begin_recording("Other").has_value());
    REQUIRE(model.history().is_recording_in_progress(*first));
    part.set_color(rgb(0.2f, 0.3f, 0.4f));
    model.history().finish_recording("nope", engine_core::FinishRecordingOperation::Commit);
    REQUIRE(model.history().is_recording_in_progress(*first));
    model.history().finish_recording(*first, engine_core::FinishRecordingOperation::Commit);

    REQUIRE(model.history().can_undo().second == "Paint");
    model.history().undo();
    REQUIRE(same_color(part.color(), original));
}

TEST_CASE("H9 undo during a recording is a no-op", "[H9][history]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = make_part(model, "Brick");
    close_gesture(model);
    model.history().reset_waypoints();

    const auto recording = model.history().try_begin_recording("Paint");
    REQUIRE(recording.has_value());
    part.set_color(rgb(0.f, 1.f, 0.f));
    const engine_core::ColorRgb painted = part.color();
    model.history().undo();
    model.history().redo();
    REQUIRE(same_color(part.color(), painted));
    REQUIRE(model.history().is_recording_in_progress(*recording));
    REQUIRE_FALSE(model.history().can_undo().first);

    model.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
    model.history().undo();
    REQUIRE_FALSE(same_color(part.color(), painted));
}

TEST_CASE("H10 script focus undoes text and leaves the place alone", "[H10][history]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = make_part(model, "Brick");
    close_gesture(model);
    model.history().reset_waypoints();
    const engine_core::ColorRgb painted = rgb(1.f, 0.f, 0.f);
    part.set_color(painted);
    close_gesture(model);

    ide::InputRouter router;
    ide::TextUndoStack& text = router.script_stack(42);
    text.reset("hi");
    text.insert(text.text().size(), "!");
    REQUIRE(text.text() == "hi!");

    ide::Focus editor;
    editor.kind = ide::FocusKind::ScriptEditor;
    editor.script = 42;
    router.set_focus(editor);

    ide::KeyChord undo;
    undo.primary = true;
    undo.key = ide::ChordKey::Z;
    REQUIRE(router.handle(undo, &model.history()));
    REQUIRE(text.text() == "hi");
    REQUIRE(same_color(part.color(), painted));
    REQUIRE(model.history().can_undo().first);

    ide::Focus viewport;
    viewport.kind = ide::FocusKind::Viewport;
    router.set_focus(viewport);
    REQUIRE(router.handle(undo, &model.history()));
    REQUIRE(text.text() == "hi");
    REQUIRE_FALSE(same_color(part.color(), painted));
}

TEST_CASE("H11 an empty text stack does not undo the place", "[H11][history]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = make_part(model, "Brick");
    close_gesture(model);
    model.history().reset_waypoints();
    part.set_color(rgb(0.f, 1.f, 0.f));
    close_gesture(model);
    const auto edit = model.history().can_undo();

    ide::InputRouter router;
    router.script_stack(7).reset("local x = 1");
    ide::Focus editor;
    editor.kind = ide::FocusKind::ScriptEditor;
    editor.script = 7;
    router.set_focus(editor);

    ide::KeyChord undo;
    undo.primary = true;
    undo.key = ide::ChordKey::Z;
    REQUIRE(router.handle(undo, &model.history()));
    REQUIRE(model.history().can_undo() == edit);
    REQUIRE(same_color(part.color(), rgb(0.f, 1.f, 0.f)));
    REQUIRE_FALSE(model.history().is_recording_in_progress());
}

TEST_CASE("H12 redo follows focus", "[H12][history]") {
    engine_core::DataModel model;
    engine_core::GameObject& part = make_part(model, "Brick");
    close_gesture(model);
    model.history().reset_waypoints();
    const engine_core::ColorRgb painted = rgb(0.f, 0.f, 1.f);
    part.set_color(painted);
    close_gesture(model);
    model.history().undo();
    REQUIRE_FALSE(same_color(part.color(), painted));

    ide::InputRouter router;
    ide::TextUndoStack& text = router.script_stack(3);
    text.reset("");
    text.insert(0, "a");
    text.insert(1, "b");
    text.undo();
    REQUIRE(text.text() == "a");

    ide::KeyChord mac_redo;
    mac_redo.primary = true;
    mac_redo.shift = true;
    mac_redo.apple = true;
    mac_redo.key = ide::ChordKey::Z;
    ide::KeyChord win_redo;
    win_redo.primary = true;
    win_redo.key = ide::ChordKey::Y;

    ide::Focus editor;
    editor.kind = ide::FocusKind::ScriptEditor;
    editor.script = 3;
    router.set_focus(editor);
    REQUIRE(router.handle(mac_redo, &model.history()));
    REQUIRE(text.text() == "ab");
    REQUIRE_FALSE(same_color(model.game_object(part.id())->color(), painted));

    text.undo();
    REQUIRE(router.handle(win_redo, &model.history()));
    REQUIRE(text.text() == "ab");

    ide::Focus viewport;
    viewport.kind = ide::FocusKind::Viewport;
    router.set_focus(viewport);
    REQUIRE(router.handle(mac_redo, &model.history()));
    REQUIRE(same_color(model.game_object(part.id())->color(), painted));
    model.history().undo();
    REQUIRE(router.handle(win_redo, &model.history()));
    REQUIRE(same_color(model.game_object(part.id())->color(), painted));
}

TEST_CASE("H13 undo destroy restores children and names", "[H13][history]") {
    engine_core::DataModel model;
    engine_core::Folder& folder = model.create<engine_core::Folder>();
    const engine_core::InstanceId folder_id = folder.id();
    model.set_name(folder_id, "Box");
    model.set_parent(folder_id, model.id());

    engine_core::GameObject& first = model.create<engine_core::GameObject>();
    engine_core::GameObject& second = model.create<engine_core::GameObject>();
    const engine_core::InstanceId first_id = first.id();
    const engine_core::InstanceId second_id = second.id();
    model.set_name(first_id, "Ann");
    model.set_name(second_id, "Bea");
    model.set_parent(first_id, folder_id);
    model.set_parent(second_id, folder_id);
    engine_core::GameObject& grand = model.create<engine_core::GameObject>();
    const engine_core::InstanceId grand_id = grand.id();
    model.set_name(grand_id, "Cara");
    model.set_parent(grand_id, second_id);
    close_gesture(model);
    model.history().reset_waypoints();

    const auto recording = model.history().try_begin_recording("Delete");
    REQUIRE(recording.has_value());
    model.destroy(folder_id);
    model.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);

    REQUIRE_FALSE(model.alive(folder_id));
    REQUIRE(model.alive(second_id));
    REQUIRE(model.parent(second_id) == engine_core::DataModel::kNoParent);
    REQUIRE(model.parent(grand_id) == second_id);

    model.history().undo();
    REQUIRE(model.alive(folder_id));
    REQUIRE(model.name(folder_id) == "Box");
    REQUIRE(model.name(first_id) == "Ann");
    REQUIRE(model.name(second_id) == "Bea");
    REQUIRE(model.name(grand_id) == "Cara");
    REQUIRE(model.parent(first_id) == folder_id);
    REQUIRE(model.parent(second_id) == folder_id);
    REQUIRE(model.parent(grand_id) == second_id);
    // set_parent inserts at the head, so the later child is first.
    REQUIRE(model.first_child(folder_id) == second_id);
    REQUIRE(model.next_sibling(second_id) == first_id);
}

TEST_CASE("H15 applying undo does not record a waypoint", "[H15][history]") {
    SimRole role;
    engine_core::DataModel model;
    model.events().set_policy(engine_core::EventPolicy::Immediate);
    engine_core::GameObject& part = make_part(model, "Brick");
    close_gesture(model);
    model.history().reset_waypoints();

    const engine_core::ColorRgb original = part.color();
    part.set_color(rgb(1.f, 0.f, 0.f));
    close_gesture(model);

    int undos = 0;
    model.history().on_undo.connect([&](const std::string& name) {
        REQUIRE(name == "Set Color");
        ++undos;
    });
    model.property_changed(part.id(), engine_core::Field::Color)
        .connect([&](engine_core::InstanceId, engine_core::Field) {
            REQUIRE(undos == 0);
            model.set_name(part.id(), "from-handler");
            REQUIRE(undos == 0);
        });

    model.history().undo();
    REQUIRE(undos == 1);
    REQUIRE(same_color(part.color(), original));
    REQUIRE(model.name(part.id()) == "from-handler");
    REQUIRE_FALSE(model.history().can_undo().first);
    REQUIRE(model.history().can_redo().second == "Set Color");
}

TEST_CASE("H16 a text stack out of sync with the editor refuses the edit", "[H16][history]") {
    // An editor that loaded "print(1)" but whose stack was never seeded: typing at
    // the end must not be recorded against an empty buffer, or undo empties the script.
    ide::TextUndoStack text;
    REQUIRE_FALSE(text.record_change(8, "", "\n"));
    REQUIRE_FALSE(text.can_undo());
    REQUIRE(text.text().empty());

    text.reset("print(1)");
    REQUIRE(text.record_change(8, "", "\n"));
    REQUIRE(text.text() == "print(1)\n");
    REQUIRE(text.undo());
    REQUIRE(text.text() == "print(1)");
    REQUIRE_FALSE(text.undo());
    REQUIRE(text.text() == "print(1)");

    REQUIRE_FALSE(text.record_change(0, "x", ""));
    REQUIRE_FALSE(text.can_undo());
}

TEST_CASE("H17 set_parent_at places a child and undoes to its old place", "[H17][history]") {
    engine_core::DataModel model;
    const engine_core::InstanceId a = make_part(model, "A").id();
    const engine_core::InstanceId b = make_part(model, "B").id();
    const engine_core::InstanceId c = make_part(model, "C").id();
    engine_core::Folder& folder = model.create<engine_core::Folder>();
    const engine_core::InstanceId f = folder.id();
    model.set_parent(f, model.id());
    const engine_core::InstanceId x = make_part(model, "X").id();
    model.set_parent(x, f);
    close_gesture(model);
    // set_parent puts each child first.
    const std::vector<engine_core::InstanceId> start{f, c, b, a};
    REQUIRE(model.get_children(0) == start);

    SECTION("a reorder under the same parent is one undo step") {
        model.set_parent_at(c, 0, 3);
        close_gesture(model);
        REQUIRE(model.get_children(0) == std::vector<engine_core::InstanceId>{f, b, a, c});
        model.set_parent_at(c, 0, 3);
        close_gesture(model);
        REQUIRE(model.get_children(0) == std::vector<engine_core::InstanceId>{f, b, a, c});

        model.history().undo();
        REQUIRE(model.get_children(0) == start);
        model.history().redo();
        REQUIRE(model.get_children(0) == std::vector<engine_core::InstanceId>{f, b, a, c});
    }

    SECTION("a move under a new parent lands at the index, and redo puts it back there") {
        model.set_parent_at(b, f, 1);
        close_gesture(model);
        REQUIRE(model.parent(b) == f);
        REQUIRE(model.get_children(f) == std::vector<engine_core::InstanceId>{x, b});

        model.history().undo();
        REQUIRE(model.get_children(0) == start);
        REQUIRE(model.get_children(f) == std::vector<engine_core::InstanceId>{x});
        model.history().redo();
        REQUIRE(model.get_children(f) == std::vector<engine_core::InstanceId>{x, b});
    }

    SECTION("a negative or too large index is last") {
        model.set_parent_at(a, f, -1);
        model.set_parent_at(c, f, 99);
        close_gesture(model);
        REQUIRE(model.get_children(f) == std::vector<engine_core::InstanceId>{x, a, c});
    }
}
