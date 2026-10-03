#include "ChangeHistoryService.hpp"

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
    const std::vector<std::string> all = lines(rig.runtime);
    REQUIRE(std::count(all.begin(), all.end(), "still open\ttrue\n") == 2);
    REQUIRE(std::any_of(all.begin(), all.end(),
                        [](const std::string& line) { return line.rfind("bad op\tfalse", 0) == 0; }));
    REQUIRE(rig.game.history().can_undo().second == "Rename");
    REQUIRE(rig.game.name(part.id()) == "A");
}
