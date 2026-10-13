#include "ParticleEmitter.hpp"

#include "AssetInstances.hpp"
#include "Brush.hpp"
#include "CollisionGroups.hpp"
#include "Containment.hpp"
#include "Contract.hpp"
#include "CurveJson.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "ParticleBursts.hpp"
#include "PhysicsObject.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace engine_core {
namespace {

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("ParticleEmitter setters run on SimulationThread");
    }
}

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

LuaSlot enum_slot(const EnumType& type, int value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &type;
    slot.number = value;
    return slot;
}

LuaSlot vec3_slot(Vec3 value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec3;
    slot.vec = value;
    return slot;
}

LuaSlot vec2_slot(Vec2 value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Vec2;
    slot.vec = Vec3{value.x, value.y, 0.f};
    return slot;
}

LuaSlot string_slot(const std::string& value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::String;
    slot.text = value;
    return slot;
}

bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Every value the curve can give at a key, for a range check.
template <class Check>
bool curve_values_ok(const NumberCurve& curve, Check ok) {
    switch (curve.mode()) {
    case NumberCurveMode::Constant:
        return ok(curve.min());
    case NumberCurveMode::Range:
        return ok(curve.min()) && ok(curve.max());
    default:
        break;
    }
    for (const NumberKey& key : curve.keys_a()) {
        if (!ok(key.value)) {
            return false;
        }
    }
    for (const NumberKey& key : curve.keys_b()) {
        if (!ok(key.value)) {
            return false;
        }
    }
    return true;
}

bool curve_finite(const NumberCurve& curve) {
    return curve_values_ok(curve, [](float v) { return std::isfinite(v); }) && std::isfinite(curve.max());
}

}  // namespace

std::optional<std::string> ParticleEmitter::set_number_property(const char* name, double& slot, double value,
                                                                double low, double high, bool whole,
                                                                const char* range_text) {
    require_thread(*this);
    if (!std::isfinite(value)) {
        return std::string(name) + " must be a finite number";
    }
    if ((whole && value != std::floor(value)) || value < low || value > high) {
        return std::string(range_text);
    }
    if (slot == value) {
        return std::nullopt;
    }
    const double previous = slot;
    slot = value;
    dirty_ |= kDirtySettings;
    note_property_change(name, number_slot(previous), number_slot(value));
    return std::nullopt;
}

std::optional<std::string> ParticleEmitter::set_bool_property(const char* name, bool& slot, bool value) {
    require_thread(*this);
    if (slot == value) {
        return std::nullopt;
    }
    slot = value;
    dirty_ |= kDirtySettings;
    note_property_change(name, bool_slot(!value), bool_slot(value));
    return std::nullopt;
}

std::optional<std::string> ParticleEmitter::set_enum_property(const char* name, const EnumType& type, int& slot,
                                                              int value) {
    require_thread(*this);
    if (enum_item_name(type, value) == nullptr) {
        return std::string(name) + " must be an Enum." + type.name;
    }
    if (slot == value) {
        return std::nullopt;
    }
    const int previous = slot;
    slot = value;
    dirty_ |= kDirtySettings;
    note_property_change(name, enum_slot(type, previous), enum_slot(type, value));
    return std::nullopt;
}

// A direction: normalised, and never zero.
std::optional<std::string> ParticleEmitter::set_axis_property(const char* name, Vec3& slot, Vec3 value) {
    require_thread(*this);
    if (!finite(value)) {
        return std::string(name) + " must be finite";
    }
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (!(length > 0.f)) {
        return std::string(name) + " must not be zero";
    }
    return set_vector_property(name, slot, Vec3{value.x / length, value.y / length, value.z / length});
}

std::optional<std::string> ParticleEmitter::set_vector_property(const char* name, Vec3& slot, Vec3 value) {
    require_thread(*this);
    if (!finite(value)) {
        return std::string(name) + " must be finite";
    }
    if (slot.x == value.x && slot.y == value.y && slot.z == value.z) {
        return std::nullopt;
    }
    const Vec3 previous = slot;
    slot = value;
    dirty_ |= kDirtySettings;
    note_property_change(name, vec3_slot(previous), vec3_slot(value));
    return std::nullopt;
}

