// Terrain's Lua methods.

#include "support.hpp"

#include "Terrain.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

bool has_line(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const auto& line : batch.lines) {
        if (line.text == text) {
            return true;
        }
    }
    return false;
}

std::string all_text(const engine_core::ScriptRuntime::OutputBatch& batch) {
    std::string out;
    for (const auto& line : batch.lines) {
        out += line.text;
    }
    return out;
}

}  // namespace

TEST_CASE("TL1 FillBall and ReadVoxels agree", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        local rock = t:AddMaterial(nil)
        t:FillBall(Vector3.new(0, 0, 0), 4, rock)
        local v = t:ReadVoxels(Vector3.new(0, 0, 0), Vector3.new(0, 0, 0))
        print(v.Materials[1][1][1] == rock.Id, v.Distances[1][1][1] < 0)
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    INFO(all_text(out));
    REQUIRE(has_line(out, "true\ttrue\n"));
}

TEST_CASE("TL2 Local space follows a moved and turned Terrain; World space does not", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        t.Transform = Matrix4.new(100, 0, 0) * Matrix4.Angles(0, math.pi / 2, 0)
        t:FillBall(Vector3.new(0, 0, 0), 3, nil, Enum.TransformSpace.Local)
        t:FillBall(Vector3.new(100, 0, 20), 3, nil)
        local here = t:WorldToCell(Vector3.new(100, 0, 0))
        local far = t:WorldToCell(Vector3.new(100, 0, 20))
        local a = t:ReadVoxels(here, here).Distances[1][1][1]
        local b = t:ReadVoxels(far, far).Distances[1][1][1]
        print(here == Vector3.new(0, 0, 0), a < 0, b < 0)
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    INFO(all_text(out));
    REQUIRE(has_line(out, "true\ttrue\ttrue\n"));
}

TEST_CASE("TL3 Subtract, Paint, and ReplaceMaterial", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        local a, b = t:AddMaterial(nil), t:AddMaterial(nil)
        t:FillBlock(Matrix4.new(0, 0, 0), Vector3.new(10, 10, 10), a)
        t:SubtractBall(Vector3.new(0, 0, 0), 2)
        t:PaintBlock(Matrix4.new(3, 0, 0), Vector3.new(2, 2, 2), b)
        local function at(x) return t:ReadVoxels(Vector3.new(x, 0, 0), Vector3.new(x, 0, 0)) end
        print(at(0).Distances[1][1][1] > 0, at(3).Materials[1][1][1] == b.Id)
        t:ReplaceMaterial(Vector3.new(-10, -10, -10), Vector3.new(10, 10, 10), b, a)
        print(at(3).Materials[1][1][1] == a.Id)
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    INFO(all_text(out));
    REQUIRE(has_line(out, "true\ttrue\n"));
    REQUIRE(has_line(out, "true\n"));
}

TEST_CASE("TL4 materials must be this Terrain's TerrainMaterials", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        local u = Instance.new("Terrain", workspace)
        local mine = u:AddMaterial(nil)
        print(select(2, pcall(function() t:FillBall(Vector3.new(), 2, mine) end)))
        print(select(2, pcall(function() t:FillBall(Vector3.new(), 2, workspace) end)))
    )");
    rig.frames(1);
    const std::string text = all_text(rig.runtime.drain_output());
    INFO(text);
    REQUIRE(text.find("TerrainMaterial belongs to another Terrain") != std::string::npos);
    REQUIRE(text.find("Pass a TerrainMaterial (see Terrain:GetMaterials)") != std::string::npos);
}

TEST_CASE("TL5 too large an edit raises", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        print(select(2, pcall(function() t:FillBlock(Matrix4.new(), Vector3.new(400, 400, 400), nil) end)))
    )");
    rig.frames(1);
    REQUIRE(all_text(rig.runtime.drain_output()).find("Terrain edit too large: split it into smaller calls") !=
            std::string::npos);
}

TEST_CASE("TL6 a zero or negative radius changes nothing", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        t:FillBall(Vector3.new(), 0, nil)
        t:FillBall(Vector3.new(), -3, nil)
        print(t:ReadVoxels(Vector3.new(), Vector3.new()).Distances[1][1][1] > 0)
    )");
    rig.frames(1);
    REQUIRE(has_line(rig.runtime.drain_output(), "true\n"));
}

