// BloomMath: bloom's chain and soft threshold, with no GL context.
// bloom.glsl's brightWeight is BrightWeight, line for line.

#include "runner/BloomMath.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using namespace runner;

TEST_CASE("BM1 a pane has a level for each halving that stays 2 pixels wide, up to 8", "[bloom]") {
    REQUIRE(BloomLevelsAvailable(1920, 1080) == kBloomMaxLevels);
    REQUIRE(BloomLevelsAvailable(4, 4) == 1);
    REQUIRE(BloomLevelsAvailable(3, 3) == 0);
    REQUIRE(BloomLevelsAvailable(0, 0) == 0);
    // The shorter side decides.
    REQUIRE(BloomLevelsAvailable(8, 1000) == 2);
    REQUIRE(BloomLevelsAvailable(1000, 8) == 2);
}

TEST_CASE("BM2 Size spreads the same share of the pane at any height", "[bloom]") {
    // The spread in pane pixels is 2^levels * radius; as a share of the height it must not change.
    const auto share = [](int width, int height) {
        const BloomPlan plan = PlanBloom(24.f, width, height);
        REQUIRE(plan.levels > 0);
        return static_cast<float>(1 << plan.levels) * plan.radius / static_cast<float>(height);
    };
    const float at1080 = share(1920, 1080);
    REQUIRE(share(960, 540) == Approx(at1080));
    REQUIRE(share(3840, 2160) == Approx(at1080));
    // 24 pixels at 1080: 32-pixel texels, with the tent drawn in to three quarters.
    const BloomPlan plan = PlanBloom(24.f, 1920, 1080);
    REQUIRE(plan.levels == 5);
    REQUIRE(plan.radius == Approx(0.75f));
    REQUIRE(PlanBloom(24.f, 960, 540).levels == 4);
    REQUIRE(PlanBloom(24.f, 3840, 2160).levels == 6);
}

TEST_CASE("BM3 Size moves the spread smoothly between level counts", "[bloom]") {
    float previous = 0.f;
    for (float size = 1.f; size <= 56.f; size += 0.5f) {
        const BloomPlan plan = PlanBloom(size, 1920, 1080);
        const float spread = static_cast<float>(1 << plan.levels) * plan.radius;
        INFO(size);
        REQUIRE(spread >= previous);
        previous = spread;
    }
}

TEST_CASE("BM4 no Size, or no room, plans no bloom", "[bloom]") {
    REQUIRE(PlanBloom(0.f, 1920, 1080).levels == 0);
    REQUIRE(PlanBloom(-1.f, 1920, 1080).levels == 0);
    REQUIRE(PlanBloom(24.f, 3, 3).levels == 0);
    // A tiny spread still draws one level, with the tent at its smallest.
    const BloomPlan tiny = PlanBloom(0.5f, 64, 64);
    REQUIRE(tiny.levels == 1);
    REQUIRE(tiny.radius == Approx(kBloomMinRadius));
}

TEST_CASE("BM5 the threshold fades in over a knee half its size", "[bloom]") {
    // Threshold 0 takes all of any light.
    REQUIRE(BrightWeight(0.01f, 0.f) == 1.f);
    REQUIRE(BrightWeight(5.f, 0.f) == 1.f);
    REQUIRE(BrightWeight(0.f, 0.f) == 0.f);
    // Below threshold - knee, nothing.
    REQUIRE(BrightWeight(0.49f, 1.f) == 0.f);
    // Above threshold + knee, exactly the light above the threshold.
    REQUIRE(BrightWeight(3.f, 1.f) * 3.f == Approx(2.f));
    // Continuous across both ends of the knee, and rising through it.
    const float t = 2.f;
    const float k = t * 0.5f;
    REQUIRE(BrightWeight(t - k + 1e-4f, t) == Approx(0.f).margin(1e-6));
    REQUIRE(BrightWeight(t + k - 1e-4f, t) == Approx(BrightWeight(t + k + 1e-4f, t)).epsilon(1e-3));
    float previous = 0.f;
    for (float b = t - k; b <= t + k; b += 0.05f) {
        const float light = BrightWeight(b, t) * b;
        REQUIRE(light >= previous);
        previous = light;
    }
}
