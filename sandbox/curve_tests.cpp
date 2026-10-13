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

#include "CurveJson.hpp"
#include "PropertyReflection.hpp"

namespace {
JsonValue parsed(const char* text) {
    JsonValue out;
    std::string error;
    REQUIRE(parse_json(text, out, error));
    return out;
}
}  // namespace

TEST_CASE("CRV10 curves round-trip through JSON and refuse bad JSON", "[curve]") {
    std::string error;
    const auto curve = NumberCurve::keys({{0.f, 0.f}, {0.2f, 3.f, CurveInterp::Smooth}, {1.f, 0.f}}, error);
    REQUIRE(curve.has_value());
    const auto back = number_curve_from_json(number_curve_to_json(*curve), error);
    REQUIRE(back.has_value());
    REQUIRE(*back == *curve);
    REQUIRE(compact_json(number_curve_to_json(NumberCurve::constant(5.f))) == R"({"mode":"Constant","value":5})");
    REQUIRE(compact_json(number_curve_to_json(NumberCurve::range(1.f, 3.f))) == R"({"max":3,"min":1,"mode":"Range"})");
    REQUIRE(compact_json(number_curve_to_json(*curve)) ==
            R"({"keys":[[0,0,"Linear"],[0.2,3,"Smooth"],[1,0,"Linear"]],"mode":"Curve"})");

    REQUIRE_FALSE(number_curve_from_json(parsed(R"({"mode":"Curve","keys":[]})"), error).has_value());
    REQUIRE(error.find("at least one key") != std::string::npos);
    REQUIRE_FALSE(number_curve_from_json(parsed(R"({"mode":"Wavy"})"), error).has_value());
    REQUIRE(error.find("unknown mode") != std::string::npos);
    REQUIRE_FALSE(number_curve_from_json(parsed(R"({"mode":"Curve","keys":[[0,0,"Wobbly"]]})"), error).has_value());
    REQUIRE(error.find("interpolation") != std::string::npos);
    REQUIRE_FALSE(number_curve_from_json(parsed(R"({"mode":"Curve","keys":[[0]]})"), error).has_value());
    REQUIRE(error.find("malformed key") != std::string::npos);
    REQUIRE_FALSE(number_curve_from_json(parsed("5"), error).has_value());

    const ColorCurve between =
        ColorCurve::between(ColorRgb{1.f, 0.5f, 0.f, 1.f}, 0.f, ColorRgb{1.f, 1.f, 0.f, 1.f}, 0.25f);
    const auto color_back = color_curve_from_json(color_curve_to_json(between), error);
    REQUIRE(color_back.has_value());
    REQUIRE(*color_back == between);
    const auto grad = ColorCurve::gradient({{0.f, ColorRgb{1.f, 0.8f, 0.2f, 1.f}, 0.f}, {1.f, ColorRgb{0.3f, 0.3f, 0.3f, 1.f}, 1.f}}, error);
    REQUIRE(*color_curve_from_json(color_curve_to_json(*grad), error) == *grad);
    REQUIRE(compact_json(color_curve_to_json(ColorCurve::constant(ColorRgb{1.f, 1.f, 1.f, 1.f}, 0.f))) ==
            R"({"color":[1,1,1],"mode":"Constant","transparency":0})");
    REQUIRE_FALSE(color_curve_from_json(parsed(R"({"mode":"Gradient","keys":[[0,5,0]]})"), error).has_value());
}

TEST_CASE("CRV11 slots of curves convert to JSON by type name and compare by value", "[curve]") {
    const LuaSlot a = number_curve_slot(NumberCurve::range(1.f, 2.f));
    const LuaSlot b = number_curve_slot(NumberCurve::range(1.f, 2.f));
    REQUIRE(same_slot(a, b));
    REQUIRE_FALSE(same_slot(a, number_curve_slot(NumberCurve::range(1.f, 3.f))));
    JsonValue json;
    REQUIRE(slot_to_json(a, "NumberCurve", json));
    LuaSlot back;
    std::string error;
    REQUIRE(slot_from_json(json, "NumberCurve", "Size", back, error));
    REQUIRE(back.kind == LuaSlot::Kind::NumberCurve);
    REQUIRE(*back.number_curve == NumberCurve::range(1.f, 2.f));
    REQUIRE_FALSE(slot_from_json(parsed("5"), "NumberCurve", "Size", back, error));
    REQUIRE(error.find("Size") != std::string::npos);

    const LuaSlot c = color_curve_slot(ColorCurve::constant(ColorRgb{1.f, 0.f, 0.f, 1.f}, 0.5f));
    REQUIRE(slot_to_json(c, "ColorCurve", json));
    REQUIRE(slot_from_json(json, "ColorCurve", "Color", back, error));
    REQUIRE(same_slot(c, back));
}