TEST_CASE("TL11 SmoothBall rounds a corner; strength 0 changes nothing; strength is held to 0 to 1",
          "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        t:FillBlock(Matrix4.new(0, 0, 0), Vector3.new(8, 8, 8), nil)
        local corner = Vector3.new(4, 4, 4)
        local function at() return t:ReadVoxels(corner, corner).Distances[1][1][1] end
        local before = at()
        t:SmoothBall(corner, 3, 0)
        local still = at()
        t:SmoothBall(corner, 3, 5)
        local after = at()
        print(still == before, after > before)
        print(select(2, pcall(function() t:SmoothBall(corner, 3, 0 / 0) end)))
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    INFO(all_text(out));
    REQUIRE(has_line(out, "true\ttrue\n"));
    REQUIRE(all_text(out).find("strength must be a finite number") != std::string::npos);
}

TEST_CASE("TL10 a box too large to count in cells raises and changes nothing", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        local u = Instance.new("Terrain", workspace)
        local a, b = u:AddMaterial(nil), u:AddMaterial(nil)
        u:FillBall(Vector3.new(), 2, a)
        local lo, hi = Vector3.new(-1e9, -1e9, -1e9), Vector3.new(1e9, 1e9, 1e9)
        print(select(2, pcall(function() t:FillBall(Vector3.new(), 1e12, nil) end)))
        print(select(2, pcall(function() t:FillBall(Vector3.new(), math.huge, nil) end)))
        print(select(2, pcall(function() t:FillBall(Vector3.new(), 1e300, nil) end)))
        print(select(2, pcall(function() t:FillBall(Vector3.new(), 1e9, nil) end)))
        print(select(2, pcall(function() t:ReadVoxels(lo, hi) end)))
        print(select(2, pcall(function() u:ReplaceMaterial(lo, hi, a, b) end)))
        local cell = u:ReadVoxels(Vector3.new(), Vector3.new())
        print(t:ReadVoxels(Vector3.new(), Vector3.new()).Distances[1][1][1] > 0, cell.Materials[1][1][1] == a.Id)
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    const std::string text = all_text(out);
    INFO(text);
    const std::string message = "Terrain edit too large: split it into smaller calls";
    int raised = 0;
    for (std::size_t at = text.find(message); at != std::string::npos; at = text.find(message, at + message.size())) {
        ++raised;
    }
    REQUIRE(raised == 6);
    REQUIRE(has_line(out, "true\ttrue\n"));
}

