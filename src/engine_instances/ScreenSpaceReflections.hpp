#pragma once

#include "DataModel.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Reflections of what is on screen, on smooth opaque surfaces, in the Scene
// View and the player. Where a reflected ray finds nothing on screen the
// surface keeps the sky's reflection. Like a Skybox it belongs under
// Lighting, at any depth through Folders, and nowhere else; when Lighting
// holds more than one, the first in the tree is used.
//
// Enabled       boolean  false traces nothing. True.
// Intensity     number   how much of the sky reflection the traced one
//                        replaces, 1. From 0 to kMaxIntensity.
// MaxDistance   number   how far a ray may travel, in studs, 50; it fades
//                        out over the last quarter. From 0 to kMaxMaxDistance.
// MaxRoughness  number   rougher surfaces keep the sky reflection, 0.3; it
//                        fades in over the last fifth below. From 0 to 1.
//
// Each is a saved registry property (lua_saved_property). The render
// snapshot reads them at every Prepare (VisualReflections).
class ScreenSpaceReflections : public DataModel {
public:
    static constexpr bool kDefaultEnabled = true;
    static constexpr double kDefaultIntensity = 1.0;
    static constexpr double kMaxIntensity = 1.0;
    static constexpr double kDefaultMaxDistance = 50.0;
    static constexpr double kMaxMaxDistance = 1000.0;
    static constexpr double kDefaultMaxRoughness = 0.3;
    static constexpr double kMaxMaxRoughness = 1.0;

    ScreenSpaceReflections(DataModel::ChildTag tag, DataModel::State& state, InstanceId id)
        : DataModel(tag, state, id) {}
    const char* class_name() const override { return "ScreenSpaceReflections"; }

    bool enabled() const { return enabled_; }
    double intensity() const { return intensity_; }
    double max_distance() const { return max_distance_; }
    double max_roughness() const { return max_roughness_; }

    // SimulationThread. Each returns why it refused the value, changing
    // nothing. A number that is not finite is refused; the others are
    // clamped to their ranges.
    std::optional<std::string> set_enabled(bool value);
    std::optional<std::string> set_intensity(double value);
    std::optional<std::string> set_max_distance(double value);
    std::optional<std::string> set_max_roughness(double value);

protected:
    void on_reuse() override;

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value, double max);

    bool enabled_ = kDefaultEnabled;
    double intensity_ = kDefaultIntensity;
    double max_distance_ = kDefaultMaxDistance;
    double max_roughness_ = kDefaultMaxRoughness;
};

}  // namespace engine_core
