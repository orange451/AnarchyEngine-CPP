#include "BloomMath.hpp"

#include <algorithm>

namespace runner {

int BloomLevelsAvailable(int width, int height) {
    int levels = 0;
    while (levels < kBloomMaxLevels && (width >> (levels + 1)) >= kBloomMinPixels &&
           (height >> (levels + 1)) >= kBloomMinPixels) {
        ++levels;
    }
    return levels;
}

BloomPlan PlanBloom(float size, int width, int height) {
    BloomPlan plan;
    const int available = BloomLevelsAvailable(width, height);
    if (!(size > 0.f) || available == 0) {
        return plan;
    }
    const float spread = size * static_cast<float>(height) / kBloomReferenceHeight;
    int levels = 1;
    while (levels < available && static_cast<float>(1 << levels) < spread) {
        ++levels;
    }
    plan.levels = levels;
    plan.radius = std::clamp(spread / static_cast<float>(1 << levels), kBloomMinRadius, kBloomMaxRadius);
    return plan;
}

float BrightWeight(float brightness, float threshold) {
    if (!(brightness > 0.f)) {
        return 0.f;
    }
    if (!(threshold > 0.f)) {
        return 1.f;
    }
    const float knee = 0.5f * threshold;
    float soft = std::clamp(brightness - threshold + knee, 0.f, 2.f * knee);
    soft = soft * soft / (4.f * knee);
    return std::max(soft, brightness - threshold) / brightness;
}

}  // namespace runner
