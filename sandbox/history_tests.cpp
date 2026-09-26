#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "Events.hpp"
#include "Folder.hpp"
#include "Game.hpp"
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

void close_gesture(engine_core::DataModel& game) { game.history().end_gesture(); }

engine_core::GameObject& make_part(engine_core::DataModel& game, const char* name) {
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    game.set_name(part.id(), name);
    game.set_parent(part.id(), game.id());
    return part;
}

}  // namespace

TEST_CASE("H1 edit create undo restores the same id", "[H1][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    game.set_name(id, "Brick");
    const engine_core::Transform placed = engine_core::transform_translation(1.f, 2.f, 3.f);
    part.set_transform(placed);
    close_gesture(game);

    REQUIRE(game.history().can_undo().first);
    game.history().undo();
    REQUIRE_FALSE(game.alive(id));
    REQUIRE_FALSE(game.history().can_undo().first);
    REQUIRE(game.history().can_redo().first);

    game.history().redo();
    REQUIRE(game.alive(id));
    REQUIRE(game.name(id) == "Brick");
    const engine_core::GameObject* restored = game.game_object(id);
    REQUIRE(restored != nullptr);
    REQUIRE(same_transform(restored->transform(), placed));
}

TEST_CASE("H2 three color recordings and a new edit clears redo", "[H2][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    close_gesture(game);
    game.history().reset_waypoints();

    const engine_core::ColorRgb a = rgb(1.f, 0.f, 0.f);
    const engine_core::ColorRgb b = rgb(0.f, 1.f, 0.f);
    const engine_core::ColorRgb c = rgb(0.f, 0.f, 1.f);
    const engine_core::ColorRgb d = rgb(1.f, 1.f, 0.f);
    part.set_color(a);
    close_gesture(game);
    part.set_color(b);
    close_gesture(game);
    part.set_color(c);
    close_gesture(game);

    game.history().undo();
    game.history().undo();
    REQUIRE(same_color(part.color(), a));
    REQUIRE(game.history().can_redo().first);

    const auto redo_before = game.history().can_redo();
    const auto empty = game.history().try_begin_recording("Empty");
    REQUIRE(empty.has_value());
    game.history().finish_recording(*empty, engine_core::FinishRecordingOperation::Commit);
    REQUIRE(game.history().can_redo() == redo_before);

    part.set_color(d);
    close_gesture(game);
    REQUIRE_FALSE(game.history().can_redo().first);
    game.history().redo();
    REQUIRE(same_color(part.color(), d));
}

TEST_CASE("H3 one recording coalesces a drag", "[H3][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    close_gesture(game);
    game.history().reset_waypoints();
    const engine_core::Transform home = part.transform();

    const auto recording = game.history().try_begin_recording("Move");
    REQUIRE(recording.has_value());
    for (int step = 0; step < 50; ++step) {
        part.set_transform(engine_core::transform_translation(static_cast<float>(step), 0.f, 0.f));
    }
    game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);

    game.history().undo();
    REQUIRE(same_transform(part.transform(), home));
    REQUIRE_FALSE(game.history().can_undo().first);
    REQUIRE(game.history().can_redo().second == "Move");
}

TEST_CASE("H4 implicit names coalesce until the gesture ends", "[H4][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    close_gesture(game);
    game.history().reset_waypoints();

    game.history().set_pending_gesture("Rename");
    game.set_name(part.id(), "A");
    game.set_name(part.id(), "B");
    close_gesture(game);

    REQUIRE(game.history().can_undo().second == "Rename");
    game.history().undo();
    REQUIRE(game.name(part.id()) == "Brick");
    REQUIRE_FALSE(game.history().can_undo().first);
    game.history().redo();
    REQUIRE(game.name(part.id()) == "B");
}

