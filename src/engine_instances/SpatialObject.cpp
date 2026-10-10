#include "SpatialObject.hpp"

#include "Ecs.hpp"
#include "PropertyBag.hpp"

#include <cstring>
#include <type_traits>

namespace engine_core {

SpatialObject::SpatialObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id)
    : PVInstance(tag, state, id) {
    reset_spatial();
}

void SpatialObject::set_transform(const Matrix4& transform) { apply_transform(id_, transform, false); }

void SpatialObject::set_transform(const Matrix4& transform, ForceSimWrite) { apply_transform(id_, transform, true); }

void SpatialObject::set_position(const Vec3& position) {
    Matrix4 moved = transform();
    moved.m[12] = position.x;
    moved.m[13] = position.y;
    moved.m[14] = position.z;
    set_transform(moved);
}

void SpatialObject::set_linear_velocity(float x, float y, float z) {
    require_simulation_thread("set_linear_velocity runs on SimulationThread");
    Slot* part = slot(id_);
    if (part == nullptr) {
        contract_fail("velocity write on a dead instance");
    }
    if (part->instance != this) {
        contract_fail("velocity write on an instance that is not a SpatialObject");
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
Matrix4 SpatialObject::transform() const {
    const Matrix4* value = read_component<Matrix4>(ecs_world(), entity_of(id_), component_ids().transform);
    return value != nullptr ? *value : Matrix4{};
}

Vec3 SpatialObject::position() const {
    const Matrix4 value = transform();
    return Vec3{value.m[12], value.m[13], value.m[14]};
}

void SpatialObject::store_transform(const Matrix4& transform) {
    write_component(ecs_world(), entity_of(id_), component_ids().transform, transform);
}

void SpatialObject::save_properties(PropertyBag& out) const {
    DataModel::save_properties(out);
    const Matrix4 transform_value = transform();
    const Matrix4 identity = matrix4_identity();
    if (std::memcmp(transform_value.m, identity.m, sizeof(identity.m)) != 0) {
        bag_set(out, "Transform", json_floats(transform_value.m, 16));
    }
}

void SpatialObject::default_properties(PropertyBag& out) const {
    DataModel::default_properties(out);
    const Matrix4 identity = matrix4_identity();
    bag_set(out, "Transform", json_floats(identity.m, 16));
}

bool SpatialObject::load_property(const std::string& key, const JsonValue& value, std::string& error) {
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

void SpatialObject::note_visual(VisualField fields) { note(id_, fields, current_origin()); }

void SpatialObject::on_reuse() { reset_spatial(); }

void SpatialObject::reset_spatial() {
    const std::uint64_t entity = entity_of(id_);
    if (entity == 0) {
        return;
    }
    ecs_world_t* world = ecs_world();
    const EcsIds& ids = component_ids();
    write_component(world, entity, ids.transform, matrix4_identity());
    write_component(world, entity, ids.velocity, ecs::Velocity{});
}

void SpatialObject::write_place(std::vector<std::byte>& out) const {
    static_assert(std::is_trivially_copyable<Matrix4>::value, "place blob must be memcpy-safe");
    const Matrix4 pose = transform();
    const auto* bytes = reinterpret_cast<const std::byte*>(&pose);
    out.insert(out.end(), bytes, bytes + sizeof(pose));
    // The saved registry properties follow as DataModel's JSON blob.
    DataModel::write_place(out);
}

void SpatialObject::read_place(const std::byte* data, std::size_t size) {
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
    auto* body = dynamic_cast<SpatialObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Matrix4;
    out.transform = body->transform();
    return true;
}

bool write_lua_transform(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<SpatialObject*>(&object);
    if (body == nullptr) {
        return false;
    }
    body->set_transform(in.transform);
    return true;
}

}  // namespace

LuaField lua_spatial_transform() { return lua_property("Transform", "Matrix4", true, read_lua_transform, write_lua_transform); }

}  // namespace engine_core
