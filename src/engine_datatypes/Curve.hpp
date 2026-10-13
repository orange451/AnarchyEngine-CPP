#pragma once

#include "Color.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine_core {

// How a key reaches the next one: straight, a cubic with automatic tangents,
// or held until the next key.
enum class CurveInterp : std::uint8_t { Linear = 0, Smooth = 1, Step = 2 };

// "Linear", "Smooth", or "Step"; nullopt for any other name.
std::optional<CurveInterp> curve_interp_from_name(std::string_view name);
const char* curve_interp_name(CurveInterp interp);

struct NumberKey {
    float time = 0.f;
    float value = 0.f;
    CurveInterp interp = CurveInterp::Linear;

    bool operator==(const NumberKey& other) const {
        return time == other.time && value == other.value && interp == other.interp;
    }
};

enum class NumberCurveMode : std::uint8_t { Constant = 0, Range = 1, Curve = 2, CurveRange = 3 };

// A number that is fixed, random within a range, keyed over 0..1, or a random
// blend of two keyed curves. Immutable. What its time means is the property's.
class NumberCurve {
public:
    static NumberCurve constant(float value);
    static NumberCurve range(float min, float max);
    // Keys are sorted by time on the way in. Refuses no keys, a time outside
    // 0..1, or a time or value that is not finite, saying why in error.
    static std::optional<NumberCurve> keys(std::vector<NumberKey> keys, std::string& error);
    static std::optional<NumberCurve> keys_range(std::vector<NumberKey> a, std::vector<NumberKey> b,
                                                 std::string& error);

    NumberCurveMode mode() const { return mode_; }
    float min() const { return min_; }
    float max() const { return max_; }
    const std::vector<NumberKey>& keys_a() const { return a_; }
    const std::vector<NumberKey>& keys_b() const { return b_; }

    // blend, 0..1, picks within a Range or between a CurveRange's curves.
    float evaluate(float t, float blend) const;

    bool operator==(const NumberCurve& other) const {
        return mode_ == other.mode_ && min_ == other.min_ && max_ == other.max_ && a_ == other.a_ && b_ == other.b_;
    }
    bool operator!=(const NumberCurve& other) const { return !(*this == other); }

private:
    NumberCurveMode mode_ = NumberCurveMode::Constant;
    float min_ = 0.f;
    float max_ = 0.f;
    std::vector<NumberKey> a_;
    std::vector<NumberKey> b_;
};

// The keys' value at t, holding the end values outside them. keys is
// non-empty and sorted by time.
float evaluate_keys(const std::vector<NumberKey>& keys, float t);

struct ColorKey {
    float time = 0.f;
    ColorRgb color{1.f, 1.f, 1.f, 1.f};   // a is ignored
    float transparency = 0.f;

    bool operator==(const ColorKey& other) const {
        return time == other.time && color.r == other.color.r && color.g == other.color.g &&
               color.b == other.color.b && transparency == other.transparency;
    }
};

enum class ColorCurveMode : std::uint8_t { Constant = 0, RandomBetween = 1, Gradient = 2, GradientRange = 3 };

// A color and transparency that is fixed, one of two picked per particle,
// keyed over 0..1, or a random blend of two gradients. evaluate's alpha is
// 1 - transparency. Channels interpolate linearly as stored.
class ColorCurve {
public:
    static ColorCurve constant(ColorRgb color, float transparency);
    static ColorCurve between(ColorRgb a, float transparency_a, ColorRgb b, float transparency_b);
    static std::optional<ColorCurve> gradient(std::vector<ColorKey> keys, std::string& error);
    static std::optional<ColorCurve> gradient_range(std::vector<ColorKey> a, std::vector<ColorKey> b,
                                                    std::string& error);

    ColorCurveMode mode() const { return mode_; }
    // Constant and RandomBetween keep their values as one key each.
    const std::vector<ColorKey>& keys_a() const { return a_; }
    const std::vector<ColorKey>& keys_b() const { return b_; }

    ColorRgb evaluate(float t, float blend) const;

    bool operator==(const ColorCurve& other) const {
        return mode_ == other.mode_ && a_ == other.a_ && b_ == other.b_;
    }
    bool operator!=(const ColorCurve& other) const { return !(*this == other); }

private:
    ColorCurveMode mode_ = ColorCurveMode::Constant;
    std::vector<ColorKey> a_;
    std::vector<ColorKey> b_;
};

ColorRgb evaluate_color_keys(const std::vector<ColorKey>& keys, float t);

// A NumberCurve read in O(1): 64 samples per curve, linear between them.
struct CurveTable {
    static constexpr int kSamples = 64;
    NumberCurveMode mode = NumberCurveMode::Constant;
    float min = 0.f;
    float max = 0.f;
    float a[kSamples]{};
    float b[kSamples]{};

    void bake(const NumberCurve& curve);
    float sample(float t, float blend) const;
};

// A ColorCurve read in O(1), as CurveTable.
struct ColorTable {
    static constexpr int kSamples = 64;
    ColorCurveMode mode = ColorCurveMode::Constant;
    ColorRgb a[kSamples]{};
    ColorRgb b[kSamples]{};

    void bake(const ColorCurve& curve);
    ColorRgb sample(float t, float blend) const;
};

}  // namespace engine_core