std::optional<std::string> ParticleEmitter::set_curve(const char* name, NumberCurve& slot, NumberCurve value,
                                                      CurveRule rule) {
    require_thread(*this);
    if (!curve_finite(value)) {
        return std::string(name) + " must be finite";
    }
    if (rule == CurveRule::NotNegative && !curve_values_ok(value, [](float v) { return v >= 0.f; })) {
        return std::string(name) + " must not be negative";
    }
    if (rule == CurveRule::AboveZero && !curve_values_ok(value, [](float v) { return v > 0.f; })) {
        return std::string(name) + " must be above 0";
    }
    if (slot == value) {
        return std::nullopt;
    }
    LuaSlot before = number_curve_slot(slot);
    slot = value;
    dirty_ |= kDirtySettings;
    note_property_change(name, before, number_curve_slot(std::move(value)));
    return std::nullopt;
}

std::optional<std::string> ParticleEmitter::set_playing(bool value) {
    const bool restart = value && !playing_;
    std::optional<std::string> error = set_bool_property("Playing", playing_, value);
    if (restart) {
        dirty_ |= kDirtyRestart;
    }
    return error;
}

std::optional<std::string> ParticleEmitter::set_rate(NumberCurve value) {
    return set_curve("Rate", rate_, std::move(value), CurveRule::NotNegative);
}

std::optional<std::string> ParticleEmitter::set_bursts(std::vector<ParticleBurst> value) {
    require_thread(*this);
    if (std::optional<std::string> error = check_bursts(value)) {
        return error;
    }
    std::stable_sort(value.begin(), value.end(),
                     [](const ParticleBurst& a, const ParticleBurst& b) { return a.time < b.time; });
    if (bursts_ == value) {
        return std::nullopt;
    }
    LuaSlot before = bursts_slot(bursts_);
    bursts_ = std::move(value);
    dirty_ |= kDirtySettings;
    note_property_change("Bursts", before, bursts_slot(bursts_));
    return std::nullopt;
}

std::optional<std::string> ParticleEmitter::set_duration(double value) {
    return set_number_property("Duration", duration_, value, std::numeric_limits<double>::min(),
                               std::numeric_limits<double>::max(), false, "Duration must be above 0");
}

std::optional<std::string> ParticleEmitter::set_looping(bool value) { return set_bool_property("Looping", looping_, value); }

std::optional<std::string> ParticleEmitter::set_prewarm(bool value) { return set_bool_property("Prewarm", prewarm_, value); }

std::optional<std::string> ParticleEmitter::set_max_particles(double value) {
    return set_number_property("MaxParticles", max_particles_, value, 0.0, std::numeric_limits<int>::max(), true,
                               "MaxParticles must be a whole number, 0 or more");
}

std::optional<std::string> ParticleEmitter::set_seed(double value) {
    const double before = seed_;
    std::optional<std::string> error =
        set_number_property("Seed", seed_, value, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(),
                            true, "Seed must be a whole number");
    if (!error && seed_ != before) {
        dirty_ |= kDirtyRestart;
    }
    return error;
}

std::optional<std::string> ParticleEmitter::set_shape(int value) {
    return set_enum_property("Shape", particle_shape_enum(), shape_, value);
}

std::optional<std::string> ParticleEmitter::set_emit_from(int value) {
    return set_enum_property("EmitFrom", particle_emit_from_enum(), emit_from_, value);
}

std::optional<std::string> ParticleEmitter::set_arc(double value) {
    return set_number_property("Arc", arc_, value, 0.0, 360.0, false, "Arc must be from 0 to 360");
}

std::optional<std::string> ParticleEmitter::set_emit_direction(Vec3 value) {
    return set_axis_property("EmitDirection", emit_direction_, value);
}

std::optional<std::string> ParticleEmitter::set_emit_direction_space(int value) {
    return set_enum_property("EmitDirectionSpace", transform_space_enum(), emit_direction_space_, value);
}

std::optional<std::string> ParticleEmitter::set_emit_spread(Vec2 value) {
    require_thread(*this);
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || value.x < 0.f || value.y < 0.f || value.x > 180.f ||
        value.y > 180.f) {
        return std::string("EmitSpread must be from 0 to 180 on each axis");
    }
    if (emit_spread_.x == value.x && emit_spread_.y == value.y) {
        return std::nullopt;
    }
    const Vec2 previous = emit_spread_;
    emit_spread_ = value;
    dirty_ |= kDirtySettings;
    note_property_change("EmitSpread", vec2_slot(previous), vec2_slot(value));
    return std::nullopt;
}

std::optional<std::string> ParticleEmitter::set_lifetime(NumberCurve value) {
    return set_curve("Lifetime", lifetime_, std::move(value), CurveRule::AboveZero);
}

std::optional<std::string> ParticleEmitter::set_emit_speed(NumberCurve value) {
    return set_curve("EmitSpeed", emit_speed_, std::move(value), CurveRule::Any);
}

