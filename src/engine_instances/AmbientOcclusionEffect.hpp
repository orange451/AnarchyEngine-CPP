#pragma once

#include "DataModel.hpp"
#include "Enum.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Shading where nearby geometry hides the sky and the ambient light from a
// surface: in creases, under objects, where a wall meets a floor. Lights are
// not shaded by it; they have shadows. Like a Skybox it belongs under
// Lighting, at any depth through Folders, and nowhere else; when Lighting
// holds more than one, the first in the tree is used.
//
// Enabled    boolean             false shades nothing. True.
// Intensity  number              an exponent on how open a surface is: 1 is
//                                physical, above 1 darker, 0 none. 1, from 0
//                                to kMaxIntensity.
// Radius     number              how far, in studs, an occluder still counts;
//                                it fades over the last 60%. 1, from 0 to
//                                kMaxRadius.
// Quality    Enum.EffectQuality  Low and Medium shade at half resolution,
//                                High at full. Medium.
//
// Each is a saved registry property (lua_saved_property). The render
// snapshot reads them at every Prepare (VisualAmbientOcclusion).
class AmbientOcclusionEffect : public DataModel {
public:
    static constexpr bool kDefaultEnabled = true;
    static constexpr double kDefaultIntensity = 1.0;
    static constexpr double kMaxIntensity = 4.0;
    static constexpr double kDefaultRadius = 1.0;
    static constexpr double kMaxRadius = 10.0;
    static constexpr EffectQuality kDefaultQuality = EffectQuality::Medium;

    AmbientOcclusionEffect(DataModel::ChildTag tag, DataModel::State& state, InstanceId id)
        : DataModel(tag, state, id) {}
    const char* class_name() const override { return "AmbientOcclusionEffect"; }

    bool enabled() const { return enabled_; }
    double intensity() const { return intensity_; }
    double radius() const { return radius_; }
    EffectQuality quality() const { return quality_; }

    // SimulationThread. Each returns why it refused the value, changing
    // nothing. A number that is not finite is refused, and the others are
    // clamped to their ranges; a Quality that is not an Enum.EffectQuality's
    // value is refused.
    std::optional<std::string> set_enabled(bool value);
    std::optional<std::string> set_intensity(double value);
    std::optional<std::string> set_radius(double value);
    std::optional<std::string> set_quality(int value);

protected:
    void on_reuse() override;

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value, double max);

    bool enabled_ = kDefaultEnabled;
    double intensity_ = kDefaultIntensity;
    double radius_ = kDefaultRadius;
    EffectQuality quality_ = kDefaultQuality;
};

}  // namespace engine_core
