#pragma once

#include "DataModel.hpp"
#include "Enum.hpp"
#include "InstanceRef.hpp"
#include "Vector3.hpp"

#include <optional>
#include <string>

namespace engine_core {

// The sky drawn from a shader instead of an image: an atmosphere that is blue
// at noon, warm at sunset, and dark at night, with drifting clouds, the sun
// by day, and the moon and stars by night. It lights the place with its own
// directional light from the sun or the moon. Like a Skybox it belongs under
// Lighting, at any depth through Folders, and nowhere else; the first Skybox
// or DynamicSky in the tree is the sky.
//
// TimeOfDay          number              hours, wrapped into 0 up to 24. 14.
// Latitude           number              degrees, -90 to 90: how high the sun
//                                        climbs (90 - |Latitude| at noon). 35.
// Brightness         number              the sun light's intensity; the moon's
//                                        is a tenth of it. 3, 0 to kMaxBrightness.
// Shadows            boolean             whether the sun or moon light casts shadows. True.
// CloudCover         number              how much of the sky has cloud, 0 to 1. 0.5.
// CloudDensity       number              how thick the clouds are, 0 to 1. 0.5.
// WindDirection      Vector3             the clouds' drift, studs per second; Y is ignored.
// SunTexture         Texture?            drawn in place of the sun's disc. Nil.
// MoonTexture        Texture?            drawn in place of the moon's disc. Nil.
// SunSize, MoonSize  number              degrees across, kMinBodySize to kMaxBodySize. 2.
// ReflectionQuality  Enum.EffectQuality  the size of the cube the sky's light
//                                        and reflections are filtered from:
//                                        Low 128, Medium 256, High 512. Medium.
//
// Each is a saved registry property (lua_saved_property). The render
// snapshot reads them at every Prepare (VisualDynamicSky).
class DynamicSky : public DataModel {
public:
    static constexpr double kDefaultTimeOfDay = 14.0;
    static constexpr double kDefaultLatitude = 35.0;
    static constexpr double kMaxLatitude = 90.0;
    static constexpr double kDefaultBrightness = 3.0;
    static constexpr double kMaxBrightness = 20.0;
    static constexpr bool kDefaultShadows = true;
    static constexpr double kDefaultCloudCover = 0.5;
    static constexpr double kDefaultCloudDensity = 0.5;
    static constexpr Vec3 kDefaultWindDirection{1.f, 0.f, 0.3f};
    static constexpr double kDefaultSunSize = 2.0;
    static constexpr double kDefaultMoonSize = 2.0;
    static constexpr double kMinBodySize = 0.1;
    static constexpr double kMaxBodySize = 20.0;
    static constexpr EffectQuality kDefaultReflectionQuality = EffectQuality::Medium;

    DynamicSky(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "DynamicSky"; }

    double time_of_day() const { return time_of_day_; }
    double latitude() const { return latitude_; }
    double brightness() const { return brightness_; }
    bool shadows() const { return shadows_; }
    double cloud_cover() const { return cloud_cover_; }
    double cloud_density() const { return cloud_density_; }
    Vec3 wind_direction() const { return wind_direction_; }
    LuaSlot sun_texture() const;
    LuaSlot moon_texture() const;
    double sun_size() const { return sun_size_; }
    double moon_size() const { return moon_size_; }
    EffectQuality reflection_quality() const { return reflection_quality_; }

    // SimulationThread. Each returns why it refused the value, changing
    // nothing. A number that is not finite is refused; TimeOfDay wraps and
    // the others clamp into their ranges. A ReflectionQuality that is not an
    // Enum.EffectQuality's value is refused, and so is a texture that is not a Texture.
    std::optional<std::string> set_time_of_day(double hours);
    std::optional<std::string> set_latitude(double degrees);
    std::optional<std::string> set_brightness(double value);
    std::optional<std::string> set_shadows(bool value);
    std::optional<std::string> set_cloud_cover(double value);
    std::optional<std::string> set_cloud_density(double value);
    std::optional<std::string> set_wind_direction(Vec3 value);
    std::optional<std::string> set_sun_texture(const LuaSlot& value);
    std::optional<std::string> set_moon_texture(const LuaSlot& value);
    std::optional<std::string> set_sun_size(double degrees);
    std::optional<std::string> set_moon_size(double degrees);
    std::optional<std::string> set_reflection_quality(int value);

protected:
    void on_reuse() override;

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value, double min, double max);

    double time_of_day_ = kDefaultTimeOfDay;
    double latitude_ = kDefaultLatitude;
    double brightness_ = kDefaultBrightness;
    bool shadows_ = kDefaultShadows;
    double cloud_cover_ = kDefaultCloudCover;
    double cloud_density_ = kDefaultCloudDensity;
    Vec3 wind_direction_ = kDefaultWindDirection;
    InstanceRef sun_texture_ref_;
    InstanceRef moon_texture_ref_;
    double sun_size_ = kDefaultSunSize;
    double moon_size_ = kDefaultMoonSize;
    EffectQuality reflection_quality_ = kDefaultReflectionQuality;
};

}  // namespace engine_core
