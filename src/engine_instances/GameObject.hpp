#pragma once

#include "DataModel.hpp"
#include "InstanceRef.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Spatial instance. Plain DataModel instances do not have these fields. They
// live in flecs components on the instance's entity (Ecs.hpp), so render and
// physics queries read them where they are stored.
class GameObject : public DataModel {
public:
    GameObject(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);

    const char* class_name() const override { return "GameObject"; }

    void set_transform(const Transform& transform);
    void set_transform(const Transform& transform, ForceSimWrite);
    void set_color(ColorRgb color);
    void set_color(ColorRgb color, ForceSimWrite);
    void set_size(float x, float y, float z);
    void set_linear_velocity(float x, float y, float z);

    // A dead id fails closed: transform() is a zero matrix, not a recycled slot.
    Transform transform() const;
    ColorRgb color() const;
    bool copy_size(float out[3]) const;

    // Nil by default. A write of another live instance whose class inherits
    // Prefab is stored by GUID; any other is refused. Held like Material's
    // references, through DataModel::set_instance_reference.
    LuaSlot prefab() const;
    std::optional<std::string> set_prefab(const LuaSlot& value);

    // Transform, Color, and Size when they differ from a new GameObject.
    void save_properties(PropertyBag& out) const override;
    void default_properties(PropertyBag& out) const override;
    bool load_property(const std::string& key, const JsonValue& value, std::string& error) override;

protected:
    void on_reuse() override;
    void write_place(std::vector<std::byte>& out) const override;
    void read_place(const std::byte* data, std::size_t size) override;

private:
    friend class DataModel;

    // Sets the four spatial components to a new GameObject's values.
    void reset_spatial();
    // The component writes behind DataModel's checked transform and color setters.
    void store_transform(const Transform& transform);
    void store_color(ColorRgb color);

    InstanceRef prefab_ref_;
};

}  // namespace engine_core
