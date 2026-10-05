// BloomEffect: bloom under Lighting, whose Enabled, Intensity, Size, and
// Threshold the render snapshot carries for the Scene View.

#include "support.hpp"

#include "BloomEffect.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "Folder.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string>

namespace {

using engine_core::BloomEffect;
using engine_core::InstanceId;

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

BloomEffect& add_bloom(engine_core::DataModel& game, InstanceId parent) {
    BloomEffect& bloom = game.create<BloomEffect>();
    game.set_parent(bloom.id(), parent);
    return bloom;
}

}  // namespace

TEST_CASE("BLM1 a BloomEffect's properties are checked, undo, save, and come back at Stop", "[bloom]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("BloomEffect"));
    BloomEffect& bloom = add_bloom(game, game.scene_service("Lighting"));
    REQUIRE(bloom.enabled());
    REQUIRE(bloom.intensity() == 0.05);
    REQUIRE(bloom.size() == 24.0);
    REQUIRE(bloom.threshold() == 0.0);

    // A default BloomEffect saves none of them.
    engine_core::PropertyBag saved;
    bloom.save_properties(saved);
    REQUIRE(saved.empty());

    begin_step(game, "Set Intensity");
    REQUIRE_FALSE(bloom.set_intensity(0.3));
    end_step(game);
    REQUIRE(bloom.intensity() == 0.3);
    game.history().undo();
    REQUIRE(bloom.intensity() == 0.05);
    game.history().redo();
    REQUIRE(bloom.intensity() == 0.3);

    begin_step(game, "Set Enabled");
    REQUIRE_FALSE(bloom.set_enabled(false));
    end_step(game);
    REQUIRE_FALSE(bloom.enabled());
    game.history().undo();
    REQUIRE(bloom.enabled());

    // Each number is clamped to its range.
    REQUIRE_FALSE(bloom.set_intensity(5.0));
    REQUIRE(bloom.intensity() == BloomEffect::kMaxIntensity);
    REQUIRE_FALSE(bloom.set_intensity(-1.0));
    REQUIRE(bloom.intensity() == 0.0);
    REQUIRE_FALSE(bloom.set_size(100.0));
    REQUIRE(bloom.size() == BloomEffect::kMaxSize);
    REQUIRE_FALSE(bloom.set_size(-3.0));
    REQUIRE(bloom.size() == 0.0);
    REQUIRE_FALSE(bloom.set_threshold(50.0));
    REQUIRE(bloom.threshold() == BloomEffect::kMaxThreshold);
    REQUIRE_FALSE(bloom.set_threshold(-1.0));
    REQUIRE(bloom.threshold() == 0.0);
    REQUIRE(*bloom.set_intensity(std::nan("")) == "Intensity must be a finite number");
    REQUIRE(*bloom.set_size(INFINITY) == "Size must be a finite number");
    REQUIRE(*bloom.set_threshold(-INFINITY) == "Threshold must be a finite number");

    REQUIRE_FALSE(bloom.set_enabled(false));
    REQUIRE_FALSE(bloom.set_intensity(0.2));
    REQUIRE_FALSE(bloom.set_size(40.0));
    REQUIRE_FALSE(bloom.set_threshold(1.5));
    engine_core::PropertyBag changed;
    bloom.save_properties(changed);
    for (const char* name : {"Enabled", "Intensity", "Size", "Threshold"}) {
        INFO(name);
        REQUIRE(engine_core::bag_find(changed, name) != nullptr);
    }

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(bloom.set_size(10.0));
    REQUIRE_FALSE(bloom.set_enabled(true));
    game.stop_simulation();
    REQUIRE(bloom.size() == 40.0);
    REQUIRE_FALSE(bloom.enabled());
    REQUIRE(bloom.threshold() == 1.5);
}

