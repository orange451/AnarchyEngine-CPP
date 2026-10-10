#pragma once

#include "LuaApi.hpp"
#include "PVInstance.hpp"

#include <optional>
#include <string>

namespace engine_core {

// A PVInstance whose Transform and velocity live in flecs components on its
// entity (Ecs.hpp), so render and physics queries read them where they are
// stored, and DataModel checks, records, and undoes each Transform write.
// GameObject, Camera, and the Lights are SpatialObjects. Lua knows no class
// by this name: to scripts each is a PVInstance.
class SpatialObject : public PVInstance {
public:
    SpatialObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);

    void set_transform(const Matrix4& transform);
    void set_transform(const Matrix4& transform, ForceSimWrite);
    std::optional<std::string> set_pv_transform(const Matrix4& transform) override {
        set_transform(transform);
        return std::nullopt;
    }
    // The Transform's translation. A write keeps the rotation and is a
    // Transform write: it is checked, recorded, and changes as Transform.
    void set_position(const Vec3& position);
    void set_linear_velocity(float x, float y, float z);

    // A dead id fails closed: transform() is a zero matrix, not a recycled slot.
    Matrix4 transform() const override;
    Vec3 position() const;

    // Transform when it differs from a new object's.
    void save_properties(PropertyBag& out) const override;
    void default_properties(PropertyBag& out) const override;
    bool load_property(const std::string& key, const JsonValue& value, std::string& error) override;

protected:
    // Tells the render snapshot that fields of this row changed, as a
    // subclass's own visual property does (Camera's FieldOfView).
    void note_visual(VisualField fields);
    void on_reuse() override;
    void write_place(std::vector<std::byte>& out) const override;
    void read_place(const std::byte* data, std::size_t size) override;

private:
    friend class DataModel;

    // Sets the spatial components to a new object's values.
    void reset_spatial();
    // The component write behind DataModel's checked transform setter.
    void store_transform(const Matrix4& transform);
};

// The Transform property each subclass lists among its Lua fields.
LuaField lua_spatial_transform();

}  // namespace engine_core
