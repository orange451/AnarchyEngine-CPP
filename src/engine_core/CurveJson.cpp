#include "CurveJson.hpp"

#include <cmath>
#include <limits>

namespace engine_core {
namespace {

JsonValue number(float value) { return JsonValue::number_from_float(value); }

// A finite number that fits a float, in out. False otherwise.
bool read_float(const JsonValue* value, float& out) {
    if (value == nullptr || !value->is_number()) {
        return false;
    }
    const double v = value->as_number();
    if (!std::isfinite(v) || std::fabs(v) > std::numeric_limits<float>::max()) {
        return false;
    }
    out = static_cast<float>(v);
    return true;
}

JsonValue rgb(const ColorRgb& color) {
    const float channels[3] = {color.r, color.g, color.b};
    return json_floats(channels, 3);
}

bool read_rgb(const JsonValue* value, ColorRgb& out) {
    std::vector<float> channels;
    if (value == nullptr || !read_json_floats(*value, 3, 3, channels)) {
        return false;
    }
    out = ColorRgb{channels[0], channels[1], channels[2], 1.f};
    return true;
}

JsonValue number_keys(const std::vector<NumberKey>& keys) {
    std::vector<JsonValue> items;
    items.reserve(keys.size());
    for (const NumberKey& key : keys) {
        items.push_back(JsonValue::array({number(key.time), number(key.value), JsonValue::string(curve_interp_name(key.interp))}));
    }
    return JsonValue::array(std::move(items));
}

bool read_number_keys(const JsonValue* value, std::vector<NumberKey>& out, std::string& error) {
    if (value == nullptr || !value->is_array()) {
        error = "has a malformed key list";
        return false;
    }
    out.clear();
    for (const JsonValue& item : value->items()) {
        NumberKey key;
        const auto& parts = item.items();
        if (!item.is_array() || parts.size() < 2 || parts.size() > 3 || !read_float(&parts[0], key.time) ||
            !read_float(&parts[1], key.value)) {
            error = "has a malformed key";
            return false;
        }
        if (parts.size() == 3) {
            const auto interp = parts[2].is_string() ? curve_interp_from_name(parts[2].as_string()) : std::nullopt;
            if (!interp) {
                error = "has an unknown interpolation";
                return false;
            }
            key.interp = *interp;
        }
        out.push_back(key);
    }
    return true;
}

JsonValue color_keys(const std::vector<ColorKey>& keys) {
    std::vector<JsonValue> items;
    items.reserve(keys.size());
    for (const ColorKey& key : keys) {
        items.push_back(JsonValue::array({number(key.time), rgb(key.color), number(key.transparency)}));
    }
    return JsonValue::array(std::move(items));
}

bool read_color_keys(const JsonValue* value, std::vector<ColorKey>& out, std::string& error) {
    if (value == nullptr || !value->is_array()) {
        error = "has a malformed key list";
        return false;
    }
    out.clear();
    for (const JsonValue& item : value->items()) {
        ColorKey key;
        const auto& parts = item.items();
        if (!item.is_array() || parts.size() < 2 || parts.size() > 3 || !read_float(&parts[0], key.time) ||
            !read_rgb(&parts[1], key.color) || (parts.size() == 3 && !read_float(&parts[2], key.transparency))) {
            error = "has a malformed key";
            return false;
        }
        out.push_back(key);
    }
    return true;
}

const std::string* mode_of(const JsonValue& value, std::string& error) {
    if (!value.is_object()) {
        error = "must be an object with a mode";
        return nullptr;
    }
    const JsonValue* mode = value.find("mode");
    if (mode == nullptr || !mode->is_string()) {
        error = "has an unknown mode";
        return nullptr;
    }
    return &mode->as_string();
}

}  // namespace

LuaSlot number_curve_slot(NumberCurve curve) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::NumberCurve;
    slot.number_curve = std::make_shared<const NumberCurve>(std::move(curve));
    return slot;
}

LuaSlot color_curve_slot(ColorCurve curve) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::ColorCurve;
    slot.color_curve = std::make_shared<const ColorCurve>(std::move(curve));
    return slot;
}

