#pragma once

#include "DataModel.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Bloom: bright light spreading into what surrounds it, in the Scene View and
// the player. Like a Skybox it belongs under Lighting, at any depth through
// Folders, and nowhere else. When Lighting holds more than one, the first in
// the tree is the bloom.
//
// Enabled    boolean  false draws no bloom. True.
// Intensity  number   how much of the image moves into its blurred copy,
//                     0.05. From 0 to kMaxIntensity.
// Size       number   how far light spreads, in pixels at a 1080-pixel-tall
//                     view, 24. From 0 to kMaxSize.
// Threshold  number   the linear brightness where bloom starts, with a soft
//                     knee below it, 0. From 0 to kMaxThreshold. 0 blooms
//                     everything a little: a filmic haze.
//
// Each is a saved registry property (lua_saved_property), so DataModel saves,
// loads, undoes, and restores it at Stop. The render snapshot reads them at
// every Prepare (VisualBloom), as it reads the Skybox's.
class BloomEffect : public DataModel {
public:
    static constexpr bool kDefaultEnabled = true;
    static constexpr double kDefaultIntensity = 0.05;
    static constexpr double kMaxIntensity = 1.0;
    static constexpr double kDefaultSize = 24.0;
    static constexpr double kMaxSize = 56.0;
    static constexpr double kDefaultThreshold = 0.0;
    static constexpr double kMaxThreshold = 10.0;

    BloomEffect(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "BloomEffect"; }

    bool enabled() const { return enabled_; }
    double intensity() const { return intensity_; }
    double size() const { return size_; }
    double threshold() const { return threshold_; }

    // SimulationThread. Each returns why it refused the value, changing
    // nothing. A number that is not finite is refused; the others are
    // clamped to their ranges.
    std::optional<std::string> set_enabled(bool value);
    std::optional<std::string> set_intensity(double value);
    std::optional<std::string> set_size(double value);
    std::optional<std::string> set_threshold(double value);

protected:
    void on_reuse() override;

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value, double max);

    bool enabled_ = kDefaultEnabled;
    double intensity_ = kDefaultIntensity;
    double size_ = kDefaultSize;
    double threshold_ = kDefaultThreshold;
};

}  // namespace engine_core
