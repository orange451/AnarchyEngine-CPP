#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "Events.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"
#include "TaskScheduler.hpp"
#include "ide/InputRouter.hpp"
#include "ide/ScopedRecording.hpp"
#include "types.hpp"

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using engine_core::same_matrix4;

engine_core::GameObject& make_part(engine_core::DataModel& game, const char* name) {
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    game.set_name(part.id(), name);
    game.set_parent(part.id(), workspace_of(game));
    return part;
}

}  // namespace

TEST_CASE("H1 edit create undo restores the same id", "[H1][history]") {
    engine_core::Game game;
    begin_step(game, "Insert GameObject");
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    const engine_core::InstanceId id = part.id();
    game.set_name(id, "Brick");
    const engine_core::Matrix4 placed = engine_core::matrix4_translation(1.f, 2.f, 3.f);
    part.set_transform(placed);
    end_step(game);

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
    REQUIRE(same_matrix4(restored->transform(), placed));
}

TEST_CASE("H2 three transform recordings and a new edit clears redo", "[H2][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();

    const engine_core::Matrix4 a = engine_core::matrix4_translation(1.f, 0.f, 0.f);
    const engine_core::Matrix4 b = engine_core::matrix4_translation(0.f, 1.f, 0.f);
    const engine_core::Matrix4 c = engine_core::matrix4_translation(0.f, 0.f, 1.f);
    const engine_core::Matrix4 d = engine_core::matrix4_translation(1.f, 1.f, 0.f);
    begin_step(game);
    part.set_transform(a);
    end_step(game);
    begin_step(game);
    part.set_transform(b);
    end_step(game);
    begin_step(game);
    part.set_transform(c);
    end_step(game);

    game.history().undo();
    game.history().undo();
    REQUIRE(same_matrix4(part.transform(), a));
    REQUIRE(game.history().can_redo().first);

    const auto redo_before = game.history().can_redo();
    const auto empty = game.history().try_begin_recording("Empty");
    REQUIRE(empty.has_value());
    game.history().finish_recording(*empty, engine_core::FinishRecordingOperation::Commit);
    REQUIRE(game.history().can_redo() == redo_before);

    begin_step(game);
    part.set_transform(d);
    end_step(game);
    REQUIRE_FALSE(game.history().can_redo().first);
    game.history().redo();
    REQUIRE(same_matrix4(part.transform(), d));
}

TEST_CASE("H3 one recording coalesces a drag", "[H3][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    const engine_core::Matrix4 home = part.transform();

    const auto recording = game.history().try_begin_recording("Move");
    REQUIRE(recording.has_value());
    for (int step = 0; step < 50; ++step) {
        part.set_transform(engine_core::matrix4_translation(static_cast<float>(step), 0.f, 0.f));
    }
    game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);

    game.history().undo();
    REQUIRE(same_matrix4(part.transform(), home));
    REQUIRE_FALSE(game.history().can_undo().first);
    REQUIRE(game.history().can_redo().second == "Move");
}

TEST_CASE("H4 writes inside one recording coalesce into one named step", "[H4][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();

    begin_step(game, "Rename");
    game.set_name(part.id(), "A");
    game.set_name(part.id(), "B");
    end_step(game);

    REQUIRE(game.history().can_undo().second == "Rename");
    game.history().undo();
    REQUIRE(game.name(part.id()) == "Brick");
    REQUIRE_FALSE(game.history().can_undo().first);
    game.history().redo();
    REQUIRE(game.name(part.id()) == "B");
}

TEST_CASE("H4b a write outside a recording is not an undo step, but a save writes it", "[H4b][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    game.clear_authored_dirty();

    REQUIRE_FALSE(game.history().wants_mutation());
    game.set_name(part.id(), "A");
    part.set_transform(engine_core::matrix4_translation(1.f, 0.f, 0.f));

    REQUIRE_FALSE(game.history().is_recording_in_progress());
    REQUIRE_FALSE(game.history().can_undo().first);
    const engine_core::AuthoredDirty dirty = game.authored_dirty();
    REQUIRE(std::find(dirty.ids.begin(), dirty.ids.end(), part.id()) != dirty.ids.end());
}

