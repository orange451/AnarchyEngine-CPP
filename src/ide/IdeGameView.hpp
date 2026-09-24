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
class IdeGameView : public IdePane {
public:
    explicit IdeGameView(runner::Runner& runner);

protected:
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void sceneChanged(jadefx::Scene* previous) override;

private:
    bool ensureGraphics();

    runner::Renderer renderer_;
    // Heartbeat writes this on the simulation thread. The UI thread reads it.
    // The callback keeps a weak reference, so the angle can die with this view.
    std::shared_ptr<std::atomic<double>> angleDegrees_;
    bool graphicsAttempted_ = false;
    bool graphicsReady_ = false;
};

}  // namespace ide