TEST_CASE("H5 play script writes stay off the edit stack", "[H5][history]") {
    SimRole role;
    engine_core::Game game;
    engine_core::TaskScheduler scheduler;
    engine_core::ScriptRuntime runtime;
    scheduler.reserve(8);
    game.attach_scheduler(&scheduler);
    runtime.attach(game, scheduler);

    engine_core::GameObject& part = make_part(game, "Brick");
    const engine_core::ColorRgb authored = rgb(1.f, 0.f, 0.f);
    part.set_color(authored);
    part.set_linear_velocity(4.f, 0.f, 0.f);
    engine_core::Script& script = game.create<engine_core::Script>();
    game.set_name(script.id(), "Paint");
    script.set_source(R"(
        local part = game:FindFirstChild("Brick")
        for _ = 1, 100 do
            part.Color = {r = 0, g = 0, b = 1, a = 1}
        end
    )");
    game.set_parent(script.id(), game.id());
    close_gesture(game);
    const auto edit = game.history().can_undo();
    REQUIRE(edit.first);

    game.start_simulation();
    const auto during = game.history().can_undo();
    scheduler.run_phase(engine_core::Phase::Heartbeat, 1.0 / 60.0);
    game.events().drain();
    runtime.heartbeat(1.0 / 60.0);
    game.events().drain();
    game.integrate_simulated(1.0);

    REQUIRE(runtime.last_error().empty());
    REQUIRE(same_color(part.color(), rgb(0.f, 0.f, 1.f)));
    REQUIRE(game.history().can_undo() == during);
    REQUIRE_FALSE(during.first);

    game.stop_simulation();
    REQUIRE(same_color(part.color(), authored));
    REQUIRE(same_transform(part.transform(), engine_core::transform_identity()));
    REQUIRE(game.history().can_undo() == edit);
}

TEST_CASE("H6 a play recording undoes, then stop drops it", "[H6][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    const engine_core::ColorRgb authored = rgb(1.f, 0.f, 0.f);
    part.set_color(authored);
    close_gesture(game);
    const auto edit = game.history().can_undo();

    game.start_simulation();
    const auto recording = game.history().try_begin_recording("Play");
    REQUIRE(recording.has_value());
    part.set_color(rgb(0.f, 0.f, 1.f));
    game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
    REQUIRE(game.history().can_undo().second == "Play");

    game.history().undo();
    REQUIRE(same_color(part.color(), authored));
    REQUIRE(game.history().can_redo().second == "Play");

    const auto again = game.history().try_begin_recording("Play");
    REQUIRE(again.has_value());
    part.set_color(rgb(0.f, 1.f, 0.f));
    game.history().finish_recording(*again, engine_core::FinishRecordingOperation::Commit);
    REQUIRE(same_color(part.color(), rgb(0.f, 1.f, 0.f)));

    game.stop_simulation();
    REQUIRE(same_color(part.color(), authored));
    REQUIRE(game.history().can_undo() == edit);
    REQUIRE_FALSE(game.history().can_redo().first);

    game.start_simulation();
    REQUIRE_FALSE(game.history().can_undo().first);
    game.stop_simulation();
    REQUIRE(game.history().can_undo() == edit);
}

TEST_CASE("H7 cancel restores the destroyed part and leaves the stacks", "[H7][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    const engine_core::InstanceId id = part.id();
    close_gesture(game);
    const auto undo_before = game.history().can_undo();
    const auto redo_before = game.history().can_redo();

    const auto recording = game.history().try_begin_recording("Delete");
    REQUIRE(recording.has_value());
    game.destroy(id);
    REQUIRE_FALSE(game.alive(id));
    game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Cancel);

    REQUIRE(game.alive(id));
    REQUIRE(game.name(id) == "Brick");
    REQUIRE(game.history().can_undo() == undo_before);
    REQUIRE(game.history().can_redo() == redo_before);
    REQUIRE_FALSE(game.history().is_recording_in_progress());
}

TEST_CASE("H8 a second begin does not replace the open recording", "[H8][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    close_gesture(game);
    game.history().reset_waypoints();
    const engine_core::ColorRgb original = part.color();

    const auto first = game.history().try_begin_recording("Paint", "Paint");
    REQUIRE(first.has_value());
    REQUIRE_FALSE(game.history().try_begin_recording("Other").has_value());
    REQUIRE(game.history().is_recording_in_progress(*first));
    part.set_color(rgb(0.2f, 0.3f, 0.4f));
    game.history().finish_recording("nope", engine_core::FinishRecordingOperation::Commit);
    REQUIRE(game.history().is_recording_in_progress(*first));
    game.history().finish_recording(*first, engine_core::FinishRecordingOperation::Commit);

    REQUIRE(game.history().can_undo().second == "Paint");
    game.history().undo();
    REQUIRE(same_color(part.color(), original));
}

