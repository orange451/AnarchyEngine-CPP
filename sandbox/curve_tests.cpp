#include "Curve.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace engine_core;
using Catch::Approx;

TEST_CASE("CRV1 a constant and a range evaluate as they say", "[curve]") {
    const NumberCurve five = NumberCurve::constant(5.f);
    REQUIRE(five.mode() == NumberCurveMode::Constant);
    REQUIRE(five.evaluate(0.3f, 0.9f) == 5.f);
    const NumberCurve range = NumberCurve::range(1.f, 3.f);
    REQUIRE(range.evaluate(0.7f, 0.f) == 1.f);
    REQUIRE(range.evaluate(0.7f, 1.f) == 3.f);
    REQUIRE(range.evaluate(0.7f, 0.5f) == Approx(2.f));
}

TEST_CASE("CRV2 keys sort by time, hold their ends, and interpolate per segment", "[curve]") {
    std::string error;
    const auto curve = NumberCurve::keys({{1.f, 0.f}, {0.f, 0.f}, {0.5f, 4.f, CurveInterp::Step}}, error);
    REQUIRE(curve.has_value());
    REQUIRE(curve->keys_a()[0].time == 0.f);
    REQUIRE(curve->evaluate(0.25f, 0.f) == Approx(2.f));
    REQUIRE(curve->evaluate(0.75f, 0.f) == 4.f);
    REQUIRE(curve->evaluate(1.f, 0.f) == 0.f);
    REQUIRE(curve->evaluate(-1.f, 0.f) == 0.f);
    REQUIRE(curve->evaluate(2.f, 0.f) == 0.f);
}

TEST_CASE("CRV3 smooth keys pass through their values and stay between them on a monotone run", "[curve]") {
    std::string error;
    const auto curve =
        NumberCurve::keys({{0.f, 0.f, CurveInterp::Smooth}, {0.5f, 1.f, CurveInterp::Smooth}, {1.f, 2.f}}, error);
    REQUIRE(curve.has_value());
    REQUIRE(curve->evaluate(0.5f, 0.f) == Approx(1.f));
    std::string why;
    const auto bend = NumberCurve::keys({{0.f, 0.f, CurveInterp::Smooth}, {0.5f, 1.f, CurveInterp::Smooth}, {1.f, 1.f}}, why);
    REQUIRE(bend->evaluate(0.25f, 0.f) == Approx(0.5625f));   // not the straight line's 0.5
    for (float t = 0.f; t <= 1.f; t += 0.05f) {
        const float v = curve->evaluate(t, 0.f);
        REQUIRE(v >= -0.001f);
        REQUIRE(v <= 2.001f);
    }
}

TEST_CASE("CRV4 a curve range blends its two curves by the particle's random", "[curve]") {
    std::string error;
    const auto curve = NumberCurve::keys_range({{0.f, 0.f}}, {{0.f, 10.f}}, error);
    REQUIRE(curve.has_value());
    REQUIRE(curve->evaluate(0.4f, 0.25f) == Approx(2.5f));
}

TEST_CASE("CRV5 bad keys are refused with a reason", "[curve]") {
    std::string error;
    REQUIRE_FALSE(NumberCurve::keys({}, error).has_value());
    REQUIRE(error.find("at least one key") != std::string::npos);
    REQUIRE_FALSE(NumberCurve::keys({{1.5f, 0.f}}, error).has_value());
    REQUIRE(error.find("between 0 and 1") != std::string::npos);
    REQUIRE_FALSE(NumberCurve::keys({{0.f, std::nanf("")}}, error).has_value());
    REQUIRE(error.find("finite") != std::string::npos);
}

TEST_CASE("CRV6 a baked table matches direct evaluation", "[curve]") {
    std::string error;
    const auto curve = NumberCurve::keys({{0.f, 0.f, CurveInterp::Smooth}, {0.3f, 5.f}, {1.f, -2.f}}, error);
    REQUIRE(curve.has_value());
    CurveTable table;
    table.bake(*curve);
    for (float t = 0.f; t <= 1.f; t += 0.01f) {
        REQUIRE(table.sample(t, 0.f) == Approx(curve->evaluate(t, 0.f)).margin(0.15f));
    }
    CurveTable constant;
    constant.bake(NumberCurve::range(2.f, 4.f));
    REQUIRE(constant.sample(0.9f, 0.5f) == Approx(3.f));
}

TEST_CASE("CRV7 a color curve carries transparency and blends gradients", "[curve]") {
    const ColorCurve white = ColorCurve::constant(ColorRgb{1.f, 1.f, 1.f, 1.f}, 0.25f);
    const ColorRgb c = white.evaluate(0.5f, 0.f);
    REQUIRE(c.a == Approx(0.75f));
    std::string error;
    const auto grad = ColorCurve::gradient(
        {{0.f, ColorRgb{1.f, 0.f, 0.f, 1.f}, 0.f}, {1.f, ColorRgb{0.f, 0.f, 1.f, 1.f}, 1.f}}, error);
    REQUIRE(grad.has_value());
    const ColorRgb mid = grad->evaluate(0.5f, 0.f);
    REQUIRE(mid.r == Approx(0.5f));
    REQUIRE(mid.b == Approx(0.5f));
    REQUIRE(mid.a == Approx(0.5f));
    const ColorCurve between =
        ColorCurve::between(ColorRgb{1.f, 0.f, 0.f, 1.f}, 0.f, ColorRgb{0.f, 1.f, 0.f, 1.f}, 0.f);
    REQUIRE(between.evaluate(0.9f, 1.f).g == Approx(1.f));
    ColorTable table;
    table.bake(*grad);
    REQUIRE(table.sample(0.5f, 0.f).r == Approx(0.5f).margin(0.02f));
    REQUIRE_FALSE(ColorCurve::gradient({}, error).has_value());
    REQUIRE(error.find("at least one key") != std::string::npos);
}

TEST_CASE("CRV8 equal curves compare equal", "[curve]") {
    REQUIRE(NumberCurve::constant(1.f) == NumberCurve::constant(1.f));
    REQUIRE_FALSE(NumberCurve::constant(1.f) == NumberCurve::range(1.f, 1.f));
    REQUIRE(ColorCurve::constant(ColorRgb{}, 0.f) == ColorCurve::constant(ColorRgb{}, 0.f));
    REQUIRE_FALSE(ColorCurve::constant(ColorRgb{}, 0.f) == ColorCurve::constant(ColorRgb{}, 0.5f));
}

TEST_CASE("CRV9 interpolation names go both ways", "[curve]") {
    REQUIRE((curve_interp_from_name("Smooth") == std::optional<CurveInterp>(CurveInterp::Smooth)));
    REQUIRE_FALSE(curve_interp_from_name("Wobbly").has_value());
    REQUIRE(std::string(curve_interp_name(CurveInterp::Step)) == "Step");
}