std::optional<std::string> ParticleEmitter::set_emit_rotation(NumberCurve value) {
    return set_curve("EmitRotation", emit_rotation_, std::move(value), CurveRule::Any);
}

std::optional<std::string> ParticleEmitter::set_rotation_speed(NumberCurve value) {
    return set_curve("RotationSpeed", rotation_speed_, std::move(value), CurveRule::Any);
}

std::optional<std::string> ParticleEmitter::set_simulation_space(int value) {
    return set_enum_property("SimulationSpace", transform_space_enum(), simulation_space_, value);
}

std::optional<std::string> ParticleEmitter::set_inherit_velocity(double value) {
    return set_number_property("InheritVelocity", inherit_velocity_, value, 0.0, 1.0, false,
                               "InheritVelocity must be from 0 to 1");
}

std::optional<std::string> ParticleEmitter::set_acceleration(Vec3 value) {
    return set_vector_property("Acceleration", acceleration_, value);
}

std::optional<std::string> ParticleEmitter::set_acceleration_space(int value) {
    return set_enum_property("AccelerationSpace", transform_space_enum(), acceleration_space_, value);
}

std::optional<std::string> ParticleEmitter::set_drag(NumberCurve value) {
    return set_curve("Drag", drag_, std::move(value), CurveRule::NotNegative);
}

std::optional<std::string> ParticleEmitter::set_orbit_speed(NumberCurve value) {
    return set_curve("OrbitSpeed", orbit_speed_, std::move(value), CurveRule::Any);
}

std::optional<std::string> ParticleEmitter::set_orbit_axis(Vec3 value) {
    return set_axis_property("OrbitAxis", orbit_axis_, value);
}

std::optional<std::string> ParticleEmitter::set_collision(int value) {
    return set_enum_property("Collision", particle_collision_enum(), collision_, value);
}

std::optional<std::string> ParticleEmitter::set_collision_response(int value) {
    return set_enum_property("CollisionResponse", particle_collision_response_enum(), collision_response_, value);
}

std::optional<std::string> ParticleEmitter::set_bounciness(double value) {
    return set_number_property("Bounciness", bounciness_, value, 0.0, 1.0, false, "Bounciness must be from 0 to 1");
}

std::optional<std::string> ParticleEmitter::set_friction(double value) {
    return set_number_property("Friction", friction_, value, 0.0, 1.0, false, "Friction must be from 0 to 1");
}

std::optional<std::string> ParticleEmitter::set_collision_group(std::string value) {
    require_thread(*this);
    if (value.empty()) {
        return std::string("CollisionGroup must name a group");
    }
    if (collision_group_ == value) {
        return std::nullopt;
    }
    LuaSlot before = string_slot(collision_group_);
    collision_group_ = std::move(value);
    dirty_ |= kDirtySettings;
    note_property_change("CollisionGroup", before, string_slot(collision_group_));
    return std::nullopt;
}

LuaSlot ParticleEmitter::texture() const { return instance_reference_slot(texture_ref_, "Texture"); }

std::optional<std::string> ParticleEmitter::set_texture(const LuaSlot& value) {
    require_thread(*this);
    std::optional<std::string> error = set_instance_reference("Texture", "Texture", texture_ref_, value);
    if (!error) {
        dirty_ |= kDirtySettings;
    }
    return error;
}

std::optional<std::string> ParticleEmitter::set_texture_mode(int value) {
    return set_enum_property("TextureMode", particle_texture_mode_enum(), texture_mode_, value);
}

std::optional<std::string> ParticleEmitter::set_flipbook_grid(Vec2 value) {
    require_thread(*this);
    const auto whole = [](float v) { return std::isfinite(v) && v >= 1.f && v == std::floor(v) && v <= 4096.f; };
    if (!whole(value.x) || !whole(value.y)) {
        return std::string("FlipbookGrid must be whole numbers, 1 or more");
    }
    if (flipbook_grid_.x == value.x && flipbook_grid_.y == value.y) {
        return std::nullopt;
    }
    const Vec2 previous = flipbook_grid_;
    flipbook_grid_ = value;
    dirty_ |= kDirtySettings;
    note_property_change("FlipbookGrid", vec2_slot(previous), vec2_slot(value));
    return std::nullopt;
}

std::optional<std::string> ParticleEmitter::set_color(ColorCurve value) {
    require_thread(*this);
    if (color_ == value) {
        return std::nullopt;
    }
    LuaSlot before = color_curve_slot(color_);
    color_ = value;
    dirty_ |= kDirtySettings;
    note_property_change("Color", before, color_curve_slot(std::move(value)));
    return std::nullopt;
}

