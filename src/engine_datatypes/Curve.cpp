#include "Curve.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core {
namespace {

float clamp01(float t) { return std::clamp(t, 0.f, 1.f); }

float mix(float a, float b, float f) { return a + (b - a) * f; }

ColorRgb mix(const ColorRgb& a, const ColorRgb& b, float f) {
    return ColorRgb{mix(a.r, b.r, f), mix(a.g, b.g, f), mix(a.b, b.b, f), mix(a.a, b.a, f)};
}

bool check_number_keys(std::vector<NumberKey>& keys, std::string& error) {
    if (keys.empty()) {
        error = "a curve needs at least one key";
        return false;
    }
    for (const NumberKey& key : keys) {
        if (!std::isfinite(key.time) || !std::isfinite(key.value)) {
            error = "a key's time and value must be finite";
            return false;
        }
        if (key.time < 0.f || key.time > 1.f) {
            error = "a key's time must be between 0 and 1";
            return false;
        }
    }
    std::stable_sort(keys.begin(), keys.end(),
                     [](const NumberKey& x, const NumberKey& y) { return x.time < y.time; });
    return true;
}

bool check_color_keys(std::vector<ColorKey>& keys, std::string& error) {
    if (keys.empty()) {
        error = "a gradient needs at least one key";
        return false;
    }
    for (const ColorKey& key : keys) {
        const float parts[] = {key.time, key.color.r, key.color.g, key.color.b, key.transparency};
        for (float part : parts) {
            if (!std::isfinite(part)) {
                error = "a key's time, color, and transparency must be finite";
                return false;
            }
        }
        if (key.time < 0.f || key.time > 1.f) {
            error = "a key's time must be between 0 and 1";
            return false;
        }
    }
    std::stable_sort(keys.begin(), keys.end(),
                     [](const ColorKey& x, const ColorKey& y) { return x.time < y.time; });
    return true;
}

// The slope a Smooth key passes through: the chord between its neighbours.
float tangent(const std::vector<NumberKey>& keys, std::size_t i) {
    const std::size_t last = keys.size() - 1;
    const std::size_t lo = i == 0 ? 0 : i - 1;
    const std::size_t hi = i == last ? last : i + 1;
    const float dt = keys[hi].time - keys[lo].time;
    return dt > 0.f ? (keys[hi].value - keys[lo].value) / dt : 0.f;
}

ColorRgb as_rgba(const ColorKey& key) { return ColorRgb{key.color.r, key.color.g, key.color.b, 1.f - key.transparency}; }

}  // namespace

std::optional<CurveInterp> curve_interp_from_name(std::string_view name) {
    if (name == "Linear") {
        return CurveInterp::Linear;
    }
    if (name == "Smooth") {
        return CurveInterp::Smooth;
    }
    if (name == "Step") {
        return CurveInterp::Step;
    }
    return std::nullopt;
}

const char* curve_interp_name(CurveInterp interp) {
    switch (interp) {
    case CurveInterp::Linear:
        return "Linear";
    case CurveInterp::Smooth:
        return "Smooth";
    case CurveInterp::Step:
        return "Step";
    }
    return "Linear";
}

float evaluate_keys(const std::vector<NumberKey>& keys, float t) {
    if (t <= keys.front().time) {
        return keys.front().value;
    }
    if (t >= keys.back().time) {
        return keys.back().value;
    }
    const auto next = std::upper_bound(keys.begin(), keys.end(), t,
                                       [](float value, const NumberKey& key) { return value < key.time; });
    const std::size_t i = static_cast<std::size_t>(next - keys.begin()) - 1;
    const NumberKey& k0 = keys[i];
    const NumberKey& k1 = keys[i + 1];
    const float span = k1.time - k0.time;
    if (span <= 0.f || k0.interp == CurveInterp::Step) {
        return k0.value;
    }
    const float f = (t - k0.time) / span;
    if (k0.interp == CurveInterp::Linear) {
        return mix(k0.value, k1.value, f);
    }
    // Cubic Hermite, the tangents scaled to the segment.
    const float m0 = tangent(keys, i) * span;
    const float m1 = tangent(keys, i + 1) * span;
    const float f2 = f * f;
    const float f3 = f2 * f;
    return (2 * f3 - 3 * f2 + 1) * k0.value + (f3 - 2 * f2 + f) * m0 + (-2 * f3 + 3 * f2) * k1.value +
           (f3 - f2) * m1;
}

ColorRgb evaluate_color_keys(const std::vector<ColorKey>& keys, float t) {
    if (t <= keys.front().time) {
        return as_rgba(keys.front());
    }
    if (t >= keys.back().time) {
        return as_rgba(keys.back());
    }
    const auto next = std::upper_bound(keys.begin(), keys.end(), t,
                                       [](float value, const ColorKey& key) { return value < key.time; });
    const std::size_t i = static_cast<std::size_t>(next - keys.begin()) - 1;
    const float span = keys[i + 1].time - keys[i].time;
    const float f = span > 0.f ? (t - keys[i].time) / span : 0.f;
    return mix(as_rgba(keys[i]), as_rgba(keys[i + 1]), f);
}

NumberCurve NumberCurve::constant(float value) {
    NumberCurve out;
    out.mode_ = NumberCurveMode::Constant;
    out.min_ = value;
    out.max_ = value;
    return out;
}