TEST_CASE("H5 play script writes stay off the edit stack", "[H5][history]") {
    SimRole role;
    engine_core::Game game;
    engine_core::TaskScheduler scheduler;
    engine_core::ScriptRuntime runtime;
    scheduler.reserve(8);
    game.attach_scheduler(&scheduler);
    runtime.attach(game, scheduler);

    begin_step(game);
    engine_core::GameObject& part = make_part(game, "Brick");
    const engine_core::Matrix4 authored = engine_core::matrix4_translation(1.f, 2.f, 3.f);
    part.set_transform(authored);
    part.set_linear_velocity(4.f, 0.f, 0.f);
    engine_core::Script& script = game.create<engine_core::Script>();
    game.set_name(script.id(), "Paint");
    script.set_source(R"(
        local part = workspace:FindFirstChild("Brick")
        for _ = 1, 100 do
            part.Transform = Matrix4.new(0, 0, 9)
        end
    )");
    game.set_parent(script.id(), workspace_of(game));
    end_step(game);
    const auto edit = game.history().can_undo();
    REQUIRE(edit.first);

    game.start_simulation();
    const auto during = game.history().can_undo();
    scheduler.run_phase(engine_core::Phase::Heartbeat, 1.0 / 60.0);
    game.events().drain();
    runtime.heartbeat(1.0 / 60.0);
    game.events().drain();
    REQUIRE(runtime.last_error().empty());
    // Checked before integrating, so the script's write is what is read.
    REQUIRE(same_matrix4(part.transform(), engine_core::matrix4_translation(0.f, 0.f, 9.f)));
    game.integrate_simulated(1.0);

    REQUIRE(game.history().can_undo() == during);
    REQUIRE_FALSE(during.first);

    game.stop_simulation();
    REQUIRE(same_matrix4(part.transform(), authored));
    REQUIRE(game.history().can_undo() == edit);
}

TEST_CASE("H6 a play recording undoes, then stop drops it", "[H6][history]") {
    engine_core::Game game;
    begin_step(game);
    engine_core::GameObject& part = make_part(game, "Brick");
    const engine_core::Matrix4 authored = engine_core::matrix4_translation(1.f, 0.f, 0.f);
    part.set_transform(authored);
    end_step(game);
    const auto edit = game.history().can_undo();

    game.start_simulation();
    const auto recording = game.history().try_begin_recording("Play");
    REQUIRE(recording.has_value());
    part.set_transform(engine_core::matrix4_translation(0.f, 0.f, 1.f));
    game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
    REQUIRE(game.history().can_undo().second == "Play");

    game.history().undo();
    REQUIRE(same_matrix4(part.transform(), authored));
    REQUIRE(game.history().can_redo().second == "Play");

    const auto again = game.history().try_begin_recording("Play");
    REQUIRE(again.has_value());
    part.set_transform(engine_core::matrix4_translation(0.f, 1.f, 0.f));
    game.history().finish_recording(*again, engine_core::FinishRecordingOperation::Commit);
    REQUIRE(same_matrix4(part.transform(), engine_core::matrix4_translation(0.f, 1.f, 0.f)));

    game.stop_simulation();
    REQUIRE(same_matrix4(part.transform(), authored));
    REQUIRE(game.history().can_undo() == edit);
    REQUIRE_FALSE(game.history().can_redo().first);

    game.start_simulation();
    REQUIRE_FALSE(game.history().can_undo().first);
    game.stop_simulation();
    REQUIRE(game.history().can_undo() == edit);
}

TEST_CASE("H7 cancel restores the destroyed part and leaves the stacks", "[H7][history]") {
    engine_core::Game game;
    begin_step(game);
    engine_core::GameObject& part = make_part(game, "Brick");
    const engine_core::InstanceId id = part.id();
    end_step(game);
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
    game.history().reset_waypoints();
    const engine_core::Matrix4 original = part.transform();

    const auto first = game.history().try_begin_recording("Paint", "Paint");
    REQUIRE(first.has_value());
    REQUIRE_FALSE(game.history().try_begin_recording("Other").has_value());
    REQUIRE(game.history().is_recording_in_progress(*first));
    part.set_transform(engine_core::matrix4_translation(2.f, 3.f, 4.f));
    game.history().finish_recording("nope", engine_core::FinishRecordingOperation::Commit);
    REQUIRE(game.history().is_recording_in_progress(*first));
    game.history().finish_recording(*first, engine_core::FinishRecordingOperation::Commit);

    REQUIRE(game.history().can_undo().second == "Paint");
    game.history().undo();
    REQUIRE(same_matrix4(part.transform(), original));
}

