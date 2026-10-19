#pragma once

#include "DataModel.hpp"

#include <optional>
#include <string>

namespace engine_core {

// The air: distant surfaces fade into it, it glows toward the sun, and the
// sky's horizon goes hazy behind it, in the Scene View and the player. Like a
// BloomEffect it belongs under Lighting, at any depth through Folders, and
// nowhere else. When Lighting holds more than one, the first in the tree is
// the air.
//
// Enabled            boolean  false draws no air. True.
// Density            number   how thick the air is at BaseHeight, 0.25. From
//                             0 to kMaxDensity; 0 draws no air.
// HeightFalloff      number   how fast the air thins going up, per unit,
//                             0.01. From 0 to kMaxHeightFalloff; 0 is as
//                             thick everywhere.
// BaseHeight         number   the world Y where the air is Density thick, 0.
//                             Any finite height.
// Color              Color3   the air's own color, (0.75, 0.8, 0.88).
// AerialPerspective  number   how much the air takes the sky's color in
//                             each direction instead of Color, 0.5. From 0
//                             to 1. With no sky the air is Color.
// SunScatter         number   how strong the glow toward the sun (or the
//                             moon, or the first DirectionalLight) is, in
//                             that light's color, 0.25. From 0 to 1.
// SkyHaze            number   how much the air hazes the sky behind
//                             everything, 1. From 0 to 1.
//
// Each is a saved registry property (lua_saved_property), so DataModel saves,
// loads, undoes, and restores it at Stop. The render snapshot reads them at
// every Prepare (VisualAtmosphere), as it reads the BloomEffect's.
class Atmosphere : public DataModel {
public:
    static constexpr bool kDefaultEnabled = true;
    static constexpr double kDefaultDensity = 0.25;
    static constexpr double kMaxDensity = 1.0;
    static constexpr double kDefaultHeightFalloff = 0.01;
    static constexpr double kMaxHeightFalloff = 0.2;
    static constexpr double kDefaultBaseHeight = 0.0;
    static constexpr ColorRgb kDefaultColor{0.75f, 0.8f, 0.88f, 1.f};
    static constexpr double kDefaultAerialPerspective = 0.5;
    static constexpr double kDefaultSunScatter = 0.25;
    static constexpr double kDefaultSkyHaze = 1.0;

    Atmosphere(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "Atmosphere"; }

    bool enabled() const { return enabled_; }
    double density() const { return density_; }
    double height_falloff() const { return height_falloff_; }
    double base_height() const { return base_height_; }
    ColorRgb color() const { return color_; }
    double aerial_perspective() const { return aerial_perspective_; }
    double sun_scatter() const { return sun_scatter_; }
    double sky_haze() const { return sky_haze_; }

    // SimulationThread. Each returns why it refused the value, changing
    // nothing. A number or color that is not finite is refused; the numbers
    // with a range are clamped to it.
    std::optional<std::string> set_enabled(bool value);
    std::optional<std::string> set_density(double value);
    std::optional<std::string> set_height_falloff(double value);
    std::optional<std::string> set_base_height(double value);
    std::optional<std::string> set_color(ColorRgb color);
    std::optional<std::string> set_aerial_perspective(double value);
    std::optional<std::string> set_sun_scatter(double value);
    std::optional<std::string> set_sky_haze(double value);

protected:
    void on_reuse() override;

private:
    // max below 0 leaves the value unclamped.
    std::optional<std::string> set_number(const char* property, double& slot, double value, double max);

    bool enabled_ = kDefaultEnabled;
    double density_ = kDefaultDensity;
    double height_falloff_ = kDefaultHeightFalloff;
    double base_height_ = kDefaultBaseHeight;
    ColorRgb color_ = kDefaultColor;
    double aerial_perspective_ = kDefaultAerialPerspective;
    double sun_scatter_ = kDefaultSunScatter;
    double sky_haze_ = kDefaultSkyHaze;
};

}  // namespace engine_core
