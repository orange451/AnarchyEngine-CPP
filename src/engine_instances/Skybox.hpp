#pragma once

#include "DataModel.hpp"
#include "InstanceRef.hpp"

#include <optional>
#include <string>

namespace engine_core {

// The sky around the place: an equirectangular image drawn behind everything
// in the Scene View, and the light it gives every surface (image-based
// lighting). It belongs under Lighting, at any depth through Folders, and
// nowhere else. When Lighting holds more than one, the first in the tree is
// the sky. A Texture that is .hdr gives light brighter than white; a PNG, JPEG,
// or any other image the renderer reads is taken as sRGB.
//
// Image        Texture?  the sky. Nil draws no sky, and the surfaces take
//                        the flat stand-in light they had before.
// Exposure     number    how bright the sky is, 1. From 0 to kMaxExposure.
// LightScale   number    multiplies the light the sky gives surfaces, 1,
//                        without changing how the sky looks behind them.
//                        From 0 to kMaxLightScale.
// Rotation     number    degrees the sky is turned about the world's Y axis,
//                        from 0 up to 360; 360 wraps to 0.
// Tint         Color3    multiplies the sky's color, white.
//
// Each is a saved registry property (lua_saved_property), so DataModel saves,
// loads, undoes, and restores it at Stop. The render snapshot reads them at
// every Prepare (VisualSky), as it reads Lighting's.
class Skybox : public DataModel {
public:
    static constexpr double kDefaultExposure = 1.0;
    static constexpr double kMaxExposure = 10.0;
    static constexpr double kDefaultLightScale = 1.0;
    static constexpr double kMaxLightScale = 10.0;
    static constexpr double kDefaultRotation = 0.0;
    static constexpr ColorRgb kDefaultTint{1.f, 1.f, 1.f, 1.f};

    Skybox(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "Skybox"; }

    LuaSlot image() const;
    double exposure() const { return exposure_; }
    double light_scale() const { return light_scale_; }
    double rotation() const { return rotation_; }
    ColorRgb tint() const { return tint_; }

    // SimulationThread. Each returns why it refused the value, changing
    // nothing. A number that is not finite is refused; Exposure and LightScale
    // are clamped to their ranges and Rotation wraps into its.
    std::optional<std::string> set_image(const LuaSlot& value);
    std::optional<std::string> set_exposure(double value);
    std::optional<std::string> set_light_scale(double value);
    std::optional<std::string> set_rotation(double degrees);
    std::optional<std::string> set_tint(ColorRgb color);

protected:
    void on_reuse() override;

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value);

    InstanceRef image_ref_;
    double exposure_ = kDefaultExposure;
    double light_scale_ = kDefaultLightScale;
    double rotation_ = kDefaultRotation;
    ColorRgb tint_ = kDefaultTint;
};

}  // namespace engine_core