TEST_CASE("H9 undo during a recording is a no-op", "[H9][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    close_gesture(game);
    game.history().reset_waypoints();

    const auto recording = game.history().try_begin_recording("Paint");
    REQUIRE(recording.has_value());
    part.set_color(rgb(0.f, 1.f, 0.f));
    const engine_core::ColorRgb painted = part.color();
    game.history().undo();
    game.history().redo();
    REQUIRE(same_color(part.color(), painted));
    REQUIRE(game.history().is_recording_in_progress(*recording));
    REQUIRE_FALSE(game.history().can_undo().first);

    game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
    game.history().undo();
    REQUIRE_FALSE(same_color(part.color(), painted));
}

TEST_CASE("H10 script focus undoes text and leaves the place alone", "[H10][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    close_gesture(game);
    game.history().reset_waypoints();
    const engine_core::ColorRgb painted = rgb(1.f, 0.f, 0.f);
    part.set_color(painted);
    close_gesture(game);

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
    REQUIRE(router.handle(undo, &game.history()));
    REQUIRE(text.text() == "hi");
    REQUIRE(same_color(part.color(), painted));
    REQUIRE(game.history().can_undo().first);

    ide::Focus viewport;
    viewport.kind = ide::FocusKind::Viewport;
    router.set_focus(viewport);
    REQUIRE(router.handle(undo, &game.history()));
    REQUIRE(text.text() == "hi");
    REQUIRE_FALSE(same_color(part.color(), painted));
}

TEST_CASE("H11 an empty text stack does not undo the place", "[H11][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    close_gesture(game);
    game.history().reset_waypoints();
    part.set_color(rgb(0.f, 1.f, 0.f));
    close_gesture(game);
    const auto edit = game.history().can_undo();

    ide::InputRouter router;
    router.script_stack(7).reset("local x = 1");
    ide::Focus editor;
    editor.kind = ide::FocusKind::ScriptEditor;
    editor.script = 7;
    router.set_focus(editor);

    ide::KeyChord undo;
    undo.primary = true;
    undo.key = ide::ChordKey::Z;
    REQUIRE(router.handle(undo, &game.history()));
    REQUIRE(game.history().can_undo() == edit);
    REQUIRE(same_color(part.color(), rgb(0.f, 1.f, 0.f)));
    REQUIRE_FALSE(game.history().is_recording_in_progress());
}

TEST_CASE("H12 redo follows focus", "[H12][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    close_gesture(game);
    game.history().reset_waypoints();
    const engine_core::ColorRgb painted = rgb(0.f, 0.f, 1.f);
    part.set_color(painted);
    close_gesture(game);
    game.history().undo();
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
    REQUIRE(router.handle(mac_redo, &game.history()));
    REQUIRE(text.text() == "ab");
    REQUIRE_FALSE(same_color(game.game_object(part.id())->color(), painted));

    text.undo();
    REQUIRE(router.handle(win_redo, &game.history()));
    REQUIRE(text.text() == "ab");

    ide::Focus viewport;
    viewport.kind = ide::FocusKind::Viewport;
    router.set_focus(viewport);
    REQUIRE(router.handle(mac_redo, &game.history()));
    REQUIRE(same_color(game.game_object(part.id())->color(), painted));
    game.history().undo();
    REQUIRE(router.handle(win_redo, &game.history()));
    REQUIRE(same_color(game.game_object(part.id())->color(), painted));
}