TEST_CASE("BLM2 a BloomEffect belongs under Lighting and nowhere else", "[bloom]") {
    SimRole role;
    engine_core::Game game;
    using engine_core::placement_error;
    REQUIRE_FALSE(placement_error("Lighting", "BloomEffect", "Bloom"));
    REQUIRE(reason(placement_error("Workspace", "BloomEffect", "Bloom")) == "A BloomEffect must be in Lighting");
    REQUIRE(reason(placement_error("Storage", "BloomEffect", "Bloom")) == "A BloomEffect must be in Lighting");

    const InstanceId lighting = game.scene_service("Lighting");
    BloomEffect& bloom = add_bloom(game, lighting);
    REQUIRE(game.parent(bloom.id()) == lighting);
    // In a Folder under Lighting, Lighting's rule decides.
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    REQUIRE_FALSE(game.parent_error(bloom.id(), folder.id()));
    REQUIRE(reason(game.parent_error(bloom.id(), workspace_of(game))) == "A BloomEffect must be in Lighting");
    game.set_parent(bloom.id(), folder.id());
    REQUIRE(reason(game.parent_error(folder.id(), workspace_of(game))) == "A BloomEffect must be in Lighting");

    REQUIRE(engine_core::parent_suits("Lighting", "BloomEffect"));
    REQUIRE_FALSE(engine_core::parent_suits("Workspace", "BloomEffect"));
}

TEST_CASE("BLM4 scripts make a BloomEffect and set it", "[bloom]") {
    ScriptRig rig;
    add_script(rig.game, "Bloom", R"(
        local bloom = Instance.new("BloomEffect", game.Lighting)
        _G.defaults = bloom.Enabled == true and math.abs(bloom.Intensity - 0.05) < 1e-9
            and bloom.Size == 24 and bloom.Threshold == 0
        bloom.Enabled = false
        bloom.Intensity = 2
        bloom.Size = 12
        bloom.Threshold = 1.5
        _G.set = bloom.Enabled == false and bloom.Intensity == 1 and bloom.Size == 12 and bloom.Threshold == 1.5
        _G.refused = not pcall(function() bloom.Size = 0 / 0 end)
            and not pcall(function() Instance.new("BloomEffect", workspace) end)
            and not pcall(function() bloom.Parent = workspace end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"defaults", "set", "refused"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

TEST_CASE("BLM3 the snapshot carries the first BloomEffect under Lighting", "[bloom][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    frame();
    REQUIRE_FALSE(pump.front().bloom.present);

    const InstanceId lighting = game.scene_service("Lighting");
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    BloomEffect& first = add_bloom(game, folder.id());
    BloomEffect& second = add_bloom(game, lighting);
    REQUIRE_FALSE(first.set_enabled(false));
    REQUIRE_FALSE(first.set_intensity(0.5));
    REQUIRE_FALSE(first.set_size(12.0));
    REQUIRE_FALSE(first.set_threshold(2.0));
    REQUIRE_FALSE(second.set_size(40.0));
    frame();
    // The Folder comes first in Lighting, so the BloomEffect inside it is the bloom.
    {
        const engine_core::VisualBloom& bloom = pump.front().bloom;
        REQUIRE(bloom.present);
        REQUIRE_FALSE(bloom.enabled);
        REQUIRE(bloom.intensity == 0.5f);
        REQUIRE(bloom.size == 12.f);
        REQUIRE(bloom.threshold == 2.f);
    }

    // A change shows on the next frame.
    REQUIRE_FALSE(first.set_enabled(true));
    frame();
    REQUIRE(pump.front().bloom.enabled);

    // Gone, the next one is the bloom.
    game.destroy(first.id());
    frame();
    REQUIRE(pump.front().bloom.present);
    REQUIRE(pump.front().bloom.size == 40.f);

    game.destroy(second.id());
    frame();
    REQUIRE_FALSE(pump.front().bloom.present);
    REQUIRE(pump.front().bloom.intensity == 0.05f);
}
