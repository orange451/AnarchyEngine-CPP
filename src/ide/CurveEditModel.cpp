#include "CurveEditModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace ide {

using engine_core::ColorCurve;
using engine_core::ColorCurveMode;
using engine_core::ColorKey;
using engine_core::NumberCurve;
using engine_core::NumberCurveMode;
using engine_core::NumberKey;

namespace {

bool read_number(std::string_view text, float& out) {
    std::string s(text);
    const std::size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return false;
    }
    s = s.substr(start, s.find_last_not_of(" \t") - start + 1);
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) {
        return false;
    }
    out = v;
    return true;
}

}  // namespace

std::optional<NumberCurve> parse_curve_text(std::string_view text) {
    float one = 0.f;
    if (read_number(text, one)) {
        return NumberCurve::constant(one);
    }
    const std::size_t dots = text.find("..");
    if (dots != std::string_view::npos) {
        float lo = 0.f;
        float hi = 0.f;
        if (read_number(text.substr(0, dots), lo) && read_number(text.substr(dots + 2), hi)) {
            return NumberCurve::range(lo, hi);
        }
        return std::nullopt;
    }
    // "a - b": a minus that splits the text into two numbers.
    for (std::size_t i = 1; i < text.size(); ++i) {
        if (text[i] != '-') {
            continue;
        }
        float lo = 0.f;
        float hi = 0.f;
        if (read_number(text.substr(0, i), lo) && read_number(text.substr(i + 1), hi)) {
            return NumberCurve::range(lo, hi);
        }
    }
    return std::nullopt;
}

CurveEditModel::CurveEditModel(NumberCurve curve) { set_curve(std::move(curve)); }

void CurveEditModel::set_curve(NumberCurve curve) {
    curve_ = std::move(curve);
    a_ = curve_.keys_a();
    b_ = curve_.keys_b();
}

void CurveEditModel::rebuild() {
    std::string error;
    if (curve_.mode() == NumberCurveMode::Curve) {
        if (auto next = NumberCurve::keys(a_, error)) {
            curve_ = *next;
        }
    } else if (curve_.mode() == NumberCurveMode::CurveRange) {
        if (auto next = NumberCurve::keys_range(a_, b_, error)) {
            curve_ = *next;
        }
    }
    a_ = curve_.keys_a();
    b_ = curve_.keys_b();
}

void CurveEditModel::set_mode(NumberCurveMode mode) {
    if (mode == curve_.mode()) {
        return;
    }
    float low = curve_.min();
    float high = curve_.max();
    if (!a_.empty()) {
        low = high = a_.front().value;
        for (const NumberKey& key : a_) {
            low = std::min(low, key.value);
            high = std::max(high, key.value);
        }
    }
    std::string error;
    switch (mode) {
    case NumberCurveMode::Constant:
        curve_ = NumberCurve::constant(low);
        break;
    case NumberCurveMode::Range:
        curve_ = NumberCurve::range(low, high);
        break;
    case NumberCurveMode::Curve:
        if (a_.empty()) {
            a_ = {{0.f, low}, {1.f, high}};
        }
        curve_ = *NumberCurve::keys(a_, error);
        break;
    case NumberCurveMode::CurveRange:
        if (a_.empty()) {
            a_ = {{0.f, low}, {1.f, low}};
        }
        if (b_.empty()) {
            b_ = a_;
            for (NumberKey& key : b_) {
                key.value = high > low ? high : key.value + 1.f;
            }
        }
        curve_ = *NumberCurve::keys_range(a_, b_, error);
        break;
    }
    a_ = curve_.keys_a();
    b_ = curve_.keys_b();
}

void CurveEditModel::fit(float& low, float& high) const {
    low = std::min(0.f, curve_.min());
    high = std::max(1.f, curve_.max());
    for (const auto* list : {&a_, &b_}) {
        for (const NumberKey& key : *list) {
            low = std::min(low, key.value);
            high = std::max(high, key.value);
        }
    }
    const float margin = std::max(0.1f * (high - low), 0.05f);
    low -= margin;
    high += margin;
    if (high - low < 1.f) {
        high = low + 1.f;
    }
}

