#pragma once

#include "Curve.hpp"
#include "ParticleEmitter.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ide {

// What a curve row's field takes: "5" is a Constant, "1 - 3" (or "1..3") a
// Range. nullopt for anything else.
std::optional<engine_core::NumberCurve> parse_curve_text(std::string_view text);

// ParticleEmitter.Bursts as its field shows it: "0: 50, 1.5: 10 - 20", each
// "time: count" with count a number or two as 1 - 3. Empty is no bursts.
// nullopt for anything else, or a time or count below 0.
std::optional<std::vector<engine_core::ParticleBurst>> parse_bursts_text(std::string_view text);
std::string format_bursts_text(const std::vector<engine_core::ParticleBurst>& bursts);

// Editing a NumberCurve's keys, with nothing on screen: the curve editor's
// model. Graph space is time 0..1 across and value up. which is 0 for the
// first curve and 1 for a CurveRange's second.
class CurveEditModel {
public:
    struct Hit {
        int which = 0;
        int key = -1;   // -1: nothing there
    };

    explicit CurveEditModel(engine_core::NumberCurve curve);
    const engine_core::NumberCurve& curve() const { return curve_; }
    void set_curve(engine_core::NumberCurve curve);

    // Converts: a Constant keeps the first value, a Range spans the values, a
    // Curve keys the old value(s) across time, a CurveRange adds a second curve.
    void set_mode(engine_core::NumberCurveMode mode);
    // The value range every key fits in, with a margin, at least one unit tall.
    void fit(float& low, float& high) const;
    // The key within radius_t and radius_v of (t, v), nearest first, on either curve.
    Hit hit(float t, float v, float radius_t, float radius_v) const;
    // Returns the new key's index.
    int add_key(int which, float t, float v);
    // Time is kept within 0..1. Returns where the key is after the keys resort.
    int move_key(int which, int index, float t, float v);
    // Never removes a curve's last key.
    void delete_key(int which, int index);
    void set_interp(int which, int index, engine_core::CurveInterp interp);

private:
    std::vector<engine_core::NumberKey>& keys(int which) { return which == 1 ? b_ : a_; }
    void rebuild();

    engine_core::NumberCurve curve_;
    std::vector<engine_core::NumberKey> a_;
    std::vector<engine_core::NumberKey> b_;
};

// Editing a ColorCurve's stops, as CurveEditModel edits keys.
class GradientEditModel {
public:
    explicit GradientEditModel(engine_core::ColorCurve curve);
    const engine_core::ColorCurve& curve() const { return curve_; }
    void set_curve(engine_core::ColorCurve curve);

    // A Gradient starts from the old color at both ends; RandomBetween and
    // GradientRange add a second color or gradient.
    void set_mode(engine_core::ColorCurveMode mode);
    // The stop of gradient which within radius of t, or -1.
    int hit(int which, float t, float radius) const;
    // A stop at t colored as the gradient is there. Returns its index.
    int add_stop(int which, float t);
    int move_stop(int which, int index, float t);
    void delete_stop(int which, int index);
    void set_color(int which, int index, engine_core::ColorRgb color);
    void set_transparency(int which, int index, float transparency);

private:
    std::vector<engine_core::ColorKey>& keys(int which) { return which == 1 ? b_ : a_; }
    void rebuild();

    engine_core::ColorCurve curve_;
    std::vector<engine_core::ColorKey> a_;
    std::vector<engine_core::ColorKey> b_;
};

}  // namespace ide
