// WireframeAdornment: its lines in the snapshot, in world space, and its Lua methods.

#include "support.hpp"

#include "GameObject.hpp"
#include "Matrix4.hpp"
#include "SnapshotPump.hpp"
#include "WireframeAdornment.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using engine_core::WireframeAdornment;

const engine_core::VisualSnapshot& publish(engine_core::SnapshotPump& pump, engine_core::DataModel& game) {
    pump.prepare_copy(game);
    pump.publish();
    return pump.front();
}

engine_core::LuaSlot instance_slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

}  // namespace

TEST_CASE("WF1 a drawn WireframeAdornment publishes its lines in world space", "[wireframe]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);

    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    game.set_parent(part.id(), game.scene_service("Workspace"));
    part.set_transform(engine_core::matrix4_translation(10, 0, 0));

    WireframeAdornment& wire = game.create<WireframeAdornment>();
    game.set_parent(wire.id(), game.scene_service("Storage"));
    WireframeAdornment::Line red;
    red.to = {1, 0, 0};
    red.own_color = true;
    red.color = {1, 0, 0, 1};
    REQUIRE_FALSE(wire.add_line(red));
    WireframeAdornment::Line plain;
    plain.to = {0, 1, 0};
    REQUIRE_FALSE(wire.add_line(plain));
    REQUIRE_FALSE(wire.set_transparency(0.5));
    // Only in Workspace or Core.
    REQUIRE(publish(pump, game).wire_lines.empty());

    game.set_parent(wire.id(), game.scene_service("Workspace"));
    {
        const auto& lines = publish(pump, game).wire_lines;
        REQUIRE(lines.size() == 28);
        CHECK(lines[0] == 0.f);
        CHECK(lines[3] == 1.f);
        CHECK(lines[4] == 0.f);
        CHECK(lines[6] == 0.5f);
        // The second line follows Color3, white.
        CHECK(lines[14 + 4] == 1.f);
    }
    REQUIRE_FALSE(wire.set_adornee(instance_slot(part.id())));
    CHECK(publish(pump, game).wire_lines[0] == 10.f);

    REQUIRE_FALSE(wire.set_visible(false));
    CHECK(publish(pump, game).wire_lines.empty());
    REQUIRE_FALSE(wire.set_visible(true));
    wire.clear_lines();
    CHECK(publish(pump, game).wire_lines.empty());
    CHECK(wire.set_transparency(2.0).has_value());
}

TEST_CASE("WF2 Lua adds, counts, and clears lines", "[wireframe]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local wire = Instance.new("WireframeAdornment", workspace)
        wire:AddLine(Vector3.zero, Vector3.xAxis)
        wire:AddLine(Vector3.zero, Vector3.yAxis, Color3.new(1, 0, 0))
        wire:AddLines({Vector3.zero, Vector3.zAxis, Vector3.xAxis, Vector3.yAxis})
        wire:AddPath({Vector3.zero, Vector3.xAxis, Vector3.yAxis}, true)
        print("count", wire:GetLineCount())
        print(pcall(function() wire:AddLines({Vector3.zero}) end))
        print(pcall(function() wire:AddLine(Vector3.zero, Vector3.xAxis, "red") end))
        print("after", wire:GetLineCount())
        wire.Color3 = Color3.new(0, 1, 0)
        wire.Transparency = 0.25
        wire:Clear()
        print("cleared", wire:GetLineCount(), wire.Transparency)
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    const auto has = [&](const std::string& text) {
        for (const auto& line : out.lines) {
            if (line.text.find(text) != std::string::npos) return true;
        }
        return false;
    };
    for (const auto& line : out.lines) UNSCOPED_INFO(line.text);
    CHECK(has("count\t7"));
    CHECK(has("in pairs"));
    CHECK(has("Color3"));
    CHECK(has("after\t7"));
    CHECK(has("cleared\t0\t0.25"));
}
