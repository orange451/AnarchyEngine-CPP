#pragma once

#include "SpatialObject.hpp"
#include "InstanceRef.hpp"

#include <optional>
#include <string>

namespace engine_core {

// A SpatialObject that draws a Prefab, tinted by its Color and Transparency
// and sized by its Scale. A PhysicsObject may move it.
class GameObject : public SpatialObject {
public:
    using SpatialObject::SpatialObject;

    const char* class_name() const override { return "GameObject"; }

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
    // as a Material's is.
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

protected:
    void on_reuse() override;

private:
    InstanceRef prefab_ref_;
    ColorRgb color_ = kDefaultColor;
    double transparency_ = kDefaultTransparency;
    double scale_ = kDefaultScale;
};

}  // namespace engine_core