std::optional<std::string> ParticleEmitter::set_size(NumberCurve value) {
    return set_curve("Size", size_, std::move(value), CurveRule::NotNegative);
}

std::optional<std::string> ParticleEmitter::set_brightness(double value) {
    return set_number_property("Brightness", brightness_, value, 0.0, std::numeric_limits<double>::max(), false,
                               "Brightness must not be negative");
}

std::optional<std::string> ParticleEmitter::set_stretch(NumberCurve value) {
    return set_curve("Stretch", stretch_, std::move(value), CurveRule::Any);
}

std::optional<std::string> ParticleEmitter::set_facing(int value) {
    return set_enum_property("Facing", particle_facing_enum(), facing_, value);
}

std::optional<std::string> ParticleEmitter::set_facing_axis(Vec3 value) {
    return set_axis_property("FacingAxis", facing_axis_, value);
}

std::optional<std::string> ParticleEmitter::set_facing_axis_space(int value) {
    return set_enum_property("FacingAxisSpace", transform_space_enum(), facing_axis_space_, value);
}

std::optional<std::string> ParticleEmitter::set_velocity_stretch(double value) {
    return set_number_property("VelocityStretch", velocity_stretch_, value, 0.0, std::numeric_limits<double>::max(),
                               false, "VelocityStretch must not be negative");
}

std::optional<std::string> ParticleEmitter::set_blend_mode(int value) {
    return set_enum_property("BlendMode", particle_blend_mode_enum(), blend_mode_, value);
}

std::optional<std::string> ParticleEmitter::set_soft_edge(double value) {
    return set_number_property("SoftEdge", soft_edge_, value, 0.0, std::numeric_limits<double>::max(), false,
                               "SoftEdge must not be negative");
}

std::optional<std::string> ParticleEmitter::set_light_influence(double value) {
    return set_number_property("LightInfluence", light_influence_, value, 0.0, 1.0, false,
                               "LightInfluence must be from 0 to 1");
}

std::optional<std::string> ParticleEmitter::set_draw_order(double value) {
    return set_number_property("DrawOrder", draw_order_, value, std::numeric_limits<int>::min(),
                               std::numeric_limits<int>::max(), true, "DrawOrder must be a whole number");
}

void ParticleEmitter::request_emit(int count) { emit_requests_ += std::max(0, count); }

void ParticleEmitter::request_clear() { clear_requested_ = true; }

void ParticleEmitter::store_finished_cycle() {
    if (!playing_) {
        return;
    }
    playing_ = false;
    emit_property("Playing");
}

void ParticleEmitter::store_particle_count(int count) {
    if (particle_count_ == count) {
        return;
    }
    particle_count_ = count;
    emit_property("ParticleCount");
}

void ParticleEmitter::read_place(const std::byte* data, std::size_t size) {
    DataModel::read_place(data, size);
    emit_requests_ = 0;
    clear_requested_ = true;
    dirty_ = kDirtyAll;
}

void ParticleEmitter::on_reuse() {
    DataModel::on_reuse();
    reset();
}