TEST_CASE("H9 undo during a recording is a no-op", "[H9][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();

    const auto recording = game.history().try_begin_recording("Paint");
    REQUIRE(recording.has_value());
    part.set_transform(engine_core::matrix4_translation(0.f, 1.f, 0.f));
    const engine_core::Matrix4 painted = part.transform();
    game.history().undo();
    game.history().redo();
    REQUIRE(same_matrix4(part.transform(), painted));
    REQUIRE(game.history().is_recording_in_progress(*recording));
    REQUIRE_FALSE(game.history().can_undo().first);

    game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
    game.history().undo();
    REQUIRE_FALSE(same_matrix4(part.transform(), painted));
}

TEST_CASE("H10 script focus undoes text and leaves the place alone", "[H10][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    const engine_core::Matrix4 painted = engine_core::matrix4_translation(1.f, 0.f, 0.f);
    begin_step(game);
    part.set_transform(painted);
    end_step(game);

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
    REQUIRE(same_matrix4(part.transform(), painted));
    REQUIRE(game.history().can_undo().first);

    ide::Focus viewport;
    viewport.kind = ide::FocusKind::Viewport;
    router.set_focus(viewport);
    REQUIRE(router.handle(undo, &game.history()));
    REQUIRE(text.text() == "hi");
    REQUIRE_FALSE(same_matrix4(part.transform(), painted));
}

TEST_CASE("H11 an empty text stack does not undo the place", "[H11][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    begin_step(game);
    part.set_transform(engine_core::matrix4_translation(0.f, 1.f, 0.f));
    end_step(game);
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
    REQUIRE(same_matrix4(part.transform(), engine_core::matrix4_translation(0.f, 1.f, 0.f)));
    REQUIRE_FALSE(game.history().is_recording_in_progress());
}

TEST_CASE("H12 redo follows focus", "[H12][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    const engine_core::Matrix4 painted = engine_core::matrix4_translation(0.f, 0.f, 1.f);
    begin_step(game);
    part.set_transform(painted);
    end_step(game);
    game.history().undo();
    REQUIRE_FALSE(same_matrix4(part.transform(), painted));

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
    REQUIRE_FALSE(same_matrix4(game.game_object(part.id())->transform(), painted));

    text.undo();
    REQUIRE(router.handle(win_redo, &game.history()));
    REQUIRE(text.text() == "ab");

    ide::Focus viewport;
    viewport.kind = ide::FocusKind::Viewport;
    router.set_focus(viewport);
    REQUIRE(router.handle(mac_redo, &game.history()));
    REQUIRE(same_matrix4(game.game_object(part.id())->transform(), painted));
    game.history().undo();
    REQUIRE(router.handle(win_redo, &game.history()));
    REQUIRE(same_matrix4(game.game_object(part.id())->transform(), painted));
}

