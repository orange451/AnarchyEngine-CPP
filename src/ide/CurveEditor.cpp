#include "CurveEditor.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ide {
namespace {

using engine_core::ColorCurveMode;
using engine_core::CurveInterp;
using engine_core::NumberCurveMode;
using engine_core::NumberKey;

constexpr const char* kFamily = "Open Sans";
constexpr double kTabHeight = 18;
constexpr double kGap = 4;
constexpr float kKeySize = 7.f;
constexpr const char* kModes[] = {"Constant", "Range", "Curve", "Curve range"};
constexpr const char* kInterps[] = {"Linear", "Smooth", "Step"};
constexpr const char* kColorModes[] = {"Constant", "Between", "Gradient", "Gradient range"};

jadefx::Color with_alpha(jadefx::Color c, float a) {
    c.a *= a;
    return c;
}

jadefx::Color accent(int which, float opacity) {
    return which == 0 ? jadefx::Color{0.35f, 0.65f, 1.f, opacity} : jadefx::Color{1.f, 0.6f, 0.25f, opacity};
}

// A row of equal tabs across [x, x + w): the selected one filled.
void draw_tabs(jadefx::Painter& painter, const char* const* names, int count, int selected, double x, double y,
               double w, jadefx::Color text) {
    const double each = w / count;
    for (int i = 0; i < count; ++i) {
        const float tx = static_cast<float>(x + i * each);
        if (i == selected) {
            painter.fillRect(tx + 1, static_cast<float>(y), static_cast<float>(each - 2), kTabHeight,
                             with_alpha(text, 0.18f));
        }
        painter.text(tx + 6, static_cast<float>(y + 2), names[i], kFamily, 11.f,
                     with_alpha(text, i == selected ? 1.f : 0.6f));
    }
}

int tab_at(double px, double x, double w, int count) {
    if (px < x || px >= x + w) {
        return -1;
    }
    return std::clamp(static_cast<int>((px - x) / (w / count)), 0, count - 1);
}

}  // namespace

CurveEditor::CurveEditor() : model_(engine_core::NumberCurve::constant(0.f)) {
    getClassList().add("curve-editor");
    setStyle("color: var(--ide-text-color);");
}

void CurveEditor::show(const engine_core::NumberCurve& curve) {
    if (dragging_) {
        return;
    }
    // After an edit, the row shows its old value until the edit lands: keep ours.
    if (sent_) {
        if (curve == *sent_) {
            sent_.reset();
        } else if (shown_ && curve == *shown_) {
            return;
        } else {
            sent_.reset();
        }
    }
    shown_ = curve;
    if (model_.curve() == curve) {
        return;
    }
    model_.set_curve(curve);
    if (!keyed()) {
        key_ = -1;
    }
    model_.fit(low_, high_);
}

bool CurveEditor::keyed() const {
    const auto& c = model_.curve();
    if (c.mode() != NumberCurveMode::Curve && c.mode() != NumberCurveMode::CurveRange) {
        return false;
    }
    const auto& keys = which_ == 1 ? c.keys_b() : c.keys_a();
    return key_ >= 0 && key_ < static_cast<int>(keys.size());
}

void CurveEditor::emit(bool done) {
    sent_ = model_.curve();
    if (on_edit) {
        on_edit(done);
    }
}

void CurveEditor::select(int which, int key) {
    if (which == which_ && key == key_) {
        return;
    }
    which_ = which;
    key_ = key;
    if (on_select) {
        on_select();
    }
}

CurveEditor::Rect CurveEditor::mode_tab(int mode) const {
    const double each = getWidth() / 4;
    return {getAbsoluteX() + mode * each, getAbsoluteY(), each, kTabHeight};
}

CurveEditor::Rect CurveEditor::interp_tab(int interp) const {
    const double w = std::min(getWidth(), 210.0);
    const double each = w / 3;
    return {getAbsoluteX() + interp * each, getAbsoluteY() + getHeight() - kTabHeight, each, kTabHeight};
}

CurveEditor::Rect CurveEditor::graph() const {
    const double top = getAbsoluteY() + kTabHeight + kGap;
    return {getAbsoluteX() + 8, top, std::max(1.0, getWidth() - 16),
            std::max(1.0, getHeight() - 2 * kTabHeight - 2 * kGap)};
}

double CurveEditor::x_of(float t) const {
    const Rect g = graph();
    return g.x + static_cast<double>(t) * g.w;
}

double CurveEditor::y_of(float v) const {
    const Rect g = graph();
    return g.y + g.h - (static_cast<double>(v) - low_) / std::max(1e-6f, high_ - low_) * g.h;
}

