#pragma once

#include "PVInstance.hpp"
#include "InstanceRef.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Spatial instance. Plain DataModel instances do not have these fields. They
// live in flecs components on the instance's entity (Ecs.hpp), so render and
// physics queries read them where they are stored.
class GameObject : public PVInstance {
public:
    GameObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);

    const char* class_name() const override { return "GameObject"; }

    void set_transform(const Matrix4& transform);
    void set_transform(const Matrix4& transform, ForceSimWrite);
    // The Transform's translation. A write keeps the rotation and is a
    // Transform write: it is checked, recorded, and changes as Transform.
    void set_position(const Vec3& position);
    void set_linear_velocity(float x, float y, float z);

    // False for a class whose moves are a viewpoint, not content: its Transform
    // writes still mark the place changed, so they save, but are not undo steps.
    virtual bool transform_in_history() const { return true; }

    // A dead id fails closed: transform() is a zero matrix, not a recycled slot.
    Matrix4 transform() const override;
    Vec3 position() const;

    // Nil by default. A write of another live instance whose class inherits
    // Prefab is stored by GUID; any other is refused. Held like Material's
    // references, through DataModel::set_instance_reference.
    LuaSlot prefab() const;
    std::optional<std::string> set_prefab(const LuaSlot& value);
    // The Prefab's GUID as stored, whether or not an instance holds it now. Empty for none.
    const std::string& prefab_guid() const { return prefab_ref_.guid(); }

    // Transform when it differs from a new GameObject's.
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

    // Sets the spatial components to a new GameObject's values.
    void reset_spatial();
    // The component write behind DataModel's checked transform setter.
    void store_transform(const Matrix4& transform);

    InstanceRef prefab_ref_;
};

}  // namespace engine_core