TEST_CASE("H13 undo destroy restores children and names", "[H13][history]") {
    engine_core::Game game;
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    const engine_core::InstanceId folder_id = folder.id();
    game.set_name(folder_id, "Box");
    game.set_parent(folder_id, workspace_of(game));

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
    game.history().reset_waypoints();

    const engine_core::Matrix4 original = part.transform();
    begin_step(game, "Move");
    part.set_transform(engine_core::matrix4_translation(1.f, 0.f, 0.f));
    end_step(game);

    int undos = 0;
    game.history().on_undo.connect([&](const std::string& name) {
        REQUIRE(name == "Move");
        ++undos;
    });
    game.property_changed(part.id(), engine_core::Field::Transform)
        .connect([&](engine_core::InstanceId, engine_core::Field) {
            REQUIRE(undos == 0);
            game.set_name(part.id(), "from-handler");
            REQUIRE(undos == 0);
        });

    game.history().undo();
    REQUIRE(undos == 1);
    REQUIRE(same_matrix4(part.transform(), original));
    REQUIRE(game.name(part.id()) == "from-handler");
    REQUIRE_FALSE(game.history().can_undo().first);
    REQUIRE(game.history().can_redo().second == "Move");
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
    game.set_parent(folder.id(), workspace_of(game));
    const engine_core::InstanceId f = folder.id();
    const engine_core::InstanceId a = make_part(game, "A").id();
    const engine_core::InstanceId b = make_part(game, "B").id();
    const engine_core::InstanceId c = make_part(game, "C").id();
    game.history().reset_waypoints();
    using Ids = std::vector<engine_core::InstanceId>;
    REQUIRE(game.get_children(workspace_of(game)) == Ids{f, a, b, c});

    begin_step(game, "Move");
    game.set_parent(b, f);
    end_step(game);
    begin_step(game);
    game.set_parent(a, f);
    end_step(game);
    REQUIRE(game.get_children(f) == Ids{b, a});
    REQUIRE(game.get_children(workspace_of(game)) == Ids{f, c});

    game.history().undo();
    game.history().undo();
    REQUIRE(game.get_children(workspace_of(game)) == Ids{f, a, b, c});
    game.history().redo();
    REQUIRE(game.get_children(f) == Ids{b});
    REQUIRE(game.get_children(workspace_of(game)) == Ids{f, a, c});
}