float CurveEditor::t_of(double x) const {
    const Rect g = graph();
    return static_cast<float>(std::clamp((x - g.x) / g.w, 0.0, 1.0));
}

float CurveEditor::v_of(double y) const {
    const Rect g = graph();
    return low_ + static_cast<float>((g.y + g.h - y) / g.h) * (high_ - low_);
}

void CurveEditor::handleMousePressed(const jadefx::MouseEvent& event) {
    const int tab = tab_at(event.x, getAbsoluteX(), getWidth(), 4);
    if (event.y < getAbsoluteY() + kTabHeight && tab >= 0) {
        model_.set_mode(static_cast<NumberCurveMode>(tab));
        model_.fit(low_, high_);
        emit(true);
        select(0, -1);
        return;
    }
    const auto mode = model_.curve().mode();
    const bool curves = mode == NumberCurveMode::Curve || mode == NumberCurveMode::CurveRange;
    if (!curves) {
        return;
    }
    if (event.y >= getAbsoluteY() + getHeight() - kTabHeight) {
        for (int i = 0; i < 3; ++i) {
            if (interp_tab(i).contains(event.x, event.y) && keyed()) {
                model_.set_interp(which_, key_, static_cast<CurveInterp>(i));
                emit(true);
            }
        }
        return;
    }
    const Rect g = graph();
    const float radius_t = static_cast<float>(kKeySize / g.w);
    const float radius_v = static_cast<float>(kKeySize / g.h) * (high_ - low_);
    const CurveEditModel::Hit hit = model_.hit(t_of(event.x), v_of(event.y), radius_t, radius_v);
    if (event.button == 1) {   // right: remove
        if (hit.key >= 0) {
            model_.delete_key(hit.which, hit.key);
            emit(true);
            select(0, -1);
        }
        return;
    }
    if (hit.key >= 0) {
        select(hit.which, hit.key);
        dragging_ = true;
        return;
    }
    if (event.clickCount >= 2 && g.contains(event.x, event.y)) {
        // On a CurveRange, the curve nearer the pointer takes it.
        int which = 0;
        if (mode == NumberCurveMode::CurveRange) {
            const float t = t_of(event.x);
            const float va = engine_core::evaluate_keys(model_.curve().keys_a(), t);
            const float vb = engine_core::evaluate_keys(model_.curve().keys_b(), t);
            which = std::fabs(vb - v_of(event.y)) < std::fabs(va - v_of(event.y)) ? 1 : 0;
        }
        const int key = model_.add_key(which, t_of(event.x), v_of(event.y));
        emit(true);
        select(which, key);
    }
}

void CurveEditor::handleMouseDragged(const jadefx::MouseEvent& event) {
    if (!dragging_ || !keyed()) {
        return;
    }
    const int moved = model_.move_key(which_, key_, t_of(event.x), v_of(event.y));
    key_ = moved;
    emit(false);
}

void CurveEditor::handleMouseReleased(const jadefx::MouseEvent&) {
    if (!dragging_) {
        return;
    }
    dragging_ = false;
    model_.fit(low_, high_);
    if (on_select) {
        on_select();
    }
    emit(true);
}

