#pragma once

#include "IdePane.hpp"
#include "../runner/Renderer.hpp"
#include "StepEvents.hpp"

namespace ide {

// Scene viewport. The runner draws its picture here.
// The rainbow triangle stays still until the simulation publishes a step,
// then it turns by that step's delta time. This page stays open.
class IdeGameView : public IdePane {
public:
    explicit IdeGameView(engine_core::StepEvents& steps);

protected:
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void sceneChanged(jadefx::Scene* previous) override;

private:
    bool ensureGraphics();

    runner::Renderer renderer_;
    engine_core::StepEvents* steps_ = nullptr;
    double angleDegrees_ = 0;
    bool graphicsAttempted_ = false;
    bool graphicsReady_ = false;
};

}  // namespace ide