TEST_CASE("TL7 material lookups", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        local a, b = t:AddMaterial(nil), t:AddMaterial(nil)
        print(#t:GetMaterials(), t:GetMaterialById(2) == b, t:GetMaterialById(9))
        t:RemoveMaterial(a)
        print(#t:GetMaterials(), t:AddMaterial(nil).Id)
        print(pcall(function() b.Id = 7 end))
        print(pcall(function() b.Parent = workspace end))
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    INFO(all_text(out));
    REQUIRE(has_line(out, "2\ttrue\tnil\n"));
    REQUIRE(has_line(out, "1\t1\n"));
    REQUIRE(all_text(out).find("TerrainMaterial cannot be reparented") != std::string::npos);
}

TEST_CASE("TL8 WriteVoxels round-trips ReadVoxels exactly, unassigned Ids included", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        local u = Instance.new("Terrain", workspace)
        local lo, hi = Vector3.new(-3, -3, -3), Vector3.new(3, 3, 3)
        local d, m = {}, {}
        for x = 1, 7 do d[x], m[x] = {}, {}
            for y = 1, 7 do d[x][y], m[x][y] = {}, {}
                for z = 1, 7 do d[x][y][z] = (x + y + z) / 4 - 3; m[x][y][z] = 42 end end end
        t:WriteVoxels(lo, d, m)
        local v = t:ReadVoxels(lo, hi)
        u:WriteVoxels(lo, v.Distances, v.Materials)
        local w = u:ReadVoxels(lo, hi)
        local same = true
        for x = 1, 7 do for y = 1, 7 do for z = 1, 7 do
            same = same and w.Distances[x][y][z] == v.Distances[x][y][z] and w.Materials[x][y][z] == v.Materials[x][y][z]
        end end end
        print(same, v.Materials[1][1][1])
        print(select(2, pcall(function() t:WriteVoxels(lo, {{{0}}}, {{{1.5}}}) end)))
        print(select(2, pcall(function() t:WriteVoxels(lo, {{{0}}}, {{{256}}}) end)))
        t:WriteVoxels(lo, {{{1e300}}}, {{{0}}})
        local far = t:ReadVoxels(lo, lo).Distances[1][1][1]
        t:WriteVoxels(lo, {{{-1e300}}}, {{{0}}})
        print("huge", far == 4, t:ReadVoxels(lo, lo).Distances[1][1][1] == -4)
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    const std::string text = all_text(out);
    INFO(text);
    REQUIRE(has_line(out, "true\t42\n"));
    REQUIRE(has_line(out, "huge\ttrue\ttrue\n"));
    REQUIRE(text.find("material Ids must be whole numbers from 0 to 255") != std::string::npos);
}

TEST_CASE("TL9 CellToWorld inverts WorldToCell; Clear empties the Terrain but keeps materials", "[terrain][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        t.Transform = Matrix4.new(5, 6, 7)
        local m = t:AddMaterial(nil)
        t:FillBall(Vector3.new(5, 6, 7), 3, m)
        print(t:CellToWorld(t:WorldToCell(Vector3.new(8, 6, 7))) == Vector3.new(8, 6, 7))
        t:Clear()
        print(t:ReadVoxels(Vector3.new(), Vector3.new()).Distances[1][1][1] > 0, #t:GetMaterials())
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    REQUIRE(has_line(out, "true\n"));
    REQUIRE(has_line(out, "true\t1\n"));
}

namespace {

// Prints whether the cells at the origin and at (10, 0, 0) are solid.
std::string solids(ScriptRig& rig) {
    rig.runtime.run_chunk(R"(
        local t = workspace.Island
        local function solid(x)
            return t:ReadVoxels(Vector3.new(x, 0, 0), Vector3.new(x, 0, 0)).Distances[1][1][1] < 0
        end
        print(solid(0), solid(10))
    )");
    rig.frames(1);
    return all_text(rig.runtime.drain_output());
}

}  // namespace

TEST_CASE("TU1 terrain edits inside a ChangeHistoryService recording undo and redo as one step", "[terrain][lua][history]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        t.Name = "Island"
        t:FillBall(Vector3.new(0, 0, 0), 4, t:AddMaterial(nil))
    )");
    rig.frames(1);
    REQUIRE(solids(rig) == "true\tfalse\n");

    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        local id = history:TryBeginRecording("Dig")
        local t = workspace.Island
        t:SubtractBall(Vector3.new(0, 0, 0), 2)
        t:FillBall(Vector3.new(10, 0, 0), 3, t:GetMaterialById(1))
        history:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
    )");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(solids(rig) == "false\ttrue\n");
    REQUIRE(rig.game.history().can_undo().second == "Dig");

    rig.game.history().undo();   // both edits, as one step
    REQUIRE(solids(rig) == "true\tfalse\n");
    rig.game.history().redo();
    REQUIRE(solids(rig) == "false\ttrue\n");
}

TEST_CASE("TU2 a cancelled recording puts the voxels back", "[terrain][lua][history]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local t = Instance.new("Terrain", workspace)
        t.Name = "Island"
        t:FillBall(Vector3.new(0, 0, 0), 4, t:AddMaterial(nil))
        local history = game:GetService("ChangeHistoryService")
        local id = history:TryBeginRecording("Dig")
        t:SubtractBall(Vector3.new(0, 0, 0), 2)
        history:FinishRecording(id, Enum.FinishRecordingOperation.Cancel)
    )");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    rig.frames(1);
    REQUIRE(solids(rig) == "true\tfalse\n");
}

TEST_CASE("TU3 a Terrain made in one step and edited in the next comes back whole after undo and redo of both", "[terrain][lua][history]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local history = game:GetService("ChangeHistoryService")
        local id = history:TryBeginRecording("Make")
        local t = Instance.new("Terrain", workspace)
        t.Name = "Island"
        t:FillBall(Vector3.new(0, 0, 0), 4, t:AddMaterial(nil))
        history:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
        id = history:TryBeginRecording("Grow")
        t:FillBall(Vector3.new(10, 0, 0), 3, t:GetMaterialById(1))
        history:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
    )");
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    rig.frames(1);
    REQUIRE(solids(rig) == "true\ttrue\n");
    rig.game.history().undo();   // Grow
    REQUIRE(solids(rig) == "true\tfalse\n");
    rig.game.history().undo();   // Make
    rig.game.history().redo();   // Make: the Terrain as it was made
    REQUIRE(solids(rig) == "true\tfalse\n");
    rig.game.history().redo();   // Grow
    REQUIRE(solids(rig) == "true\ttrue\n");
}
