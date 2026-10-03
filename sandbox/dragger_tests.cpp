// The Dragger: translate handles that move a PVInstance, and the math under them.

#include "support.hpp"

#include "Camera.hpp"
#include "ChangeHistoryService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace {

using engine_core::InstanceId;

bool has_line(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const auto& line : batch.lines) {
        if (line.text == text) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("VP1 Camera.ViewportSize is read-only to scripts, unsaved, and not undone", "[VP1]") {
    ScriptRig rig;
    engine_core::Camera& camera = rig.game.create<engine_core::Camera>();
    rig.game.set_parent(camera.id(), rig.game.scene_service("Workspace"));
    rig.game.history().end_gesture();
    rig.game.history().reset_waypoints();
    camera.set_viewport_size(engine_core::Vec2{800.f, 600.f});
    rig.game.history().end_gesture();
    REQUIRE_FALSE(rig.game.history().can_undo().first);
    rig.runtime.run_chunk(R"(
        local camera
        for _, child in workspace:GetChildren() do
            if child.ClassName == "Camera" then camera = child end
        end
        print("size", camera.ViewportSize.X, camera.ViewportSize.Y)
        print("refused", not pcall(function() camera.ViewportSize = Vector2.new(1, 1) end))
    )");
    rig.frames(1);
    const auto output = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(output, "size\t800\t600\n"));
    REQUIRE(has_line(output, "refused\ttrue\n"));
}