CurveEditModel::Hit CurveEditModel::hit(float t, float v, float radius_t, float radius_v) const {
    Hit best;
    float best_distance = 2.f;
    for (int which = 0; which < 2; ++which) {
        const std::vector<NumberKey>& list = which == 0 ? a_ : b_;
        for (std::size_t i = 0; i < list.size(); ++i) {
            const float dt = (list[i].time - t) / radius_t;
            const float dv = (list[i].value - v) / radius_v;
            const float d = dt * dt + dv * dv;
            if (d <= 1.f && d < best_distance) {
                best_distance = d;
                best.which = which;
                best.key = static_cast<int>(i);
            }
        }
    }
    return best;
}

int CurveEditModel::add_key(int which, float t, float v) {
    std::vector<NumberKey>& list = keys(which);
    const NumberKey key{std::clamp(t, 0.f, 1.f), v, engine_core::CurveInterp::Linear};
    const auto at = std::upper_bound(list.begin(), list.end(), key.time,
                                     [](float time, const NumberKey& k) { return time < k.time; });
    const int index = static_cast<int>(at - list.begin());
    list.insert(at, key);
    rebuild();
    return index;
}

int CurveEditModel::move_key(int which, int index, float t, float v) {
    std::vector<NumberKey>& list = keys(which);
    if (index < 0 || index >= static_cast<int>(list.size())) {
        return index;
    }
    NumberKey key = list[static_cast<std::size_t>(index)];
    key.time = std::clamp(t, 0.f, 1.f);
    key.value = v;
    list.erase(list.begin() + index);
    const auto at = std::upper_bound(list.begin(), list.end(), key.time,
                                     [](float time, const NumberKey& k) { return time < k.time; });
    const int moved = static_cast<int>(at - list.begin());
    list.insert(at, key);
    rebuild();
    return moved;
}

void CurveEditModel::delete_key(int which, int index) {
    std::vector<NumberKey>& list = keys(which);
    if (list.size() <= 1 || index < 0 || index >= static_cast<int>(list.size())) {
        return;
    }
    list.erase(list.begin() + index);
    rebuild();
}

void CurveEditModel::set_interp(int which, int index, engine_core::CurveInterp interp) {
    std::vector<NumberKey>& list = keys(which);
    if (index < 0 || index >= static_cast<int>(list.size())) {
        return;
    }
    list[static_cast<std::size_t>(index)].interp = interp;
    rebuild();
}

GradientEditModel::GradientEditModel(ColorCurve curve) { set_curve(std::move(curve)); }

void GradientEditModel::set_curve(ColorCurve curve) {
    curve_ = std::move(curve);
    a_ = curve_.keys_a();
    b_ = curve_.keys_b();
}

void GradientEditModel::rebuild() {
    std::string error;
    switch (curve_.mode()) {
    case ColorCurveMode::Constant:
        curve_ = ColorCurve::constant(a_.front().color, a_.front().transparency);
        break;
    case ColorCurveMode::RandomBetween:
        curve_ = ColorCurve::between(a_.front().color, a_.front().transparency, b_.front().color,
                                     b_.front().transparency);
        break;
    case ColorCurveMode::Gradient:
        if (auto next = ColorCurve::gradient(a_, error)) {
            curve_ = *next;
        }
        break;
    case ColorCurveMode::GradientRange:
        if (auto next = ColorCurve::gradient_range(a_, b_, error)) {
            curve_ = *next;
        }
        break;
    }
    a_ = curve_.keys_a();
    b_ = curve_.keys_b();
}