void ParticleEmitter::reset() {
    playing_ = true;
    rate_ = NumberCurve::constant(20.f);
    bursts_.clear();
    duration_ = 5.0;
    looping_ = true;
    prewarm_ = false;
    max_particles_ = 1000.0;
    seed_ = 0.0;
    shape_ = static_cast<int>(ParticleShape::Box);
    emit_from_ = static_cast<int>(ParticleEmitFrom::Volume);
    arc_ = 360.0;
    emit_direction_ = Vec3{0.f, 1.f, 0.f};
    emit_direction_space_ = static_cast<int>(TransformSpace::Local);
    emit_spread_ = Vec2{0.f, 0.f};
    lifetime_ = NumberCurve::range(1.f, 2.f);
    emit_speed_ = NumberCurve::constant(5.f);
    emit_rotation_ = NumberCurve::constant(0.f);
    rotation_speed_ = NumberCurve::constant(0.f);
    simulation_space_ = static_cast<int>(TransformSpace::World);
    inherit_velocity_ = 0.0;
    acceleration_ = Vec3{0.f, 0.f, 0.f};
    acceleration_space_ = static_cast<int>(TransformSpace::World);
    drag_ = NumberCurve::constant(0.f);
    orbit_speed_ = NumberCurve::constant(0.f);
    orbit_axis_ = Vec3{0.f, 1.f, 0.f};
    collision_ = static_cast<int>(ParticleCollision::None);
    collision_response_ = static_cast<int>(ParticleCollisionResponse::Bounce);
    bounciness_ = 0.5;
    friction_ = 0.2;
    collision_group_ = "Default";
    texture_ref_.set_guid(std::string());
    texture_mode_ = static_cast<int>(ParticleTextureMode::Static);
    flipbook_grid_ = Vec2{1.f, 1.f};
    color_ = ColorCurve::constant(ColorRgb{1.f, 1.f, 1.f, 1.f}, 0.f);
    size_ = NumberCurve::constant(1.f);
    brightness_ = 1.0;
    stretch_ = NumberCurve::constant(0.f);
    facing_ = static_cast<int>(ParticleFacing::Camera);
    facing_axis_ = Vec3{0.f, 1.f, 0.f};
    facing_axis_space_ = static_cast<int>(TransformSpace::Local);
    velocity_stretch_ = 0.1;
    blend_mode_ = static_cast<int>(ParticleBlendMode::Normal);
    soft_edge_ = 0.5;
    light_influence_ = 1.0;
    draw_order_ = 0.0;
    dirty_ = kDirtyAll;
    emit_requests_ = 0;
    clear_requested_ = false;
    particle_count_ = 0;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

ParticleEmitter* emitter_of(DataModel& object) { return dynamic_cast<ParticleEmitter*>(&object); }

// Read and write thunks, one template per value type, keyed on the getter and setter.
template <class T, T (ParticleEmitter::*Get)() const>
bool read_number(DataModel&, DataModel& object, LuaSlot& out) {
    const ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    out = number_slot(static_cast<double>((e->*Get)()));
    return true;
}

template <std::optional<std::string> (ParticleEmitter::*Set)(double)>
bool write_number(DataModel&, DataModel& object, LuaSlot& in) {
    ParticleEmitter* e = emitter_of(object);
    return e != nullptr && refuse(in, (e->*Set)(in.number));
}

template <bool (ParticleEmitter::*Get)() const>
bool read_bool(DataModel&, DataModel& object, LuaSlot& out) {
    const ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    out = bool_slot((e->*Get)());
    return true;
}

template <std::optional<std::string> (ParticleEmitter::*Set)(bool)>
bool write_bool(DataModel&, DataModel& object, LuaSlot& in) {
    ParticleEmitter* e = emitter_of(object);
    return e != nullptr && refuse(in, (e->*Set)(in.flag));
}

template <class E, E (ParticleEmitter::*Get)() const, const EnumType& (*Type)()>
bool read_enum(DataModel&, DataModel& object, LuaSlot& out) {
    const ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    out = enum_slot(Type(), static_cast<int>((e->*Get)()));
    return true;
}

template <std::optional<std::string> (ParticleEmitter::*Set)(int)>
bool write_enum(DataModel&, DataModel& object, LuaSlot& in) {
    ParticleEmitter* e = emitter_of(object);
    return e != nullptr && refuse(in, (e->*Set)(static_cast<int>(in.number)));
}

template <Vec3 (ParticleEmitter::*Get)() const>
bool read_vec3(DataModel&, DataModel& object, LuaSlot& out) {
    const ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    out = vec3_slot((e->*Get)());
    return true;
}

template <std::optional<std::string> (ParticleEmitter::*Set)(Vec3)>
bool write_vec3(DataModel&, DataModel& object, LuaSlot& in) {
    ParticleEmitter* e = emitter_of(object);
    return e != nullptr && refuse(in, (e->*Set)(in.vec));
}

template <Vec2 (ParticleEmitter::*Get)() const>
bool read_vec2(DataModel&, DataModel& object, LuaSlot& out) {
    const ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    out = vec2_slot((e->*Get)());
    return true;
}

template <std::optional<std::string> (ParticleEmitter::*Set)(Vec2)>
bool write_vec2(DataModel&, DataModel& object, LuaSlot& in) {
    ParticleEmitter* e = emitter_of(object);
    return e != nullptr && refuse(in, (e->*Set)(Vec2{in.vec.x, in.vec.y}));
}

template <const NumberCurve& (ParticleEmitter::*Get)() const>
bool read_curve(DataModel&, DataModel& object, LuaSlot& out) {
    const ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    out = number_curve_slot((e->*Get)());
    return true;
}

template <std::optional<std::string> (ParticleEmitter::*Set)(NumberCurve)>
bool write_curve(DataModel&, DataModel& object, LuaSlot& in) {
    ParticleEmitter* e = emitter_of(object);
    if (e == nullptr || in.kind != LuaSlot::Kind::NumberCurve || !in.number_curve) {
        in.error = "expects a NumberCurve";
        return false;
    }
    return refuse(in, (e->*Set)(*in.number_curve));
}

bool read_color(DataModel&, DataModel& object, LuaSlot& out) {
    const ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    out = color_curve_slot(e->color());
    return true;
}

bool write_color(DataModel&, DataModel& object, LuaSlot& in) {
    ParticleEmitter* e = emitter_of(object);
    if (e == nullptr || in.kind != LuaSlot::Kind::ColorCurve || !in.color_curve) {
        in.error = "Color expects a ColorCurve";
        return false;
    }
    return refuse(in, e->set_color(*in.color_curve));
}

bool read_bursts_property(DataModel&, DataModel& object, LuaSlot& out) {
    const ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    out = bursts_slot(e->bursts());
    return true;
}

bool write_bursts_property(DataModel&, DataModel& object, LuaSlot& in) {
    ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    std::string error;
    auto bursts = bursts_from_slot(in, error);
    if (!bursts) {
        in.error = "Bursts " + error;
        return false;
    }
    return refuse(in, e->set_bursts(std::move(*bursts)));
}

bool read_collision_group(DataModel&, DataModel& object, LuaSlot& out) {
    const ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    out = string_slot(e->collision_group());
    return true;
}

bool write_collision_group(DataModel&, DataModel& object, LuaSlot& in) {
    ParticleEmitter* e = emitter_of(object);
    return e != nullptr && refuse(in, e->set_collision_group(in.text));
}

// The place's collision groups, Default first, for the Properties dropdown.
void collision_group_choices(DataModel& world, std::vector<std::string>& out) {
    for (const CollisionGroups::Info& info : world.collision_groups().groups()) {
        out.push_back(info.name);
    }
}

bool read_texture(DataModel&, DataModel& object, LuaSlot& out) {
    const ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    out = e->texture();
    return true;
}

bool write_texture(DataModel&, DataModel& object, LuaSlot& in) {
    ParticleEmitter* e = emitter_of(object);
    return e != nullptr && refuse(in, e->set_texture(in));
}

bool read_particle_count(DataModel&, DataModel& object, LuaSlot& out) {
    const ParticleEmitter* e = emitter_of(object);
    if (e == nullptr) {
        return false;
    }
    out = number_slot(e->particle_count());
    return true;
}

// Whether the emitter's parent gives its Shape a size: a PhysicsObject, a
// Brush, or a GameObject drawing a Prefab. ParticleWorld's emitter_extent
// decides the size itself.
bool parent_has_size(DataModel& world, DataModel& object) {
    const InstanceId parent = world.parent(object.id());
    const DataModel* p = parent != DataModel::kNoParent ? world.instance(parent) : nullptr;
    if (dynamic_cast<const PhysicsObject*>(p) != nullptr || dynamic_cast<const Brush*>(p) != nullptr) {
        return true;
    }
    if (const auto* go = dynamic_cast<const GameObject*>(p)) {
        const LuaSlot slot = go->prefab();
        return slot.kind == LuaSlot::Kind::Instance && dynamic_cast<const Prefab*>(world.instance(slot.id)) != nullptr;
    }
    return false;
}

bool emits_from_area(DataModel& world, DataModel& object) {
    const ParticleEmitter* e = emitter_of(object);
    return e != nullptr && e->shape() != ParticleShape::Point && parent_has_size(world, object);
}

bool arc_applies(DataModel& world, DataModel& object) {
    const ParticleEmitter* e = emitter_of(object);
    return e != nullptr && e->shape() != ParticleShape::Box && emits_from_area(world, object);
}

bool simulates_in_world(DataModel&, DataModel& object) {
    const ParticleEmitter* e = emitter_of(object);
    return e != nullptr && e->simulation_space() == TransformSpace::World;
}

bool collides(DataModel&, DataModel& object) {
    const ParticleEmitter* e = emitter_of(object);
    return e != nullptr && e->collision() != ParticleCollision::None;
}

bool collides_with_world(DataModel&, DataModel& object) {
    const ParticleEmitter* e = emitter_of(object);
    return e != nullptr && e->collision() == ParticleCollision::World;
}

using E = ParticleEmitter;

// Emit and Clear are bound in ScriptBindings, which resolves self.
ANARCHY_LUA_REGISTER(register_particle_emitter_lua) {
    const int bounce = static_cast<int>(ParticleCollisionResponse::Bounce);
    const int flipbook = static_cast<int>(ParticleTextureMode::Flipbook);
    const int world_facing = static_cast<int>(ParticleFacing::World);
    const int velocity_facing = static_cast<int>(ParticleFacing::Velocity);
    const LuaField fields[] = {
        lua_lead_group("Emission"),
        lua_saved_property("Playing", "boolean", read_bool<&E::playing>, write_bool<&E::set_playing>, "true"),
        lua_saved_property("Rate", "NumberCurve", read_curve<&E::rate>, write_curve<&E::set_rate>,
                           R"({"mode":"Constant","value":20})"),
        lua_saved_property("Bursts", "ParticleBursts", read_bursts_property, write_bursts_property, "[]"),
        lua_saved_property("Duration", "number", read_number<double, &E::duration>, write_number<&E::set_duration>, "5"),
        lua_saved_property("Looping", "boolean", read_bool<&E::looping>, write_bool<&E::set_looping>, "true"),
        lua_saved_property("Prewarm", "boolean", read_bool<&E::prewarm>, write_bool<&E::set_prewarm>, "false"),
        lua_saved_property("MaxParticles", "number", read_number<int, &E::max_particles>,
                           write_number<&E::set_max_particles>, "1000"),
        lua_saved_property("Seed", "number", read_number<int, &E::seed>, write_number<&E::set_seed>, "0"),
        lua_group("Shape"),
        lua_saved_enum("Shape", particle_shape_enum(), read_enum<ParticleShape, &E::shape, particle_shape_enum>,
                       write_enum<&E::set_shape>, "\"Box\""),
        lua_enabled_when(lua_saved_enum("EmitFrom", particle_emit_from_enum(),
                                        read_enum<ParticleEmitFrom, &E::emit_from, particle_emit_from_enum>,
                                        write_enum<&E::set_emit_from>, "\"Volume\""),
                         emits_from_area),
        lua_enabled_when(lua_slider(lua_saved_property("Arc", "number", read_number<double, &E::arc>,
                                                       write_number<&E::set_arc>, "360"),
                                    0.0, 360.0),
                         arc_applies),
        lua_saved_property("EmitDirection", "Vector3", read_vec3<&E::emit_direction>,
                           write_vec3<&E::set_emit_direction>, "[0,1,0]"),
        lua_saved_enum("EmitDirectionSpace", transform_space_enum(),
                       read_enum<TransformSpace, &E::emit_direction_space, transform_space_enum>,
                       write_enum<&E::set_emit_direction_space>, "\"Local\""),
        lua_saved_property("EmitSpread", "Vector2", read_vec2<&E::emit_spread>, write_vec2<&E::set_emit_spread>,
                           "[0,0]"),
        lua_group("Particle"),
        lua_saved_property("Lifetime", "NumberCurve", read_curve<&E::lifetime>, write_curve<&E::set_lifetime>,
                           R"({"mode":"Range","min":1,"max":2})"),
        lua_saved_property("EmitSpeed", "NumberCurve", read_curve<&E::emit_speed>, write_curve<&E::set_emit_speed>,
                           R"({"mode":"Constant","value":5})"),
        lua_saved_property("EmitRotation", "NumberCurve", read_curve<&E::emit_rotation>,
                           write_curve<&E::set_emit_rotation>, R"({"mode":"Constant","value":0})"),
        lua_saved_property("RotationSpeed", "NumberCurve", read_curve<&E::rotation_speed>,
                           write_curve<&E::set_rotation_speed>, R"({"mode":"Constant","value":0})"),
        lua_group("Motion"),
        lua_saved_enum("SimulationSpace", transform_space_enum(),
                       read_enum<TransformSpace, &E::simulation_space, transform_space_enum>,
                       write_enum<&E::set_simulation_space>, "\"World\""),
        lua_enabled_when(lua_slider(lua_saved_property("InheritVelocity", "number",
                                                       read_number<double, &E::inherit_velocity>,
                                                       write_number<&E::set_inherit_velocity>, "0"),
                                    0.0, 1.0),
                         simulates_in_world),
        lua_saved_property("Acceleration", "Vector3", read_vec3<&E::acceleration>, write_vec3<&E::set_acceleration>,
                           "[0,0,0]"),
        lua_saved_enum("AccelerationSpace", transform_space_enum(),
                       read_enum<TransformSpace, &E::acceleration_space, transform_space_enum>,
                       write_enum<&E::set_acceleration_space>, "\"World\""),
        lua_saved_property("Drag", "NumberCurve", read_curve<&E::drag>, write_curve<&E::set_drag>,
                           R"({"mode":"Constant","value":0})"),
        lua_saved_property("OrbitSpeed", "NumberCurve", read_curve<&E::orbit_speed>, write_curve<&E::set_orbit_speed>,
                           R"({"mode":"Constant","value":0})"),
        lua_saved_property("OrbitAxis", "Vector3", read_vec3<&E::orbit_axis>, write_vec3<&E::set_orbit_axis>,
                           "[0,1,0]"),
        lua_group("Collision"),
        lua_saved_enum("Collision", particle_collision_enum(),
                       read_enum<ParticleCollision, &E::collision, particle_collision_enum>,
                       write_enum<&E::set_collision>, "\"None\""),
        lua_enabled_when(lua_saved_enum("CollisionResponse", particle_collision_response_enum(),
                                        read_enum<ParticleCollisionResponse, &E::collision_response,
                                                  particle_collision_response_enum>,
                                        write_enum<&E::set_collision_response>, "\"Bounce\""),
                         collides),
        lua_shown_when(lua_slider(lua_saved_property("Bounciness", "number", read_number<double, &E::bounciness>,
                                                     write_number<&E::set_bounciness>, "0.5"),
                                  0.0, 1.0),
                       "CollisionResponse", {bounce}),
        lua_shown_when(lua_slider(lua_saved_property("Friction", "number", read_number<double, &E::friction>,
                                                     write_number<&E::set_friction>, "0.2"),
                                  0.0, 1.0),
                       "CollisionResponse", {bounce}),
        lua_enabled_when(lua_choices(lua_saved_property("CollisionGroup", "string", read_collision_group,
                                                        write_collision_group, "\"Default\""),
                                     collision_group_choices, "Edit Collision Groups…"),
                         collides_with_world),
        lua_group("Appearance"),
        lua_saved_property("Texture", "Texture?", read_texture, write_texture, "null"),
        lua_saved_enum("TextureMode", particle_texture_mode_enum(),
                       read_enum<ParticleTextureMode, &E::texture_mode, particle_texture_mode_enum>,
                       write_enum<&E::set_texture_mode>, "\"Static\""),
        lua_shown_when(lua_saved_property("FlipbookGrid", "Vector2", read_vec2<&E::flipbook_grid>,
                                          write_vec2<&E::set_flipbook_grid>, "[1,1]"),
                       "TextureMode", {flipbook}),
        lua_saved_property("Color", "ColorCurve", read_color, write_color,
                           R"({"mode":"Constant","color":[1,1,1],"transparency":0})"),
        lua_saved_property("Size", "NumberCurve", read_curve<&E::size>, write_curve<&E::set_size>,
                           R"({"mode":"Constant","value":1})"),
        lua_saved_property("Brightness", "number", read_number<double, &E::brightness>,
                           write_number<&E::set_brightness>, "1"),
        lua_saved_property("Stretch", "NumberCurve", read_curve<&E::stretch>, write_curve<&E::set_stretch>,
                           R"({"mode":"Constant","value":0})"),
        lua_group("Rendering"),
        lua_saved_enum("Facing", particle_facing_enum(), read_enum<ParticleFacing, &E::facing, particle_facing_enum>,
                       write_enum<&E::set_facing>, "\"Camera\""),
        lua_shown_when(lua_saved_property("FacingAxis", "Vector3", read_vec3<&E::facing_axis>,
                                          write_vec3<&E::set_facing_axis>, "[0,1,0]"),
                       "Facing", {world_facing}),
        lua_shown_when(lua_saved_enum("FacingAxisSpace", transform_space_enum(),
                                      read_enum<TransformSpace, &E::facing_axis_space, transform_space_enum>,
                                      write_enum<&E::set_facing_axis_space>, "\"Local\""),
                       "Facing", {world_facing}),
        lua_shown_when(lua_saved_property("VelocityStretch", "number", read_number<double, &E::velocity_stretch>,
                                          write_number<&E::set_velocity_stretch>, "0.1"),
                       "Facing", {velocity_facing}),
        lua_saved_enum("BlendMode", particle_blend_mode_enum(),
                       read_enum<ParticleBlendMode, &E::blend_mode, particle_blend_mode_enum>,
                       write_enum<&E::set_blend_mode>, "\"Normal\""),
        lua_saved_property("SoftEdge", "number", read_number<double, &E::soft_edge>, write_number<&E::set_soft_edge>,
                           "0.5"),
        lua_slider(lua_saved_property("LightInfluence", "number", read_number<double, &E::light_influence>,
                                      write_number<&E::set_light_influence>, "1"),
                   0.0, 1.0),
        lua_saved_property("DrawOrder", "number", read_number<int, &E::draw_order>, write_number<&E::set_draw_order>,
                           "0"),
        lua_property("ParticleCount", "number", false, read_particle_count, nullptr),
        lua_event("Finished"),
    };
    register_lua_class("ParticleEmitter", "Instance", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("ParticleEmitter", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
