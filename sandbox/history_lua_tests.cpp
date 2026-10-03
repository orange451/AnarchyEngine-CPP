#include "ChangeHistoryService.hpp"
#include "ide/PluginLoader.hpp"
#include "ide/ScopedRecording.hpp"

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

std::vector<std::string> lines(engine_core::ScriptRuntime& runtime) {
    std::vector<std::string> out;
    for (const auto& line : runtime.drain_output().lines) {
        out.push_back(line.text);
    }
    return out;
}

bool has(const std::vector<std::string>& all, const std::string& line) {
    return std::find(all.begin(), all.end(), line) != all.end();
}

// Whether a line holds every one of these pieces.
bool has_line_with(const std::vector<std::string>& all, std::initializer_list<const char*> pieces) {
    return std::any_of(all.begin(), all.end(), [&](const std::string& line) {
        return std::all_of(pieces.begin(), pieces.end(),
                           [&](const char* piece) { return line.find(piece) != std::string::npos; });
    });
}

engine_core::GameObject& brick(ScriptRig& rig) {
    engine_core::GameObject& part = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(part.id(), "Brick");
    rig.game.set_parent(part.id(), workspace_of(rig.game));
    rig.game.history().reset_waypoints();
    return part;
}

}  // namespace

TEST_CASE("HL1 a chunk's write is an undo step only when the chunk records it", "[HL1][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);

    rig.runtime.run_chunk("workspace.Brick.Name = 'Loose'");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(rig.game.name(part.id()) == "Loose");
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    REQUIRE_FALSE(rig.game.history().dirty());

    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        local id = history:TryBeginRecording("Rename Brick")
        workspace.Loose.Name = "Kept"
        history:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
    )");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(rig.game.history().can_undo().second == "Rename Brick");
    REQUIRE(rig.game.history().dirty());
    rig.game.history().undo();
    REQUIRE(rig.game.name(part.id()) == "Loose");
}

TEST_CASE("HL2 FinishRecording with Cancel puts the value back", "[HL2][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        local id = history:TryBeginRecording("Rename")
        workspace.Brick.Name = "Gone"
        history:FinishRecording(id, Enum.FinishRecordingOperation.Cancel)
    )");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(rig.game.name(part.id()) == "Brick");
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    REQUIRE_FALSE(rig.game.history().dirty());
}

TEST_CASE("HL3 the queries name the step and the signals fire with their arguments", "[HL3][history]") {
    ScriptRig rig;
    brick(rig);
    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        history.OnRecordingStarted:Connect(function(name, display) print("started", name, display) end)
        history.OnRecordingFinished:Connect(function(name, display, id, op) print("finished", name, display, op) end)
        history.OnUndo:Connect(function(name) print("undo", name) end)
        history.OnRedo:Connect(function(name) print("redo", name) end)
        print("second", history:TryBeginRecording("Outer") ~= nil, history:TryBeginRecording("Inner"))
        history:SetWaypoint("Outer")
        local id = history:TryBeginRecording("Rename", "Rename Brick")
        print("open", history:IsRecordingInProgress(), history:IsRecordingInProgress(id), history:IsRecordingInProgress("0"))
        workspace.Brick.Name = "B"
        history:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
        print("can undo", history:GetCanUndo())
        history:Undo()
        print("can redo", history:GetCanRedo())
        history:Redo()
        history:ResetWaypoints()
        print("after reset", history:GetCanUndo())
    )");
    rig.frames(1);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    const std::vector<std::string> all = lines(rig.runtime);
    REQUIRE(has(all, "second\ttrue\tnil\n"));
    REQUIRE(has(all, "open\ttrue\ttrue\tfalse\n"));
    REQUIRE(has(all, "can undo\ttrue\tRename Brick\n"));
    REQUIRE(has(all, "can redo\ttrue\tRename Brick\n"));
    REQUIRE(has(all, "after reset\tfalse\t\n"));
    REQUIRE(has(all, "started\tRename\tRename Brick\n"));
    REQUIRE(has(all, "finished\tRename\tRename Brick\tEnum.FinishRecordingOperation.Commit\n"));
    REQUIRE(has(all, "undo\tRename Brick\n"));
    REQUIRE(has(all, "redo\tRename Brick\n"));
}

