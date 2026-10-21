#include "ShaderEffect.hpp"

#include "Containment.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"
#include "Shader.hpp"

#include <cmath>
#include <iterator>

namespace engine_core {
namespace {

LuaSlot number_slot(double value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("ShaderEffect setters run on SimulationThread");
    }
}

constexpr ReferenceSpec kShaderEffectRefs[] = {
    {"Shader", "Shader"},
};

}  // namespace

std::optional<std::string> ShaderEffect::set_enabled(bool value) {
    require_thread(*this);
    if (enabled_ == value) {
        return std::nullopt;
    }
    enabled_ = value;
    note_property_change("Enabled", bool_slot(!value), bool_slot(value));
    return std::nullopt;
}

std::optional<std::string> ShaderEffect::set_order(double value) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string("Order must be a finite number");
    }
    if (order_ == value) {
        return std::nullopt;
    }
    const double previous = order_;
    order_ = value;
    note_property_change("Order", number_slot(previous), number_slot(value));
    return std::nullopt;
}

const Shader* ShaderEffect::shader(const DataModel& world) const {
    const LuaSlot slot = reference(kShaderReference);
    const auto* shader = slot.kind == LuaSlot::Kind::Instance ? dynamic_cast<const Shader*>(world.instance(slot.id))
                                                              : nullptr;
    return shader != nullptr && shader->kind() == ShaderKind::Post ? shader : nullptr;
}

const ReferenceSpec* ShaderEffect::reference_specs(std::size_t& count) const {
    count = std::size(kShaderEffectRefs);
    return kShaderEffectRefs;
}

std::optional<std::string> ShaderEffect::refuse_target(std::size_t index, const DataModel& target) const {
    const auto* shader = dynamic_cast<const Shader*>(&target);
    if (index == kShaderReference && shader != nullptr && shader->kind() != ShaderKind::Post) {
        return name(target.id()) + " is a surface shader; a ShaderEffect takes a post shader";
    }
    return std::nullopt;
}

void ShaderEffect::on_reuse() {
    ReferenceAsset::on_reuse();
    enabled_ = kDefaultEnabled;
    order_ = kDefaultOrder;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

ShaderEffect* effect_of(DataModel& object) { return dynamic_cast<ShaderEffect*>(&object); }

bool read_enabled(DataModel&, DataModel& object, LuaSlot& out) {
    const ShaderEffect* effect = effect_of(object);
    if (effect == nullptr) {
        return false;
    }
    out = bool_slot(effect->enabled());
    return true;
}

bool write_enabled(DataModel&, DataModel& object, LuaSlot& in) {
    ShaderEffect* effect = effect_of(object);
    return effect != nullptr && refuse(in, effect->set_enabled(in.flag));
}

bool read_order(DataModel&, DataModel& object, LuaSlot& out) {
    const ShaderEffect* effect = effect_of(object);
    if (effect == nullptr) {
        return false;
    }
    out = number_slot(effect->order());
    return true;
}

bool write_order(DataModel&, DataModel& object, LuaSlot& in) {
    ShaderEffect* effect = effect_of(object);
    return effect != nullptr && refuse(in, effect->set_order(in.number));
}

bool read_shader(DataModel&, DataModel& object, LuaSlot& out) {
    const ShaderEffect* effect = effect_of(object);
    if (effect == nullptr) {
        return false;
    }
    out = effect->reference(ShaderEffect::kShaderReference);
    return true;
}

bool write_shader(DataModel&, DataModel& object, LuaSlot& in) {
    ShaderEffect* effect = effect_of(object);
    return effect != nullptr && refuse(in, effect->set_reference(ShaderEffect::kShaderReference, in));
}

ANARCHY_LUA_REGISTER(register_shader_effect_lua) {
    const LuaField fields[] = {
        lua_saved_property("Enabled", "boolean", read_enabled, write_enabled,
                           ShaderEffect::kDefaultEnabled ? "true" : "false"),
        lua_saved_property("Shader", "Shader?", read_shader, write_shader, "null"),
        lua_saved_property("Order", "number", read_order, write_order, "0"),
    };
    register_lua_class("ShaderEffect", "ReferenceAsset", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("ShaderEffect", {"Lighting"});
}

}  // namespace

}  // namespace engine_core