JsonValue number_curve_to_json(const NumberCurve& curve) {
    JsonValue out = JsonValue::object();
    switch (curve.mode()) {
    case NumberCurveMode::Constant:
        out.set("mode", JsonValue::string("Constant"));
        out.set("value", number(curve.min()));
        break;
    case NumberCurveMode::Range:
        out.set("mode", JsonValue::string("Range"));
        out.set("min", number(curve.min()));
        out.set("max", number(curve.max()));
        break;
    case NumberCurveMode::Curve:
        out.set("mode", JsonValue::string("Curve"));
        out.set("keys", number_keys(curve.keys_a()));
        break;
    case NumberCurveMode::CurveRange:
        out.set("mode", JsonValue::string("CurveRange"));
        out.set("keys", number_keys(curve.keys_a()));
        out.set("keys2", number_keys(curve.keys_b()));
        break;
    }
    return out;
}

std::optional<NumberCurve> number_curve_from_json(const JsonValue& value, std::string& error) {
    const std::string* mode = mode_of(value, error);
    if (mode == nullptr) {
        return std::nullopt;
    }
    if (*mode == "Constant") {
        float v = 0.f;
        if (!read_float(value.find("value"), v)) {
            error = "needs a finite value";
            return std::nullopt;
        }
        return NumberCurve::constant(v);
    }
    if (*mode == "Range") {
        float lo = 0.f;
        float hi = 0.f;
        if (!read_float(value.find("min"), lo) || !read_float(value.find("max"), hi)) {
            error = "needs a finite min and max";
            return std::nullopt;
        }
        return NumberCurve::range(lo, hi);
    }
    std::vector<NumberKey> a;
    std::vector<NumberKey> b;
    if (*mode == "Curve") {
        if (!read_number_keys(value.find("keys"), a, error)) {
            return std::nullopt;
        }
        return NumberCurve::keys(std::move(a), error);
    }
    if (*mode == "CurveRange") {
        if (!read_number_keys(value.find("keys"), a, error) || !read_number_keys(value.find("keys2"), b, error)) {
            return std::nullopt;
        }
        return NumberCurve::keys_range(std::move(a), std::move(b), error);
    }
    error = "has an unknown mode";
    return std::nullopt;
}

JsonValue color_curve_to_json(const ColorCurve& curve) {
    JsonValue out = JsonValue::object();
    switch (curve.mode()) {
    case ColorCurveMode::Constant:
        out.set("mode", JsonValue::string("Constant"));
        out.set("color", rgb(curve.keys_a()[0].color));
        out.set("transparency", number(curve.keys_a()[0].transparency));
        break;
    case ColorCurveMode::RandomBetween:
        out.set("mode", JsonValue::string("RandomBetween"));
        out.set("color", rgb(curve.keys_a()[0].color));
        out.set("transparency", number(curve.keys_a()[0].transparency));
        out.set("color2", rgb(curve.keys_b()[0].color));
        out.set("transparency2", number(curve.keys_b()[0].transparency));
        break;
    case ColorCurveMode::Gradient:
        out.set("mode", JsonValue::string("Gradient"));
        out.set("keys", color_keys(curve.keys_a()));
        break;
    case ColorCurveMode::GradientRange:
        out.set("mode", JsonValue::string("GradientRange"));
        out.set("keys", color_keys(curve.keys_a()));
        out.set("keys2", color_keys(curve.keys_b()));
        break;
    }
    return out;
}

std::optional<ColorCurve> color_curve_from_json(const JsonValue& value, std::string& error) {
    const std::string* mode = mode_of(value, error);
    if (mode == nullptr) {
        return std::nullopt;
    }
    if (*mode == "Constant" || *mode == "RandomBetween") {
        ColorRgb a;
        float ta = 0.f;
        if (!read_rgb(value.find("color"), a) || !read_float(value.find("transparency"), ta)) {
            error = "needs a color and a transparency";
            return std::nullopt;
        }
        if (*mode == "Constant") {
            return ColorCurve::constant(a, ta);
        }
        ColorRgb b;
        float tb = 0.f;
        if (!read_rgb(value.find("color2"), b) || !read_float(value.find("transparency2"), tb)) {
            error = "needs a color2 and a transparency2";
            return std::nullopt;
        }
        return ColorCurve::between(a, ta, b, tb);
    }
    std::vector<ColorKey> a;
    std::vector<ColorKey> b;
    if (*mode == "Gradient") {
        if (!read_color_keys(value.find("keys"), a, error)) {
            return std::nullopt;
        }
        return ColorCurve::gradient(std::move(a), error);
    }
    if (*mode == "GradientRange") {
        if (!read_color_keys(value.find("keys"), a, error) || !read_color_keys(value.find("keys2"), b, error)) {
            return std::nullopt;
        }
        return ColorCurve::gradient_range(std::move(a), std::move(b), error);
    }
    error = "has an unknown mode";
    return std::nullopt;
}

}  // namespace engine_core
