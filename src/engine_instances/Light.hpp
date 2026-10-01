#pragma once

#include "GameObject.hpp"

#include <optional>
#include <string>

namespace engine_core {

// A light the Scene View shades with, at its Transform's translation. Like a
// Camera, it is a GameObject. A Light in Workspace, or anywhere under
// Lighting, has a render snapshot row that carries what it shines
// (VisualLight). Under Lighting it only shines: its Prefab is not drawn.
//
// Color      Color3  white.
// Intensity  number  how bright, 1. Not below 0; the slider runs to 8.
// Radius     number  studs it reaches; it fades to nothing there. 8, not below
//                    0; the slider runs to 64.
// Enabled    boolean when false it gives no light.
//
// Each is a saved registry property (lua_saved_property), so DataModel saves,
// loads, undoes, and restores it at Stop. "Light" itself is only a base class:
// Instance.new makes a PointLight or a SpotLight. A DirectionalLight, below,
// is not one: it has no Transform.
class Light : public GameObject {
public:
    static constexpr ColorRgb kDefaultColor{1.f, 1.f, 1.f, 1.f};
    static constexpr double kDefaultIntensity = 1.0;
    static constexpr double kMaxIntensitySlider = 8.0;
    static constexpr double kDefaultRadius = 8.0;
    static constexpr double kMaxRadiusSlider = 64.0;

    using GameObject::GameObject;

    ColorRgb color() const { return color_; }
    double intensity() const { return intensity_; }
    double radius() const { return radius_; }
    bool enabled() const { return enabled_; }

    // SimulationThread. A value that is not finite is refused: the setter
    // returns why and changes nothing. A negative number is taken as 0.
    std::optional<std::string> set_color(ColorRgb color);
    std::optional<std::string> set_intensity(double value);
    std::optional<std::string> set_radius(double value);
    void set_enabled(bool enabled);

protected:
    void on_reuse() override;
    // Sets a number property, as the setters above do, and tells the render snapshot.
    std::optional<std::string> set_number(const char* property, double& slot, double value);

private:
    ColorRgb color_ = kDefaultColor;
    double intensity_ = kDefaultIntensity;
    double radius_ = kDefaultRadius;
    bool enabled_ = true;
};

// Shines every way from its translation, out to Radius.
class PointLight : public Light {
public:
    using Light::Light;
    const char* class_name() const override { return "PointLight"; }
};

// Shines on everything at once and equally, as the sun does. It is not a
// GameObject: it has no Transform and no place, only a Direction, and no
// Radius. In Workspace or under Lighting it still has a render snapshot row
// (has_visual_row).
//
// Direction  Vector3  toward the light, as the legacy engine had it: (0, 1, 0)
//                     shines straight down. (1, 1, 1). Any length; a zero
//                     vector shines nowhere.
// Color      Color3   white.
// Intensity  number   how bright, 1. Not below 0; the slider runs to 8.
// Enabled    boolean  when false it gives no light.
//
// Each is a saved registry property, as a Light's are.
class DirectionalLight : public DataModel {
public:
    static constexpr Vec3 kDefaultDirection{1.f, 1.f, 1.f};

    DirectionalLight(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "DirectionalLight"; }

    Vec3 direction() const { return direction_; }
    ColorRgb color() const { return color_; }
    double intensity() const { return intensity_; }
    bool enabled() const { return enabled_; }

    // SimulationThread. A value that is not finite is refused: the setter
    // returns why and changes nothing. A negative Intensity is taken as 0.
    std::optional<std::string> set_direction(Vec3 direction);
    std::optional<std::string> set_color(ColorRgb color);
    std::optional<std::string> set_intensity(double value);
    void set_enabled(bool enabled);

protected:
    bool has_visual_row() const override { return true; }
    void on_reuse() override;

private:
    Vec3 direction_ = kDefaultDirection;
    ColorRgb color_ = Light::kDefaultColor;
    double intensity_ = Light::kDefaultIntensity;
    bool enabled_ = true;
};

// Shines a cone down its Transform's -Z, as a Camera looks, out to Radius.
//
// OuterFOV       number  the cone's whole angle, in degrees, from 1 to 179. 80.
// InnerFOVScale  number  the part of that angle at full brightness, 0 to 1;
//                        the light fades from there to the cone's edge. 0.1.
class SpotLight : public Light {
public:
    static constexpr double kDefaultOuterFov = 80.0;
    static constexpr double kMinOuterFov = 1.0;
    static constexpr double kMaxOuterFov = 179.0;
    static constexpr double kDefaultInnerFovScale = 0.1;

    using Light::Light;
    const char* class_name() const override { return "SpotLight"; }

    double outer_fov() const { return outer_fov_; }
    double inner_fov_scale() const { return inner_fov_scale_; }
    // SimulationThread. Clamped to their ranges. A value that is not finite
    // is refused: returns why and changes nothing.
    std::optional<std::string> set_outer_fov(double degrees);
    std::optional<std::string> set_inner_fov_scale(double scale);

protected:
    void on_reuse() override;

private:
    double outer_fov_ = kDefaultOuterFov;
    double inner_fov_scale_ = kDefaultInnerFovScale;
};

}  // namespace engine_core