void CurveEditor::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    if (getWidth() <= 1 || getHeight() <= 1) {
        return;
    }
    jadefx::Painter painter(renderer);
    const jadefx::Color text = with_alpha(computedStyle().color, opacity);
    const auto& curve = model_.curve();
    draw_tabs(painter, kModes, 4, static_cast<int>(curve.mode()), getAbsoluteX(), getAbsoluteY(), getWidth(), text);
    const Rect g = graph();
    painter.fillRect(static_cast<float>(g.x), static_cast<float>(g.y), static_cast<float>(g.w),
                     static_cast<float>(g.h), with_alpha(text, 0.06f));
    // Quarter lines across time, and the zero line when it is in view.
    for (int i = 1; i < 4; ++i) {
        painter.fillRect(static_cast<float>(g.x + g.w * i / 4), static_cast<float>(g.y), 1.f,
                         static_cast<float>(g.h), with_alpha(text, 0.08f));
    }
    if (low_ < 0.f && high_ > 0.f) {
        painter.fillRect(static_cast<float>(g.x), static_cast<float>(y_of(0.f)), static_cast<float>(g.w), 1.f,
                         with_alpha(text, 0.2f));
    }
    painter.pushClip(static_cast<float>(g.x), static_cast<float>(g.y), static_cast<float>(g.w),
                     static_cast<float>(g.h));
    const auto stroke = [&](auto value_at, jadefx::Color color) {
        double last = y_of(value_at(0.f));
        for (double x = 0; x <= g.w; x += 1.0) {
            const double y = y_of(value_at(static_cast<float>(x / g.w)));
            const double top = std::min(last, y) - 0.75;
            painter.fillRect(static_cast<float>(g.x + x), static_cast<float>(top), 1.5f,
                             static_cast<float>(std::fabs(y - last) + 1.5), color);
            last = y;
        }
    };
    switch (curve.mode()) {
    case NumberCurveMode::Constant:
        stroke([&](float) { return curve.min(); }, accent(0, opacity));
        break;
    case NumberCurveMode::Range:
        stroke([&](float) { return curve.min(); }, accent(0, opacity));
        stroke([&](float) { return curve.max(); }, accent(1, opacity));
        break;
    case NumberCurveMode::Curve:
    case NumberCurveMode::CurveRange:
        for (int which = 0; which < (curve.mode() == NumberCurveMode::CurveRange ? 2 : 1); ++which) {
            const auto& keys = which == 1 ? curve.keys_b() : curve.keys_a();
            stroke([&](float t) { return engine_core::evaluate_keys(keys, t); }, accent(which, opacity));
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const bool picked = which == which_ && static_cast<int>(i) == key_;
                const float half = kKeySize * 0.5f;
                painter.fillRect(static_cast<float>(x_of(keys[i].time)) - half,
                                 static_cast<float>(y_of(keys[i].value)) - half, kKeySize, kKeySize,
                                 picked ? text : accent(which, opacity));
            }
        }
        break;
    }
    painter.popClip();
    painter.text(static_cast<float>(g.x + 3), static_cast<float>(g.y + 1), std::to_string(high_).substr(0, 5), kFamily,
                 9.f, with_alpha(text, 0.5f));
    painter.text(static_cast<float>(g.x + 3), static_cast<float>(g.y + g.h - 12), std::to_string(low_).substr(0, 5),
                 kFamily, 9.f, with_alpha(text, 0.5f));
    const double bottom = getAbsoluteY() + getHeight() - kTabHeight;
    if (keyed()) {
        const auto& keys = which_ == 1 ? curve.keys_b() : curve.keys_a();
        draw_tabs(painter, kInterps, 3, static_cast<int>(keys[static_cast<std::size_t>(key_)].interp), getAbsoluteX(),
                  bottom, std::min(getWidth(), 210.0), text);
    } else if (curve.mode() == NumberCurveMode::Curve || curve.mode() == NumberCurveMode::CurveRange) {
        painter.text(static_cast<float>(getAbsoluteX() + 6), static_cast<float>(bottom + 2),
                     "Double-click to add a key, right-click to remove one", kFamily, 11.f, with_alpha(text, 0.6f));
    } else {
        painter.text(static_cast<float>(getAbsoluteX() + 6), static_cast<float>(bottom + 2),
                     "Type a number, or two as 1 - 3, in the field above", kFamily, 11.f, with_alpha(text, 0.6f));
    }
}

GradientEditor::GradientEditor() : model_(engine_core::ColorCurve::constant(engine_core::ColorRgb{}, 0.f)) {
    getClassList().add("gradient-editor");
    setStyle("color: var(--ide-text-color);");
}

void GradientEditor::show(const engine_core::ColorCurve& curve) {
    if (dragging_) {
        return;
    }
    if (sent_) {
        if (curve == *sent_) {
            sent_.reset();
        } else if (shown_ && curve == *shown_) {
            return;
        } else {
            sent_.reset();
        }
    }
    shown_ = curve;
    if (model_.curve() == curve) {
        return;
    }
    model_.set_curve(curve);
    const auto& keys = which_ == 1 ? curve.keys_b() : curve.keys_a();
    if (stop_ >= static_cast<int>(keys.size())) {
        stop_ = 0;
        which_ = 0;
    }
}

void GradientEditor::emit(bool done) {
    sent_ = model_.curve();
    if (on_edit) {
        on_edit(done);
    }
}

void GradientEditor::select(int which, int stop) {
    if (which == which_ && stop == stop_) {
        return;
    }
    which_ = which;
    stop_ = stop;
    if (on_select) {
        on_select();
    }
}

int GradientEditor::bars() const {
    const auto mode = model_.curve().mode();
    return mode == ColorCurveMode::RandomBetween || mode == ColorCurveMode::GradientRange ? 2 : 1;
}

GradientEditor::Rect GradientEditor::mode_tab(int mode) const {
    const double each = getWidth() / 4;
    return {getAbsoluteX() + mode * each, getAbsoluteY(), each, kTabHeight};
}

