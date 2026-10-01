#include "GameObject.hpp"

#include "Ecs.hpp"
#include "Containment.hpp"
#include "LuaApi.hpp"

#include <cstring>
#include <type_traits>

namespace engine_core {

GameObject::GameObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : PVInstance(tag, state, id) {
    reset_spatial();
}

void GameObject::set_transform(const Matrix4& transform) { apply_transform(id_, transform, false); }

void GameObject::set_transform(const Matrix4& transform, ForceSimWrite) { apply_transform(id_, transform, true); }

void GameObject::set_position(const Vec3& position) {
    Matrix4 moved = transform();
    moved.m[12] = position.x;
    moved.m[13] = position.y;
    moved.m[14] = position.z;
    set_transform(moved);
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
Matrix4 GameObject::transform() const {
    const Matrix4* value = read_component<Matrix4>(ecs_world(), entity_of(id_), component_ids().transform);
    return value != nullptr ? *value : Matrix4{};
}

Vec3 GameObject::position() const {
    const Matrix4 value = transform();
    return Vec3{value.m[12], value.m[13], value.m[14]};
}

void GameObject::store_transform(const Matrix4& transform) {
    write_component(ecs_world(), entity_of(id_), component_ids().transform, transform);
}

LuaSlot GameObject::prefab() const {
    if (!alive(id_)) {
        return LuaSlot();
    }
    return instance_reference_slot(prefab_ref_, "Prefab");
}

std::optional<std::string> GameObject::set_prefab(const LuaSlot& value) {
    require_simulation_thread("set_prefab runs on SimulationThread");
    std::optional<std::string> error = set_instance_reference("Prefab", "Prefab", prefab_ref_, value);
    if (!error) {
        // The snapshot row keeps the GUID, and finds the Prefab's Models at each Prepare.
        note(id_, VisualField::Prefab, current_origin());
    }
    return error;
}

void GameObject::save_properties(PropertyBag& out) const {
    DataModel::save_properties(out);
    const Matrix4 transform_value = transform();
    const Matrix4 identity = matrix4_identity();
    if (std::memcmp(transform_value.m, identity.m, sizeof(identity.m)) != 0) {
        bag_set(out, "Transform", json_floats(transform_value.m, 16));
    }
}

void GameObject::default_properties(PropertyBag& out) const {
    DataModel::default_properties(out);
    const Matrix4 identity = matrix4_identity();
    bag_set(out, "Transform", json_floats(identity.m, 16));
}

bool GameObject::load_property(const std::string& key, const JsonValue& value, std::string& error) {
    if (key == "Transform") {
        std::vector<float> floats;
        if (!read_json_floats(value, 16, 16, floats)) {
            error = "Transform must be 16 numbers, column-major";
            return true;
        }
        Matrix4 transform;
        std::memcpy(transform.m, floats.data(), sizeof(transform.m));
        set_transform(transform);
        return true;
    }
    return DataModel::load_property(key, value, error);
}

void GameObject::note_visual(VisualField fields) { note(id_, fields, current_origin()); }

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
    write_component(world, entity, ids.transform, matrix4_identity());
    write_component(world, entity, ids.velocity, ecs::Velocity{});
}

void GameObject::write_place(std::vector<std::byte>& out) const {
    static_assert(std::is_trivially_copyable<Matrix4>::value, "place blob must be memcpy-safe");
    const Matrix4 pose = transform();
    const auto* bytes = reinterpret_cast<const std::byte*>(&pose);
    out.insert(out.end(), bytes, bytes + sizeof(pose));
    // Prefab, the only saved registry property, follows as DataModel's JSON blob.
    DataModel::write_place(out);
}

void GameObject::read_place(const std::byte* data, std::size_t size) {
    if (data == nullptr || size < sizeof(Matrix4)) {
        reset_spatial();
        DataModel::read_place(nullptr, 0);
        return;
    }
    Matrix4 pose;
    std::memcpy(&pose, data, sizeof(pose));
    const std::uint64_t entity = entity_of(id_);
    ecs_world_t* world = ecs_world();
    const EcsIds& ids = component_ids();
    write_component(world, entity, ids.transform, pose);
    // Velocity is session-only. A place restore always clears it.
    write_component(world, entity, ids.velocity, ecs::Velocity{});
    DataModel::read_place(data + sizeof(Matrix4), size - sizeof(Matrix4));
}

namespace {

bool read_lua_transform(DataModel&, DataModel& object, LuaSlot& out) {
    auto* body = dynamic_cast<GameObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Matrix4;
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
    // PVInstance has no source file of its own, which would not stay linked.
    // It is abstract, and adds no members: IsA("PVInstance") is true of every
    // class with a Transform.
    register_lua_class("PVInstance", "Instance", nullptr, 0);
    const LuaField fields[] = {
        lua_property("Transform", "Matrix4", true, read_lua_transform, write_lua_transform),
        lua_saved_property("Prefab", "Prefab?", read_prefab, write_prefab, "null"),
    };
    register_lua_class("GameObject", "PVInstance", fields, 2);
    // Camera and the Lights inherit these.
    register_suited_parents("GameObject", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