TEST_CASE("HL4 a stale id is a no-op and a bad operation is an error a pcall catches", "[HL4][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        local id = history:TryBeginRecording("Rename")
        workspace.Brick.Name = "A"
        history:FinishRecording("no such id", Enum.FinishRecordingOperation.Commit)
        print("still open", history:IsRecordingInProgress(id))
        print("bad op", pcall(function() history:FinishRecording(id, "Sideways") end))
        print("still open", history:IsRecordingInProgress(id))
        history:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
    )");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    const std::vector<std::string> all = lines(rig.runtime);
    REQUIRE(std::count(all.begin(), all.end(), "still open\ttrue\n") == 2);
    REQUIRE(std::any_of(all.begin(), all.end(),
                        [](const std::string& line) { return line.rfind("bad op\tfalse", 0) == 0; }));
    REQUIRE(rig.game.history().can_undo().second == "Rename");
    REQUIRE(rig.game.name(part.id()) == "A");
}

TEST_CASE("HL5 a chunk that errors with its recording open cancels it, so undo works again", "[HL5][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    begin_step(rig.game, "Move");
    part.set_position({1.f, 2.f, 3.f});
    end_step(rig.game);

    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        history:TryBeginRecording("Paint")
        workspace.Brick.Name = "Painted"
        error("boom")
    )");
    REQUIRE(rig.runtime.last_error().find("boom") != std::string::npos);
    REQUIRE_FALSE(rig.game.history().is_recording_in_progress());
    REQUIRE(rig.game.name(part.id()) == "Brick");
    REQUIRE(has_line_with(lines(rig.runtime), {"\"Paint\"", "cancelled"}));

    // A studio command after it is its own step, and undo reaches it.
    {
        ide::ScopedRecording step(rig.game, "Rename");
        rig.game.set_name(part.id(), "Renamed");
    }
    REQUIRE(rig.game.history().can_undo().second == "Rename");
    rig.game.history().undo();
    REQUIRE(rig.game.name(part.id()) == "Brick");
    REQUIRE(rig.game.history().can_undo().second == "Move");
}

TEST_CASE("HL6 a chunk that finishes with its recording open commits it", "[HL6][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        history:TryBeginRecording("Forgot")
        workspace.Brick.Name = "Kept"
    )");
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE_FALSE(rig.game.history().is_recording_in_progress());
    REQUIRE(rig.game.name(part.id()) == "Kept");
    REQUIRE(rig.game.history().can_undo().second == "Forgot");
    REQUIRE(has_line_with(lines(rig.runtime), {"\"Forgot\"", "committed"}));
    rig.game.history().undo();
    REQUIRE(rig.game.name(part.id()) == "Brick");
}

TEST_CASE("HL7 a chunk that yields with its recording open closes it when it finishes, not before", "[HL7][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        history:TryBeginRecording("Slow")
        workspace.Brick.Name = "A"
        task.wait(0.1)
        workspace.A.Name = "B"
    )");
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(rig.game.history().is_recording_in_progress());
    rig.frames(1);
    REQUIRE(rig.game.history().is_recording_in_progress());
    REQUIRE_FALSE(has_line_with(lines(rig.runtime), {"\"Slow\""}));

    rig.frames(12);
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE_FALSE(rig.game.history().is_recording_in_progress());
    REQUIRE(rig.game.name(part.id()) == "B");
    REQUIRE(rig.game.history().can_undo().second == "Slow");
    REQUIRE(has_line_with(lines(rig.runtime), {"\"Slow\"", "committed"}));
    rig.game.history().undo();
    REQUIRE(rig.game.name(part.id()) == "Brick");
}

TEST_CASE("HL8 a plugin may hold a recording past its thread, and unregistering it commits the recording",
          "[HL8][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    engine_core::Script& plugin = rig.game.create<engine_core::Script>();
    rig.game.set_name(plugin.id(), "Painter");
    plugin.set_source(R"(
        local history = game:GetService("ChangeHistoryService")
        history:TryBeginRecording("Drag")
        workspace.Brick.Name = "Dragged"
    )");
    REQUIRE(rig.runtime.register_plugin(plugin.id()));
    REQUIRE(rig.runtime.last_error().empty());
    rig.frames(2);
    REQUIRE(rig.game.history().is_recording_in_progress());

    REQUIRE(rig.runtime.unregister_plugin(plugin.id()));
    REQUIRE_FALSE(rig.game.history().is_recording_in_progress());
    REQUIRE(rig.game.name(part.id()) == "Dragged");
    REQUIRE(rig.game.history().can_undo().second == "Drag");
    REQUIRE(has_line_with(lines(rig.runtime), {"\"Drag\"", "committed"}));
}