void GradientEditModel::set_mode(ColorCurveMode mode) {
    if (mode == curve_.mode()) {
        return;
    }
    const ColorKey first = a_.empty() ? ColorKey{} : a_.front();
    const ColorKey last = a_.empty() ? ColorKey{} : a_.back();
    std::string error;
    switch (mode) {
    case ColorCurveMode::Constant:
        curve_ = ColorCurve::constant(first.color, first.transparency);
        break;
    case ColorCurveMode::RandomBetween: {
        const ColorKey second = b_.empty() ? last : b_.front();
        curve_ = ColorCurve::between(first.color, first.transparency, second.color, second.transparency);
        break;
    }
    case ColorCurveMode::Gradient:
        if (a_.size() < 2) {
            a_ = {ColorKey{0.f, first.color, first.transparency}, ColorKey{1.f, first.color, first.transparency}};
        }
        curve_ = *ColorCurve::gradient(a_, error);
        break;
    case ColorCurveMode::GradientRange:
        if (a_.size() < 2) {
            a_ = {ColorKey{0.f, first.color, first.transparency}, ColorKey{1.f, first.color, first.transparency}};
        }
        if (b_.size() < 2) {
            b_ = a_;
        }
        curve_ = *ColorCurve::gradient_range(a_, b_, error);
        break;
    }
    a_ = curve_.keys_a();
    b_ = curve_.keys_b();
}

int GradientEditModel::hit(int which, float t, float radius) const {
    const std::vector<ColorKey>& list = which == 1 ? b_ : a_;
    int best = -1;
    float best_distance = radius;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const float d = std::fabs(list[i].time - t);
        if (d <= best_distance) {
            best_distance = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int GradientEditModel::add_stop(int which, float t) {
    std::vector<ColorKey>& list = keys(which);
    const float time = std::clamp(t, 0.f, 1.f);
    const engine_core::ColorRgb at =
        list.empty() ? engine_core::ColorRgb{} : engine_core::evaluate_color_keys(list, time);
    const ColorKey key{time, engine_core::ColorRgb{at.r, at.g, at.b, 1.f}, 1.f - at.a};
    const auto where = std::upper_bound(list.begin(), list.end(), time,
                                        [](float value, const ColorKey& k) { return value < k.time; });
    const int index = static_cast<int>(where - list.begin());
    list.insert(where, key);
    rebuild();
    return index;
}

int GradientEditModel::move_stop(int which, int index, float t) {
    std::vector<ColorKey>& list = keys(which);
    if (index < 0 || index >= static_cast<int>(list.size())) {
        return index;
    }
    ColorKey key = list[static_cast<std::size_t>(index)];
    key.time = std::clamp(t, 0.f, 1.f);
    list.erase(list.begin() + index);
    const auto where = std::upper_bound(list.begin(), list.end(), key.time,
                                        [](float value, const ColorKey& k) { return value < k.time; });
    const int moved = static_cast<int>(where - list.begin());
    list.insert(where, key);
    rebuild();
    return moved;
}

void GradientEditModel::delete_stop(int which, int index) {
    std::vector<ColorKey>& list = keys(which);
    if (list.size() <= 1 || index < 0 || index >= static_cast<int>(list.size())) {
        return;
    }
    list.erase(list.begin() + index);
    rebuild();
}

void GradientEditModel::set_color(int which, int index, engine_core::ColorRgb color) {
    std::vector<ColorKey>& list = keys(which);
    if (index < 0 || index >= static_cast<int>(list.size())) {
        return;
    }
    list[static_cast<std::size_t>(index)].color = engine_core::ColorRgb{color.r, color.g, color.b, 1.f};
    rebuild();
}

void GradientEditModel::set_transparency(int which, int index, float transparency) {
    std::vector<ColorKey>& list = keys(which);
    if (index < 0 || index >= static_cast<int>(list.size())) {
        return;
    }
    list[static_cast<std::size_t>(index)].transparency = std::clamp(transparency, 0.f, 1.f);
    rebuild();
}

}  // namespace ide
