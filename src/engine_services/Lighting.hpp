#pragma once

#include "SceneService.hpp"

#include <optional>
#include <string>

namespace engine_core {

// How the scene is lit, set through its properties. Each is a saved registry
// property (lua_saved_property), so DataModel saves, loads, undoes, and
// restores it at Stop; this class keeps the values and checks them. The render
// snapshot carries Ambient, Exposure, Saturation, and Gamma (VisualLighting);
// the renderer does not read the others yet.
//
// Ambient     Color3  the light every surface gets, from no direction.
// Brightness  number  how strong the sun is. Not below 0.
// FogColor    Color3
// FogStart    number  studs from the camera where fog begins. Not below 0.
// FogEnd      number  studs from the camera where fog is solid. Not below 0.
// Exposure    number  how much the camera takes in before tone mapping. Not below 0.
// Saturation  number  1 leaves color as it is, 0 is gray. Not below 0.
// Gamma       number  the display's gamma the image is corrected for. Not below 0.
class Lighting : public SceneService {
public:
    static constexpr ColorRgb kDefaultAmbient{0.5f, 0.5f, 0.5f, 1.f};
    static constexpr double kDefaultBrightness = 2.0;
    static constexpr ColorRgb kDefaultFogColor{0.75f, 0.75f, 0.75f, 1.f};
    static constexpr double kDefaultFogStart = 0.0;
    static constexpr double kDefaultFogEnd = 100000.0;
    static constexpr double kDefaultExposure = 1.0;
    static constexpr double kDefaultSaturation = 1.2;
    static constexpr double kDefaultGamma = 2.2;

    using SceneService::SceneService;
    const char* class_name() const override;

    // Setters run on SimulationThread. A value that is not finite is refused:
    // the setter returns why and changes nothing. Otherwise empty.
    ColorRgb ambient() const { return ambient_; }
    std::optional<std::string> set_ambient(ColorRgb color);
    double brightness() const { return brightness_; }
    std::optional<std::string> set_brightness(double value);
    ColorRgb fog_color() const { return fog_color_; }
    std::optional<std::string> set_fog_color(ColorRgb color);
    double fog_start() const { return fog_start_; }
    std::optional<std::string> set_fog_start(double value);
    double fog_end() const { return fog_end_; }
    std::optional<std::string> set_fog_end(double value);
    double exposure() const { return exposure_; }
    std::optional<std::string> set_exposure(double value);
    double saturation() const { return saturation_; }
    std::optional<std::string> set_saturation(double value);
    double gamma() const { return gamma_; }
    std::optional<std::string> set_gamma(double value);

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value);
    std::optional<std::string> set_color(const char* property, ColorRgb& slot, ColorRgb color);

    ColorRgb ambient_ = kDefaultAmbient;
    double brightness_ = kDefaultBrightness;
    ColorRgb fog_color_ = kDefaultFogColor;
    double fog_start_ = kDefaultFogStart;
    double fog_end_ = kDefaultFogEnd;
    double exposure_ = kDefaultExposure;
    double saturation_ = kDefaultSaturation;
    double gamma_ = kDefaultGamma;
};

}  // namespace engine_core
