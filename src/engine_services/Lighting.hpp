#pragma once

#include "Enum.hpp"
#include "SceneService.hpp"

#include <optional>
#include <string>

namespace engine_core {

// How the scene is lit, set through its properties. Each is a saved registry
// property (lua_saved_property), so DataModel saves, loads, undoes, and
// restores it at Stop; this class keeps the values and checks them. The render
// snapshot carries Ambient, Exposure, Saturation, Gamma, and Antialiasing
// (VisualLighting); the renderer does not read Brightness yet.
//
// Ambient     Color3  the light every surface gets, from no direction.
// Brightness  number  how strong the sun is. Not below 0.
// Exposure    number  how much the camera takes in before tone mapping. Not below 0.
// Saturation  number  1 leaves color as it is, 0 is gray. Not below 0.
// Gamma       number  the display's gamma the image is corrected for. Not below 0.
// Antialiasing  Enum.AntialiasingMode  how the 3D scene's edges are smoothed. FXAA.
// TerrainQuality  Enum.EffectQuality  how much terrain texturing spends for
//                 how good it looks, settable by scripts at runtime. High.
class Lighting : public SceneService {
public:
    static constexpr ColorRgb kDefaultAmbient{0.5f, 0.5f, 0.5f, 1.f};
    static constexpr double kDefaultBrightness = 2.0;
    static constexpr double kDefaultExposure = 1.0;
    static constexpr double kDefaultSaturation = 1.2;
    static constexpr double kDefaultGamma = 2.2;
    static constexpr AntialiasingMode kDefaultAntialiasing = AntialiasingMode::FXAA;
    static constexpr EffectQuality kDefaultTerrainQuality = EffectQuality::High;

    using SceneService::SceneService;
    const char* class_name() const override;

    // Setters run on SimulationThread. A value that is not finite is refused:
    // the setter returns why and changes nothing. Otherwise empty.
    ColorRgb ambient() const { return ambient_; }
    std::optional<std::string> set_ambient(ColorRgb color);
    double brightness() const { return brightness_; }
    std::optional<std::string> set_brightness(double value);
    double exposure() const { return exposure_; }
    std::optional<std::string> set_exposure(double value);
    double saturation() const { return saturation_; }
    std::optional<std::string> set_saturation(double value);
    double gamma() const { return gamma_; }
    std::optional<std::string> set_gamma(double value);
    AntialiasingMode antialiasing() const { return antialiasing_; }
    // An Enum.AntialiasingMode's value; any other is refused.
    std::optional<std::string> set_antialiasing(int mode);
    EffectQuality terrain_quality() const { return terrain_quality_; }
    // An Enum.EffectQuality's value; any other is refused.
    std::optional<std::string> set_terrain_quality(int quality);

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value);
    std::optional<std::string> set_color(const char* property, ColorRgb& slot, ColorRgb color);

    ColorRgb ambient_ = kDefaultAmbient;
    double brightness_ = kDefaultBrightness;
    double exposure_ = kDefaultExposure;
    double saturation_ = kDefaultSaturation;
    double gamma_ = kDefaultGamma;
    AntialiasingMode antialiasing_ = kDefaultAntialiasing;
    EffectQuality terrain_quality_ = kDefaultTerrainQuality;
};

}  // namespace engine_core
