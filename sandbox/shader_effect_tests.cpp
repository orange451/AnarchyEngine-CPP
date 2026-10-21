// ShaderEffect: a post Shader drawn over the finished frame, under Lighting,
// whose uniforms are its attributes; the render snapshot carries every one in
// Order.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "Containment.hpp"
#include "Folder.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "Shader.hpp"
#include "ShaderEffect.hpp"
#include "SnapshotPump.hpp"
#include "SurfaceBinding.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string>

namespace {

using engine_core::InstanceId;
using engine_core::Shader;
using engine_core::ShaderEffect;
using engine_core::ShaderKind;

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

ShaderEffect& add_effect(engine_core::DataModel& game, const char* name, InstanceId parent) {
    ShaderEffect& effect = game.create<ShaderEffect>();
    game.set_name(effect.id(), name);
    game.set_parent(effect.id(), parent);
    return effect;
}

Shader& add_shader(engine_core::DataModel& game, const char* name, ShaderKind kind, const char* source) {
    Shader& shader = game.create<Shader>();
    game.set_name(shader.id(), name);
    game.set_parent(shader.id(), game.service("Shaders"));
    shader.set_kind(kind);
    shader.set_source(source);
    return shader;
}

engine_core::LuaSlot instance_ref(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

constexpr const char* kVignette =
    "uniform float Strength : range(0, 1) = 0.5;\nvoid post() {\n    COLOR.rgb *= 1.0 - Strength;\n}\n";
constexpr const char* kTint = "uniform vec3 Tint : color = vec3(1.0);\nvoid surface() {\n    ALBEDO *= Tint;\n}\n";

}  // namespace

TEST_CASE("SFX1 a ShaderEffect's properties are checked, undo, save, and come back at Stop", "[shader-effect]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::project_class_known("ShaderEffect"));
    ShaderEffect& effect = add_effect(game, "Vignette", game.scene_service("Lighting"));
    REQUIRE(effect.enabled());
    REQUIRE(effect.order() == 0.0);
    engine_core::PropertyBag saved;
    effect.save_properties(saved);
    REQUIRE(saved.empty());

    begin_step(game, "Set Order");
    REQUIRE_FALSE(effect.set_order(2.5));
    end_step(game);
    REQUIRE(effect.order() == 2.5);
    game.history().undo();
    REQUIRE(effect.order() == 0.0);
    game.history().redo();
    REQUIRE(effect.order() == 2.5);
    begin_step(game, "Set Enabled");
    REQUIRE_FALSE(effect.set_enabled(false));
    end_step(game);
    REQUIRE_FALSE(effect.enabled());
    game.history().undo();
    REQUIRE(effect.enabled());
    REQUIRE(*effect.set_order(std::nan("")) == "Order must be a finite number");
    REQUIRE(*effect.set_order(INFINITY) == "Order must be a finite number");
    REQUIRE_FALSE(effect.set_order(-3.0));
    REQUIRE(effect.order() == -3.0);

    REQUIRE_FALSE(effect.set_enabled(false));
    engine_core::PropertyBag changed;
    effect.save_properties(changed);
    REQUIRE(engine_core::bag_find(changed, "Enabled") != nullptr);
    REQUIRE(engine_core::bag_find(changed, "Order") != nullptr);

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(effect.set_order(9.0));
    REQUIRE_FALSE(effect.set_enabled(true));
    game.stop_simulation();
    REQUIRE(effect.order() == -3.0);
    REQUIRE_FALSE(effect.enabled());
}

TEST_CASE("SFX2 a ShaderEffect belongs under Lighting and nowhere else", "[shader-effect]") {
    SimRole role;
    engine_core::Game game;
    using engine_core::placement_error;
    REQUIRE_FALSE(placement_error("Lighting", "ShaderEffect", "Vignette"));
    REQUIRE(reason(placement_error("Workspace", "ShaderEffect", "Vignette")) == "A ShaderEffect must be in Lighting");
    const InstanceId lighting = game.scene_service("Lighting");
    ShaderEffect& effect = add_effect(game, "Vignette", lighting);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    REQUIRE_FALSE(game.parent_error(effect.id(), folder.id()));
    REQUIRE(reason(game.parent_error(effect.id(), workspace_of(game))) == "A ShaderEffect must be in Lighting");
    REQUIRE(engine_core::parent_suits("Lighting", "ShaderEffect"));
    REQUIRE_FALSE(engine_core::parent_suits("Workspace", "ShaderEffect"));
}

TEST_CASE("SFX3 a ShaderEffect takes a post Shader and refuses a surface one", "[shader-effect]") {
    SimRole role;
    engine_core::Game game;
    Shader& look = add_shader(game, "Look", ShaderKind::Surface, kTint);
    Shader& fade = add_shader(game, "Fade", ShaderKind::Post, kVignette);
    ShaderEffect& effect = add_effect(game, "Vignette", game.scene_service("Lighting"));
    REQUIRE(effect.shader(game) == nullptr);
    REQUIRE(reason(effect.set_reference(ShaderEffect::kShaderReference, instance_ref(look.id()))) ==
            "Look is a surface shader; a ShaderEffect takes a post shader");
    REQUIRE(effect.shader(game) == nullptr);
    REQUIRE_FALSE(effect.set_reference(ShaderEffect::kShaderReference, instance_ref(fade.id())));
    REQUIRE(effect.shader(game) == &fade);
    // A Shader turned Surface afterwards is kept by reference and draws nothing.
    REQUIRE_FALSE(fade.set_kind(ShaderKind::Surface));
    REQUIRE(effect.shader(game) == nullptr);
    REQUIRE(effect.reference(ShaderEffect::kShaderReference).id == fade.id());
    REQUIRE_FALSE(fade.set_kind(ShaderKind::Post));
    REQUIRE(effect.shader(game) == &fade);
}

TEST_CASE("SFX4 scripts make a ShaderEffect, give it a Shader, and set its attributes", "[shader-effect]") {
    ScriptRig rig;
    add_shader(rig.game, "Fade", ShaderKind::Post, kVignette);
    add_script(rig.game, "Effects", R"(
        local effect = Instance.new("ShaderEffect")
        effect.Name = "Vignette"
        effect.Shader = game.Assets.Shaders.Fade
        effect.Order = 2
        effect:SetAttribute("Strength", 0.8)
        effect.Parent = game.Lighting
        _G.set = effect.Enabled == true and effect.Order == 2 and effect.Shader == game.Assets.Shaders.Fade
            and effect:GetAttribute("Strength") == 0.8 and game.Lighting.Vignette == effect
        _G.refused = not pcall(function() effect.Order = 0 / 0 end)
            and not pcall(function() Instance.new("ShaderEffect", workspace) end)
            and not pcall(function() effect.Parent = workspace end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"set", "refused"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

TEST_CASE("SFX5 the snapshot carries every ShaderEffect with a post Shader, in Order", "[shader-effect][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    frame();
    REQUIRE(pump.front().effects.empty());

    Shader& fade = add_shader(game, "Fade", ShaderKind::Post, kVignette);
    Shader& look = add_shader(game, "Look", ShaderKind::Surface, kTint);
    const InstanceId lighting = game.scene_service("Lighting");
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), lighting);
    ShaderEffect& late = add_effect(game, "Late", folder.id());
    ShaderEffect& first = add_effect(game, "First", lighting);
    ShaderEffect& second = add_effect(game, "Second", lighting);
    ShaderEffect& plain = add_effect(game, "Plain", lighting);
    ShaderEffect& bare = add_effect(game, "Bare", lighting);
    for (ShaderEffect* effect : {&late, &first, &second}) {
        REQUIRE_FALSE(effect->set_reference(ShaderEffect::kShaderReference, instance_ref(fade.id())));
    }
    // Plain's Shader turned Surface after it was set: kept, and left out.
    Shader& other = add_shader(game, "Other", ShaderKind::Post, kVignette);
    REQUIRE_FALSE(plain.set_reference(ShaderEffect::kShaderReference, instance_ref(other.id())));
    REQUIRE_FALSE(other.set_kind(ShaderKind::Surface));
    (void)look;
    REQUIRE_FALSE(late.set_order(2.0));
    REQUIRE_FALSE(second.set_enabled(false));
    frame();
    const std::vector<engine_core::VisualShaderEffect>& effects = pump.front().effects;
    REQUIRE(effects.size() == 3);
    REQUIRE(effects[0].effect == first.id());
    REQUIRE(effects[0].enabled);
    REQUIRE(effects[1].effect == second.id());
    REQUIRE_FALSE(effects[1].enabled);
    REQUIRE(effects[2].effect == late.id());
    REQUIRE(effects[2].order == 2.0);
    for (const engine_core::VisualShaderEffect& row : effects) {
        REQUIRE(row.binding != nullptr);
        REQUIRE(row.binding->shader == fade.id());
    }
    (void)bare;
    // Unchanged, the bindings are last frame's.
    const std::shared_ptr<const engine_core::SurfaceBinding> kept = effects[0].binding;
    frame();
    REQUIRE(pump.front().effects[0].binding == kept);
    // An attribute change swaps the values, not the binding; a text change makes a new one.
    REQUIRE_FALSE(first.set_attribute("Strength", [] {
        engine_core::LuaSlot slot;
        slot.kind = engine_core::LuaSlot::Kind::Number;
        slot.number = 0.9;
        return slot;
    }()));
    frame();
    REQUIRE(pump.front().effects[0].binding == kept);
    REQUIRE(kept->values()->slots[0].number == 0.9);
    fade.set_source("void post() {}\n");
    frame();
    REQUIRE(pump.front().effects[0].binding != kept);
}