GradientEditor::Rect GradientEditor::bar(int which) const {
    const double top = getAbsoluteY() + kTabHeight + kGap + which * 27.0;
    return {getAbsoluteX() + 8, top, std::max(1.0, getWidth() - 16), 18};
}

void GradientEditor::handleMousePressed(const jadefx::MouseEvent& event) {
    const int tab = tab_at(event.x, getAbsoluteX(), getWidth(), 4);
    if (event.y < getAbsoluteY() + kTabHeight && tab >= 0) {
        model_.set_mode(static_cast<ColorCurveMode>(tab));
        emit(true);
        select(0, 0);
        return;
    }
    for (int which = 0; which < bars(); ++which) {
        const Rect b = bar(which);
        if (event.y < b.y || event.y >= b.y + b.h + 8) {
            continue;
        }
        const float t = static_cast<float>(std::clamp((event.x - b.x) / b.w, 0.0, 1.0));
        const bool keyed = model_.curve().mode() == ColorCurveMode::Gradient ||
                           model_.curve().mode() == ColorCurveMode::GradientRange;
        if (!keyed) {
            select(which, 0);
            return;
        }
        const int hit = model_.hit(which, t, static_cast<float>(6.0 / b.w));
        if (event.button == 1) {
            if (hit >= 0) {
                model_.delete_stop(which, hit);
                emit(true);
                select(which, 0);
            }
            return;
        }
        if (hit >= 0) {
            select(which, hit);
            dragging_ = true;
            return;
        }
        if (event.clickCount >= 2) {
            const int stop = model_.add_stop(which, t);
            emit(true);
            select(which, stop);
        }
        return;
    }
}

void GradientEditor::handleMouseDragged(const jadefx::MouseEvent& event) {
    if (!dragging_) {
        return;
    }
    const Rect b = bar(which_);
    stop_ = model_.move_stop(which_, stop_, static_cast<float>(std::clamp((event.x - b.x) / b.w, 0.0, 1.0)));
    emit(false);
}

void GradientEditor::handleMouseReleased(const jadefx::MouseEvent&) {
    if (!dragging_) {
        return;
    }
    dragging_ = false;
    emit(true);
}

void GradientEditor::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    if (getWidth() <= 1 || getHeight() <= 1) {
        return;
    }
    jadefx::Painter painter(renderer);
    const jadefx::Color text = with_alpha(computedStyle().color, opacity);
    const auto& curve = model_.curve();
    draw_tabs(painter, kColorModes, 4, static_cast<int>(curve.mode()), getAbsoluteX(), getAbsoluteY(), getWidth(),
              text);
    for (int which = 0; which < bars(); ++which) {
        const Rect b = bar(which);
        const auto& keys = which == 1 ? curve.keys_b() : curve.keys_a();
        // A checkerboard under the color, so transparency shows.
        for (double x = 0; x < b.w; x += 6) {
            for (int row = 0; row < 3; ++row) {
                const bool dark = (static_cast<int>(x / 6) + row) % 2 == 0;
                painter.fillRect(static_cast<float>(b.x + x), static_cast<float>(b.y + row * 6),
                                 static_cast<float>(std::min(6.0, b.w - x)), 6.f,
                                 dark ? jadefx::Color{0.55f, 0.55f, 0.55f, opacity}
                                      : jadefx::Color{0.8f, 0.8f, 0.8f, opacity});
            }
        }
        for (double x = 0; x < b.w; x += 1.0) {
            const engine_core::ColorRgb c = engine_core::evaluate_color_keys(keys, static_cast<float>(x / b.w));
            painter.fillRect(static_cast<float>(b.x + x), static_cast<float>(b.y), 1.f, static_cast<float>(b.h),
                             jadefx::Color{c.r, c.g, c.b, c.a * opacity});
        }
        const bool keyed = curve.mode() == ColorCurveMode::Gradient || curve.mode() == ColorCurveMode::GradientRange;
        for (std::size_t i = 0; keyed && i < keys.size(); ++i) {
            const bool picked = which == which_ && static_cast<int>(i) == stop_;
            const float x = static_cast<float>(b.x + keys[i].time * b.w);
            painter.fillRect(x - 3.f, static_cast<float>(b.y + b.h + 1), 6.f, 6.f,
                             picked ? text : with_alpha(text, 0.5f));
        }
        if (!keyed && which == which_) {
            painter.fillRect(static_cast<float>(b.x - 6), static_cast<float>(b.y + 6), 4.f, 6.f, text);
        }
    }
}

}  // namespace ide
