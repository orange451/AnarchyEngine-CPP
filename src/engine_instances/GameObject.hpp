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

    // Nil by default. A write of another live instance whose class inherits
    // Prefab is stored by GUID; any other is refused. Held like Material's
    // references, through DataModel::set_instance_reference.
    LuaSlot prefab() const;
    std::optional<std::string> set_prefab(const LuaSlot& value);
    // The Prefab's GUID as stored, whether or not an instance holds it now. Empty for none.
    const std::string& prefab_guid() const { return prefab_ref_.guid(); }

    // Color multiplies the Color of each Material the Prefab draws with, and
    // Transparency stacks on each Material's: what shows is the product of
    // their opacities. Saved registry properties, like Camera's FieldOfView.
    // Transparency is stored as any finite number and drawn clamped to 0..1,
    // as a Material's is. A Light's own Color shadows this one, so a Light's
    // Color is the light it gives, and its meshes draw untinted.
    static constexpr ColorRgb kDefaultColor{1.f, 1.f, 1.f, 1.f};
    static constexpr double kDefaultTransparency = 0.0;
    ColorRgb color() const { return color_; }
    double transparency() const { return transparency_; }
    // SimulationThread. A value that is not finite is refused: returns why and changes nothing.
    std::optional<std::string> set_color(ColorRgb color);
    std::optional<std::string> set_transparency(double value);

    // Scale multiplies the size the Prefab draws at, about the GameObject's
    // origin, on top of any scale in the Transform's axes. The shape of the
    // PhysicsObject that moves it grows with both (PhysicsWorld::shape_scale).
    // It is not part of the Transform. A saved registry property, 1 by default.
    static constexpr double kDefaultScale = 1.0;
    double scale() const { return scale_; }
    // SimulationThread. A value that is not finite and above 0 is refused:
    // returns why and changes nothing.
    std::optional<std::string> set_scale(double value);

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
    ColorRgb color_ = kDefaultColor;
    double transparency_ = kDefaultTransparency;
    double scale_ = kDefaultScale;
};

}  // namespace engine_core
