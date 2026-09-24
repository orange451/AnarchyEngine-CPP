#pragma once

#include "IdePane.hpp"
#include "../runner/Renderer.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace runner {
class Runner;
}

namespace engine_core {
class TestTriangle;
}

namespace ide {

// Scene viewport. Draws each TestTriangle parented under the root DataModel
// at that instance's position. Heartbeat steps those instances, 90 degrees
// per simulation second. A paused simulation leaves the angles where they
// are. This page stays open.
// PreRender passes the render step. The corner label shows frames per second
// from that delta, including while the simulation is paused.
class IdeGameView : public IdePane {
public:
    explicit IdeGameView(runner::Runner& runner);

protected:
    void layoutChildren() override;
    // The viewport clear covers children drawn in the normal pass.
    void renderChildren(jadefx::UiRenderer& renderer, float opacity) override;
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void sceneChanged(jadefx::Scene* previous) override;

private:
    void refreshFpsLabel();
    bool ensureGraphics();

    runner::Renderer renderer_;
    // Found before the threads start. Heartbeat writes each angle. This thread
    // only reads the atomics. The runner keeps the instances until it stops.
    std::vector<engine_core::TestTriangle*> triangles_;
    // PreRender writes the step on the render thread. The UI thread reads it.
    // The callback keeps a weak reference, so the value can die with this view.
    std::shared_ptr<std::atomic<double>> renderDt_;
    jadefx::Label* fpsLabel_ = nullptr;
    int shownFps_ = -1;
    bool graphicsAttempted_ = false;
    bool graphicsReady_ = false;
};

}  // namespace ide