TEST_CASE("H13 undo destroy restores children and names", "[H13][history]") {
    engine_core::Game game;
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    const engine_core::InstanceId folder_id = folder.id();
    game.set_name(folder_id, "Box");
    game.set_parent(folder_id, game.id());

    engine_core::GameObject& first = game.create<engine_core::GameObject>();
    engine_core::GameObject& second = game.create<engine_core::GameObject>();
    const engine_core::InstanceId first_id = first.id();
    const engine_core::InstanceId second_id = second.id();
    game.set_name(first_id, "Ann");
    game.set_name(second_id, "Bea");
    game.set_parent(first_id, folder_id);
    game.set_parent(second_id, folder_id);
    engine_core::GameObject& grand = game.create<engine_core::GameObject>();
    const engine_core::InstanceId grand_id = grand.id();
    game.set_name(grand_id, "Cara");
    game.set_parent(grand_id, second_id);
    close_gesture(game);
    game.history().reset_waypoints();

    const auto recording = game.history().try_begin_recording("Delete");
    REQUIRE(recording.has_value());
    game.destroy(folder_id);
    game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);

    REQUIRE_FALSE(game.alive(folder_id));
    REQUIRE(game.alive(second_id));
    REQUIRE(game.parent(second_id) == engine_core::DataModel::kNoParent);
    REQUIRE(game.parent(grand_id) == second_id);

    game.history().undo();
    REQUIRE(game.alive(folder_id));
    REQUIRE(game.name(folder_id) == "Box");
    REQUIRE(game.name(first_id) == "Ann");
    REQUIRE(game.name(second_id) == "Bea");
    REQUIRE(game.name(grand_id) == "Cara");
    REQUIRE(game.parent(first_id) == folder_id);
    REQUIRE(game.parent(second_id) == folder_id);
    REQUIRE(game.parent(grand_id) == second_id);
    // A child goes last, so the one parented first is first.
    REQUIRE(game.first_child(folder_id) == first_id);
    REQUIRE(game.next_sibling(first_id) == second_id);
}

TEST_CASE("H15 applying undo does not record a waypoint", "[H15][history]") {
    SimRole role;
    engine_core::Game game;
    game.events().set_policy(engine_core::EventPolicy::Immediate);
    engine_core::GameObject& part = make_part(game, "Brick");
    close_gesture(game);
    game.history().reset_waypoints();

    const engine_core::ColorRgb original = part.color();
    part.set_color(rgb(1.f, 0.f, 0.f));
    close_gesture(game);

    int undos = 0;
    game.history().on_undo.connect([&](const std::string& name) {
        REQUIRE(name == "Set Color");
        ++undos;
    });
    game.property_changed(part.id(), engine_core::Field::Color)
        .connect([&](engine_core::InstanceId, engine_core::Field) {
            REQUIRE(undos == 0);
            game.set_name(part.id(), "from-handler");
            REQUIRE(undos == 0);
        });

    game.history().undo();
    REQUIRE(undos == 1);
    REQUIRE(same_color(part.color(), original));
    REQUIRE(game.name(part.id()) == "from-handler");
    REQUIRE_FALSE(game.history().can_undo().first);
    REQUIRE(game.history().can_redo().second == "Set Color");
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

TEST_CASE("H17 set_parent puts a child last and undo puts it back in its old place", "[H17][history]") {
    engine_core::Game game;
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), 0);
    const engine_core::InstanceId f = folder.id();
    const engine_core::InstanceId a = make_part(game, "A").id();
    const engine_core::InstanceId b = make_part(game, "B").id();
    const engine_core::InstanceId c = make_part(game, "C").id();
    close_gesture(game);
    game.history().reset_waypoints();
    using Ids = std::vector<engine_core::InstanceId>;
    REQUIRE(game.get_children(0) == Ids{f, a, b, c});

    game.history().set_pending_gesture("Move");
    game.set_parent(b, f);
    close_gesture(game);
    game.set_parent(a, f);
    close_gesture(game);
    REQUIRE(game.get_children(f) == Ids{b, a});
    REQUIRE(game.get_children(0) == Ids{f, c});

    game.history().undo();
    game.history().undo();
    REQUIRE(game.get_children(0) == Ids{f, a, b, c});
    game.history().redo();
    REQUIRE(game.get_children(f) == Ids{b});
    REQUIRE(game.get_children(0) == Ids{f, a, c});
}