NumberCurve NumberCurve::range(float min, float max) {
    NumberCurve out;
    out.mode_ = NumberCurveMode::Range;
    out.min_ = min;
    out.max_ = max;
    return out;
}

std::optional<NumberCurve> NumberCurve::keys(std::vector<NumberKey> keys, std::string& error) {
    if (!check_number_keys(keys, error)) {
        return std::nullopt;
    }
    NumberCurve out;
    out.mode_ = NumberCurveMode::Curve;
    out.a_ = std::move(keys);
    return out;
}

std::optional<NumberCurve> NumberCurve::keys_range(std::vector<NumberKey> a, std::vector<NumberKey> b,
                                                   std::string& error) {
    if (!check_number_keys(a, error) || !check_number_keys(b, error)) {
        return std::nullopt;
    }
    NumberCurve out;
    out.mode_ = NumberCurveMode::CurveRange;
    out.a_ = std::move(a);
    out.b_ = std::move(b);
    return out;
}

float NumberCurve::evaluate(float t, float blend) const {
    switch (mode_) {
    case NumberCurveMode::Constant:
        return min_;
    case NumberCurveMode::Range:
        return mix(min_, max_, clamp01(blend));
    case NumberCurveMode::Curve:
        return evaluate_keys(a_, t);
    case NumberCurveMode::CurveRange:
        return mix(evaluate_keys(a_, t), evaluate_keys(b_, t), clamp01(blend));
    }
    return min_;
}

ColorCurve ColorCurve::constant(ColorRgb color, float transparency) {
    ColorCurve out;
    out.mode_ = ColorCurveMode::Constant;
    out.a_ = {ColorKey{0.f, color, transparency}};
    return out;
}

ColorCurve ColorCurve::between(ColorRgb a, float transparency_a, ColorRgb b, float transparency_b) {
    ColorCurve out;
    out.mode_ = ColorCurveMode::RandomBetween;
    out.a_ = {ColorKey{0.f, a, transparency_a}};
    out.b_ = {ColorKey{0.f, b, transparency_b}};
    return out;
}

std::optional<ColorCurve> ColorCurve::gradient(std::vector<ColorKey> keys, std::string& error) {
    if (!check_color_keys(keys, error)) {
        return std::nullopt;
    }
    ColorCurve out;
    out.mode_ = ColorCurveMode::Gradient;
    out.a_ = std::move(keys);
    return out;
}

std::optional<ColorCurve> ColorCurve::gradient_range(std::vector<ColorKey> a, std::vector<ColorKey> b,
                                                     std::string& error) {
    if (!check_color_keys(a, error) || !check_color_keys(b, error)) {
        return std::nullopt;
    }
    ColorCurve out;
    out.mode_ = ColorCurveMode::GradientRange;
    out.a_ = std::move(a);
    out.b_ = std::move(b);
    return out;
}

ColorRgb ColorCurve::evaluate(float t, float blend) const {
    switch (mode_) {
    case ColorCurveMode::Constant:
    case ColorCurveMode::Gradient:
        return evaluate_color_keys(a_, t);
    case ColorCurveMode::RandomBetween:
    case ColorCurveMode::GradientRange:
        return mix(evaluate_color_keys(a_, t), evaluate_color_keys(b_, t), clamp01(blend));
    }
    return ColorRgb{};
}

void CurveTable::bake(const NumberCurve& curve) {
    mode = curve.mode();
    min = curve.min();
    max = curve.max();
    if (mode != NumberCurveMode::Curve && mode != NumberCurveMode::CurveRange) {
        return;
    }
    for (int i = 0; i < kSamples; ++i) {
        const float t = static_cast<float>(i) / (kSamples - 1);
        a[i] = evaluate_keys(curve.keys_a(), t);
        b[i] = mode == NumberCurveMode::CurveRange ? evaluate_keys(curve.keys_b(), t) : a[i];
    }
}

float CurveTable::sample(float t, float blend) const {
    if (mode == NumberCurveMode::Constant) {
        return min;
    }
    if (mode == NumberCurveMode::Range) {
        return mix(min, max, clamp01(blend));
    }
    const float x = clamp01(t) * (kSamples - 1);
    const int i = std::min(static_cast<int>(x), kSamples - 2);
    const float f = x - static_cast<float>(i);
    const float va = mix(a[i], a[i + 1], f);
    if (mode == NumberCurveMode::Curve) {
        return va;
    }
    return mix(va, mix(b[i], b[i + 1], f), clamp01(blend));
}

void ColorTable::bake(const ColorCurve& curve) {
    mode = curve.mode();
    for (int i = 0; i < kSamples; ++i) {
        const float t = static_cast<float>(i) / (kSamples - 1);
        a[i] = evaluate_color_keys(curve.keys_a(), t);
        b[i] = curve.keys_b().empty() ? a[i] : evaluate_color_keys(curve.keys_b(), t);
    }
}

ColorRgb ColorTable::sample(float t, float blend) const {
    const float x = clamp01(t) * (kSamples - 1);
    const int i = std::min(static_cast<int>(x), kSamples - 2);
    const float f = x - static_cast<float>(i);
    const ColorRgb ca = mix(a[i], a[i + 1], f);
    if (mode == ColorCurveMode::Constant || mode == ColorCurveMode::Gradient) {
        return ca;
    }
    return mix(ca, mix(b[i], b[i + 1], f), clamp01(blend));
}

}  // namespace engine_core
