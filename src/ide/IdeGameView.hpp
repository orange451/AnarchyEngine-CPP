#pragma once

#include "IdePane.hpp"
#include "../runner/Renderer.hpp"

#include <atomic>
#include <memory>

namespace runner {
class Runner;
}

namespace ide {

// Scene viewport. The runner draws its picture here.
// Heartbeat turns the triangle 90 degrees per simulation second.
// A paused simulation leaves the angle where it is. This page stays open.
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
    // Heartbeat writes the angle on the simulation thread. PreRender writes the
    // step on the render thread. The UI thread reads both. Each callback keeps
    // a weak reference, so the values can die with this view.
    std::shared_ptr<std::atomic<double>> angleDegrees_;
    std::shared_ptr<std::atomic<double>> renderDt_;
    jadefx::Label* fpsLabel_ = nullptr;
    int shownFps_ = -1;
    bool graphicsAttempted_ = false;
    bool graphicsReady_ = false;
};

}  // namespace ide
