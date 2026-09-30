#include "GameObject.hpp"

#include "Ecs.hpp"
#include "LuaApi.hpp"

#include <cstring>
#include <type_traits>

namespace engine_core {

GameObject::GameObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {
    reset_spatial();
}

void GameObject::set_transform(const Transform& transform) { apply_transform(id_, transform, false); }

void GameObject::set_transform(const Transform& transform, ForceSimWrite) { apply_transform(id_, transform, true); }

void GameObject::set_color(ColorRgb color) { apply_color(id_, color, false); }

void GameObject::set_color(ColorRgb color, ForceSimWrite) { apply_color(id_, color, true); }

void GameObject::set_size(float x, float y, float z) {
    require_simulation_thread("set_size runs on SimulationThread");
    Slot* part = slot(id_);
    if (part == nullptr) {
        contract_fail("size write on a dead instance");
    }
    if (part->instance != this) {
        contract_fail("size write on an instance that is not a GameObject");
    }
    const EcsIds& ids = component_ids();
    const ecs::Size* current = read_component<ecs::Size>(ecs_world(), part->entity, ids.size);
    const ecs::Size previous = current != nullptr ? *current : ecs::Size{};
    if (previous.x == x && previous.y == y && previous.z == z) {
        return;
    }
    write_component(ecs_world(), part->entity, ids.size, ecs::Size{x, y, z});
    record_size(id_, previous.x, previous.y, previous.z, x, y, z);
    const WriteOrigin origin = current_origin();
    note(id_, VisualField::Size, origin);
    emit_change(id_, Field::Size, origin);
}

void GameObject::set_linear_velocity(float x, float y, float z) {
    require_simulation_thread("set_linear_velocity runs on SimulationThread");
    Slot* part = slot(id_);
    if (part == nullptr) {
        contract_fail("velocity write on a dead instance");
    }
    if (part->instance != this) {
        contract_fail("velocity write on an instance that is not a GameObject");
    }
    const EcsIds& ids = component_ids();
    const ecs::Velocity* current = read_component<ecs::Velocity>(ecs_world(), part->entity, ids.velocity);
    if (current != nullptr && current->x == x && current->y == y && current->z == z) {
        return;
    }
    write_component(ecs_world(), part->entity, ids.velocity, ecs::Velocity{x, y, z});
    emit_change(id_, Field::LinearVelocity, current_origin());
}

// A dead id has no entity, so each read below fails closed.
Transform GameObject::transform() const {
    const Transform* value = read_component<Transform>(ecs_world(), entity_of(id_), component_ids().transform);
    return value != nullptr ? *value : Transform{};
}

ColorRgb GameObject::color() const {
    const ColorRgb* value = read_component<ColorRgb>(ecs_world(), entity_of(id_), component_ids().color);
    return value != nullptr ? *value : ColorRgb{};
}

bool GameObject::copy_size(float out[3]) const {
    const ecs::Size* value = read_component<ecs::Size>(ecs_world(), entity_of(id_), component_ids().size);
    if (value == nullptr) {
        return false;
    }
    out[0] = value->x;
    out[1] = value->y;
    out[2] = value->z;
    return true;
}

void GameObject::store_transform(const Transform& transform) {
    write_component(ecs_world(), entity_of(id_), component_ids().transform, transform);
}

void GameObject::store_color(ColorRgb color) {
    write_component(ecs_world(), entity_of(id_), component_ids().color, color);
}

LuaSlot GameObject::prefab() const {
    if (!alive(id_)) {
        return LuaSlot();
    }
    return instance_reference_slot(prefab_ref_, "Prefab");
}

std::optional<std::string> GameObject::set_prefab(const LuaSlot& value) {
    require_simulation_thread("set_prefab runs on SimulationThread");
    return set_instance_reference("Prefab", "Prefab", prefab_ref_, value);
}

void GameObject::save_properties(PropertyBag& out) const {
    DataModel::save_properties(out);
    const Transform transform_value = transform();
    const Transform identity = transform_identity();
    if (std::memcmp(transform_value.m, identity.m, sizeof(identity.m)) != 0) {
        bag_set(out, "Transform", json_floats(transform_value.m, 16));
    }
    const ColorRgb color_value = color();
    const ColorRgb white{};
    if (color_value.r != white.r || color_value.g != white.g || color_value.b != white.b ||
        color_value.a != white.a) {
        // Opaque colors write three channels.
        const float channels[4] = {color_value.r, color_value.g, color_value.b, color_value.a};
        bag_set(out, "Color", json_floats(channels, color_value.a == 1.f ? 3 : 4));
    }
    float size[3] = {1.f, 1.f, 1.f};
    copy_size(size);
    if (size[0] != 1.f || size[1] != 1.f || size[2] != 1.f) {
        bag_set(out, "Size", json_floats(size, 3));
    }
}

void GameObject::default_properties(PropertyBag& out) const {
    DataModel::default_properties(out);
    const Transform identity = transform_identity();
    bag_set(out, "Transform", json_floats(identity.m, 16));
    const ColorRgb white{};
    const float channels[3] = {white.r, white.g, white.b};
    bag_set(out, "Color", json_floats(channels, 3));
    const float one[3] = {1.f, 1.f, 1.f};
    bag_set(out, "Size", json_floats(one, 3));
}

bool GameObject::load_property(const std::string& key, const JsonValue& value, std::string& error) {
    std::vector<float> floats;
    if (key == "Transform") {
        if (!read_json_floats(value, 16, 16, floats)) {
            error = "Transform must be 16 numbers, column-major";
            return true;
        }
        Transform transform;
        std::memcpy(transform.m, floats.data(), sizeof(transform.m));
        set_transform(transform);
        return true;
    }
    if (key == "Color") {
        if (!read_json_floats(value, 3, 4, floats)) {
            error = "Color must be 3 or 4 numbers";
            return true;
        }
        ColorRgb color;
        color.r = floats[0];
        color.g = floats[1];
        color.b = floats[2];
        color.a = floats.size() == 4 ? floats[3] : 1.f;
        set_color(color);
        return true;
    }
    if (key == "Size") {
        if (!read_json_floats(value, 3, 3, floats)) {
            error = "Size must be 3 numbers";
            return true;
        }
        set_size(floats[0], floats[1], floats[2]);
        return true;
    }
    return DataModel::load_property(key, value, error);
}

void GameObject::on_reuse() {
    reset_spatial();
    prefab_ref_.set_guid(std::string());
}

void GameObject::reset_spatial() {
    const std::uint64_t entity = entity_of(id_);
    if (entity == 0) {
        return;
    }
    ecs_world_t* world = ecs_world();
    const EcsIds& ids = component_ids();
    write_component(world, entity, ids.transform, transform_identity());
    write_component(world, entity, ids.color, ColorRgb{});
    write_component(world, entity, ids.size, ecs::Size{});
    write_component(world, entity, ids.velocity, ecs::Velocity{});
}

namespace {

struct SpatialPlace {
    Transform transform = transform_identity();
    ColorRgb color{};
    float size[3] = {1.f, 1.f, 1.f};
};

}  // namespace

void GameObject::write_place(std::vector<std::byte>& out) const {
    static_assert(std::is_trivially_copyable<SpatialPlace>::value, "place blob must be memcpy-safe");
    SpatialPlace pod;
    pod.transform = transform();
    pod.color = color();
    copy_size(pod.size);
    const auto* bytes = reinterpret_cast<const std::byte*>(&pod);
    out.insert(out.end(), bytes, bytes + sizeof(pod));
    // Prefab, the only saved registry property, follows as DataModel's JSON blob.
    DataModel::write_place(out);
}

void GameObject::read_place(const std::byte* data, std::size_t size) {
    if (data == nullptr || size < sizeof(SpatialPlace)) {
        reset_spatial();
        DataModel::read_place(nullptr, 0);
        return;
    }
    SpatialPlace pod;
    std::memcpy(&pod, data, sizeof(pod));
    const std::uint64_t entity = entity_of(id_);
    ecs_world_t* world = ecs_world();
    const EcsIds& ids = component_ids();
    write_component(world, entity, ids.transform, pod.transform);
    write_component(world, entity, ids.color, pod.color);
    write_component(world, entity, ids.size, ecs::Size{pod.size[0], pod.size[1], pod.size[2]});
    // Velocity is session-only. A place restore always clears it.
    write_component(world, entity, ids.velocity, ecs::Velocity{});
    DataModel::read_place(data + sizeof(SpatialPlace), size - sizeof(SpatialPlace));
}

namespace {

bool read_lua_color(DataModel&, DataModel& object, LuaSlot& out) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Color;
    out.color = body->color();
    return true;
}

bool write_lua_color(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    body->set_color(in.color);
    return true;
}

bool read_lua_transform(DataModel&, DataModel& object, LuaSlot& out) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Transform;
    out.transform = body->transform();
    return true;
}

bool write_lua_transform(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    body->set_transform(in.transform);
    return true;
}

bool read_prefab(DataModel&, DataModel& object, LuaSlot& out) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = body->prefab();
    return true;
}

bool write_prefab(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    if (std::optional<std::string> error = body->set_prefab(in)) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

ANARCHY_LUA_REGISTER(register_game_object_lua) {
    const LuaField fields[] = {
        lua_property("Color", "Color3", true, read_lua_color, write_lua_color),
        lua_property("Transform", "Transform", true, read_lua_transform, write_lua_transform),
        lua_property("CFrame", "Transform", true, read_lua_transform, write_lua_transform),
        lua_saved_property("Prefab", "Prefab?", read_prefab, write_prefab, "null"),
    };
    register_lua_class("GameObject", "Instance", fields, 4);
}

}  // namespace

}  // namespace engine_core