TEST_CASE("HL9 reloading the plugins commits a recording one of them left open", "[HL9][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    const std::vector<ide::PluginFile> files{{"Painter", R"(
        local history = game:GetService("ChangeHistoryService")
        history:TryBeginRecording("Drag")
        workspace.Brick.Name = "Dragged"
    )"}};
    ide::PluginLoader loader;
    REQUIRE(loader.load(rig.game, rig.runtime, files) == 1);
    REQUIRE(rig.game.history().is_recording_in_progress());

    // The reloaded plugin opens a recording again, after the old one is committed.
    REQUIRE(loader.load(rig.game, rig.runtime, files) == 1);
    REQUIRE(has_line_with(lines(rig.runtime), {"\"Drag\"", "committed"}));
    REQUIRE(rig.game.history().is_recording_in_progress());
    rig.game.history().seal_edit_recording();
    REQUIRE(rig.game.history().can_undo().second == "Drag");
    rig.game.history().undo();
    REQUIRE(rig.game.name(part.id()) == "Brick");
}

TEST_CASE("HL10 FinishRecordingOperation has Roblox's values: Cancel 0, Commit 1", "[HL10][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        history.OnRecordingFinished:Connect(function(name, display, id, op) print("finished", name, op, op.Value) end)
        print("values", Enum.FinishRecordingOperation.Cancel.Value, Enum.FinishRecordingOperation.Commit.Value)
        local one = history:TryBeginRecording("One")
        workspace.Brick.Name = "A"
        history:FinishRecording(one, 1)
        local two = history:TryBeginRecording("Two")
        workspace.A.Name = "B"
        history:FinishRecording(two, 0)
    )");
    rig.frames(1);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(rig.game.name(part.id()) == "A");
    REQUIRE(rig.game.history().can_undo().second == "One");
    const std::vector<std::string> all = lines(rig.runtime);
    REQUIRE(has(all, "values\t0\t1\n"));
    REQUIRE(has(all, "finished\tOne\tEnum.FinishRecordingOperation.Commit\t1\n"));
    REQUIRE(has(all, "finished\tTwo\tEnum.FinishRecordingOperation.Cancel\t0\n"));
}

TEST_CASE("HL11 ResetWaypoints during play drops only the play steps, and in edit mode every step", "[HL11][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    begin_step(rig.game, "Rename");
    rig.game.set_name(part.id(), "Renamed");
    end_step(rig.game);
    rig.game.capture_place();

    rig.game.start_simulation();
    // A game script during play: a step of its own, then a reset.
    add_script(rig.game, "Resetter", R"(
        local history = game:GetService("ChangeHistoryService")
        local id = history:TryBeginRecording("Play Move")
        workspace.Renamed.Name = "Moved"
        history:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
        print("before", history:GetCanUndo())
        history:TryBeginRecording("Left Open")
        history:ResetWaypoints()
        print("after", history:GetCanUndo(), history:IsRecordingInProgress())
    )");
    rig.frames(1);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    const std::vector<std::string> all = lines(rig.runtime);
    REQUIRE(has(all, "before\ttrue\tPlay Move\n"));
    REQUIRE(has(all, "after\tfalse\tfalse\n"));

    rig.game.stop_simulation();
    REQUIRE(rig.game.history().can_undo().second == "Rename");

    rig.runtime.run_chunk("game:GetService('ChangeHistoryService'):ResetWaypoints()");
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE_FALSE(rig.game.history().can_undo().first);
}

TEST_CASE("HL12 a recording opened inside a coroutine closes with the thread that ran it", "[HL12][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    rig.runtime.run_chunk(R"(
        coroutine.wrap(function()
            game:GetService("ChangeHistoryService"):TryBeginRecording("Wrapped")
            workspace.Brick.Name = "Wrapped"
        end)()
    )");
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE_FALSE(rig.game.history().is_recording_in_progress());
    REQUIRE(rig.game.history().can_undo().second == "Wrapped");
    REQUIRE(has_line_with(lines(rig.runtime), {"\"Wrapped\"", "committed"}));
    rig.game.history().undo();
    REQUIRE(rig.game.name(part.id()) == "Brick");
}

TEST_CASE("HL13 resetting the console commits a recording a waiting chunk holds", "[HL13][history]") {
    ScriptRig rig;
    engine_core::GameObject& part = brick(rig);
    rig.runtime.run_chunk(R"(
        game:GetService("ChangeHistoryService"):TryBeginRecording("Waiting")
        workspace.Brick.Name = "Waited"
        task.wait(10)
    )");
    REQUIRE(rig.game.history().is_recording_in_progress());
    rig.runtime.reset_console();
    REQUIRE_FALSE(rig.game.history().is_recording_in_progress());
    REQUIRE(rig.game.name(part.id()) == "Waited");
    REQUIRE(rig.game.history().can_undo().second == "Waiting");
    REQUIRE(has_line_with(lines(rig.runtime), {"\"Waiting\"", "committed"}));
}