namespace {

int undo_all(engine_core::DataModel& game) {
    int count = 0;
    while (game.history().can_undo().first) {
        game.history().undo();
        ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("H18 edit history keeps the newest waypoints up to its count", "[H18][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    game.history().set_limits(5, 1u << 30);
    for (int i = 0; i < 8; ++i) {
        // From 1, since a move to the origin changes nothing and records nothing.
        begin_step(game);
        part.set_transform(engine_core::matrix4_translation(static_cast<float>(i + 1), 0.f, 0.f));
        end_step(game);
    }
    REQUIRE(undo_all(game) == 5);
    // The oldest three are gone: undo stops at the transform the third edit made.
    REQUIRE(same_matrix4(part.transform(), engine_core::matrix4_translation(3.f, 0.f, 0.f)));
}

TEST_CASE("H19 edit history keeps its newest waypoints within its size", "[H19][history]") {
    engine_core::Game game;
    engine_core::Script& script = add_script(game, "Big", "");
    game.history().reset_waypoints();
    // Each edit keeps the text before and after, about 200 KB, so 1 MB holds four.
    game.history().set_limits(1000, 1u << 20);
    for (int i = 0; i < 10; ++i) {
        begin_step(game);
        script.set_source(std::string(100 * 1024, static_cast<char>('a' + i)));
        end_step(game);
    }
    const int kept = undo_all(game);
    REQUIRE(kept >= 1);
    REQUIRE(kept <= 5);
    REQUIRE(script.source() == std::string(100 * 1024, static_cast<char>('a' + 10 - kept - 1)));
}

TEST_CASE("H20 the newest waypoint stays however large it is", "[H20][history]") {
    engine_core::Game game;
    engine_core::Script& script = add_script(game, "Huge", "small");
    game.history().reset_waypoints();
    game.history().set_limits(1000, 1024);
    begin_step(game);
    script.set_source(std::string(64 * 1024, 'x'));
    end_step(game);
    REQUIRE(undo_all(game) == 1);
    REQUIRE(script.source() == "small");
}

namespace {

int undo_text(ide::TextUndoStack& text) {
    int count = 0;
    while (text.can_undo()) {
        text.undo();
        ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("H21 a text stack keeps its newest edits up to its count", "[H21][history]") {
    ide::TextUndoStack text;
    text.set_limits(5, 1u << 30);
    text.reset("");
    for (char c = 'a'; c < 'a' + 8; ++c) {
        text.insert(text.text().size(), std::string(1, c));
    }
    REQUIRE(undo_text(text) == 5);
    REQUIRE(text.text() == "abc");
}

TEST_CASE("H22 a text stack keeps its newest edits within its size", "[H22][history]") {
    ide::TextUndoStack text;
    // Each edit keeps about 1 KB, so 4 KB holds a few of them.
    text.set_limits(1000, 4096);
    text.reset("");
    for (int i = 0; i < 20; ++i) {
        text.insert(text.text().size(), std::string(1024, static_cast<char>('a' + i)));
    }
    const int kept = undo_text(text);
    REQUIRE(kept >= 1);
    REQUIRE(kept <= 4);
    REQUIRE(text.text().size() == static_cast<std::size_t>(20 - kept) * 1024);
}

TEST_CASE("H23 the newest text edit stays however large it is", "[H23][history]") {
    ide::TextUndoStack text;
    text.set_limits(1000, 16);
    text.reset("keep");
    text.insert(4, std::string(4096, 'z'));
    REQUIRE(undo_text(text) == 1);
    REQUIRE(text.text() == "keep");
}

TEST_CASE("H24 the router forgets a script's text stack", "[H24][history]") {
    ide::InputRouter router;
    router.script_stack(7).reset("seven");
    router.script_stack(8).reset("eight");
    ide::Focus focus;
    focus.kind = ide::FocusKind::ScriptEditor;
    focus.script = 7;
    router.set_focus(focus);
    REQUIRE(router.focused_stack() != nullptr);
    router.forget_script(7);
    REQUIRE(router.focused_stack() == nullptr);
    focus.script = 8;
    router.set_focus(focus);
    REQUIRE(router.focused_stack() != nullptr);
    router.forget_scripts();
    REQUIRE(router.focused_stack() == nullptr);
}

TEST_CASE("H25 a ScopedRecording is one named step, committed when it leaves scope", "[H25][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    {
        ide::ScopedRecording step(game, "Rename");
        REQUIRE(game.history().is_recording_in_progress());
        game.set_name(part.id(), "A");
        game.set_name(part.id(), "B");
    }
    REQUIRE_FALSE(game.history().is_recording_in_progress());
    REQUIRE(game.history().can_undo().second == "Rename");
    game.history().undo();
    REQUIRE(game.name(part.id()) == "Brick");
}

TEST_CASE("H26 a ScopedRecording inside an open recording joins it and leaves it open", "[H26][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    const std::optional<std::string> drag = game.history().try_begin_recording("Move");
    REQUIRE(drag.has_value());
    {
        // As an IDE command issued while a drag is in progress.
        ide::ScopedRecording step(game, "Rename");
        game.set_name(part.id(), "A");
    }
    REQUIRE(game.history().is_recording_in_progress(*drag));
    game.history().finish_recording(*drag, engine_core::FinishRecordingOperation::Commit);
    REQUIRE(game.history().can_undo().second == "Move");
    game.history().undo();
    REQUIRE(game.name(part.id()) == "Brick");
}

TEST_CASE("H27 redo brings back a created instance that a write outside any recording destroyed, into its own slot",
          "[H27][history]") {
    engine_core::Game game;
    game.history().reset_waypoints();
    begin_step(game, "Insert GameObject");
    const engine_core::InstanceId id = make_part(game, "Brick").id();
    end_step(game);

    // As the command line does: a destroy and a create outside any recording.
    game.destroy(id);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    const engine_core::InstanceId other = folder.id();
    game.set_parent(other, workspace_of(game));

    // Undo finds nothing to take away; redo brings the instance back.
    game.history().undo();
    REQUIRE_FALSE(game.alive(id));
    game.history().redo();
    REQUIRE(game.alive(id));
    REQUIRE(game.name(id) == "Brick");
    REQUIRE(game.parent(id) == workspace_of(game));
    REQUIRE(game.alive(other));
    REQUIRE(engine_core::id_slot(other) != engine_core::id_slot(id));
}

TEST_CASE("H28 undo brings back a deleted instance that a write outside any recording destroyed again, into its own slot",
          "[H28][history]") {
    engine_core::Game game;
    const engine_core::InstanceId id = make_part(game, "Brick").id();
    game.history().reset_waypoints();
    begin_step(game, "Delete");
    game.destroy(id);
    end_step(game);
    game.history().undo();
    REQUIRE(game.alive(id));

    // As the command line does: a destroy and a create outside any recording.
    game.destroy(id);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    const engine_core::InstanceId other = folder.id();
    game.set_parent(other, workspace_of(game));

    // Redo finds nothing to delete; undo brings the instance back.
    game.history().redo();
    REQUIRE_FALSE(game.alive(id));
    game.history().undo();
    REQUIRE(game.alive(id));
    REQUIRE(game.name(id) == "Brick");
    REQUIRE(game.parent(id) == workspace_of(game));
    REQUIRE(game.alive(other));
    REQUIRE(engine_core::id_slot(other) != engine_core::id_slot(id));
}

TEST_CASE("H29 the place is dirty once a recording holds a change, and after undo and redo", "[H29][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    REQUIRE_FALSE(game.history().dirty());

    // A write no recording covers does not dirty.
    game.set_name(part.id(), "Loose");
    REQUIRE_FALSE(game.history().dirty());

    begin_step(game, "Rename");
    game.set_name(part.id(), "A");
    end_step(game);
    REQUIRE(game.history().dirty());

    game.history().mark_saved();
    REQUIRE_FALSE(game.history().dirty());
    game.history().undo();
    REQUIRE(game.history().dirty());

    game.history().mark_saved();
    game.history().redo();
    REQUIRE(game.history().dirty());
}

TEST_CASE("H30 a cancelled recording, and one that commits nothing, leave the place clean", "[H30][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();

    const std::optional<std::string> cancelled = game.history().try_begin_recording("Rename");
    game.set_name(part.id(), "A");
    game.history().finish_recording(*cancelled, engine_core::FinishRecordingOperation::Cancel);
    REQUIRE(game.name(part.id()) == "Brick");
    REQUIRE_FALSE(game.history().dirty());

    begin_step(game, "Nothing");
    end_step(game);
    REQUIRE_FALSE(game.history().dirty());

    // There and back coalesces to no change at all.
    begin_step(game, "Rename");
    game.set_name(part.id(), "A");
    game.set_name(part.id(), "Brick");
    end_step(game);
    REQUIRE_FALSE(game.history().can_undo().first);
    REQUIRE_FALSE(game.history().dirty());
}

TEST_CASE("H31 a recording left open still makes the place dirty", "[H31][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();

    // As a plugin that errors after TryBeginRecording leaves it.
    REQUIRE(game.history().try_begin_recording("Stuck").has_value());
    game.set_name(part.id(), "A");
    REQUIRE(game.history().dirty());
}

TEST_CASE("H32 a play recording and Stop do not dirty the place", "[H32][history]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();
    game.capture_place();

    game.start_simulation();
    begin_step(game, "Play Move");
    part.set_transform(engine_core::matrix4_translation(1.f, 0.f, 0.f));
    end_step(game);
    REQUIRE_FALSE(game.history().dirty());
    game.history().undo();
    REQUIRE_FALSE(game.history().dirty());

    game.stop_simulation();
    REQUIRE_FALSE(game.history().dirty());
}

TEST_CASE("H33 writes that change a file without entering history dirty the place", "[H33][history]") {
    engine_core::Game game;
    engine_core::GameObject& part = make_part(game, "Brick");
    game.history().reset_waypoints();

    game.set_extra_property(part.id(), "Note", engine_core::JsonValue::string("kept"));
    REQUIRE(game.history().dirty());

    game.history().mark_saved();
    game.erase_extra_property(part.id(), "Note");
    REQUIRE(game.history().dirty());

    game.history().mark_saved();
    game.set_guid(part.id(), "abcd");
    REQUIRE(game.history().dirty());

    game.history().mark_saved();
    game.set_archivable(part.id(), false);
    REQUIRE(game.history().dirty());

    // Core is not the place.
    game.history().mark_saved();
    engine_core::GameObject& tool = game.create<engine_core::GameObject>();
    game.set_parent(tool.id(), game.core());
    game.set_extra_property(tool.id(), "Note", engine_core::JsonValue::string("x"));
    REQUIRE_FALSE(game.history().dirty());
}
