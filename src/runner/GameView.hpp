#pragma once

#include "../ide/IdePane.hpp"
#include "Renderer.hpp"

#include <atomic>
#include <chrono>
#include <vector>

namespace engine_core {
class DataModel;
class Engine;
class TestTriangle;
}

namespace runner {

class Runner;

// Scene viewport. Draws each TestTriangle parented under the root DataModel
// at that instance's position. Heartbeat steps those instances, 90 degrees
// per simulation second. A paused simulation leaves the angles where they
// are. This page stays open.
// The corner label is how many times this view is painted per second, averaged
// over a quarter of a second. That count keeps moving while the simulation is paused.
class GameView : public ide::IdePane {
public:
    explicit GameView(Runner& runner);

protected:
    void layoutChildren() override;
    // The viewport clear covers children drawn in the normal pass.
    void renderChildren(jadefx::UiRenderer& renderer, float opacity) override;
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void sceneChanged(jadefx::Scene* previous) override;

private:
    void notePaint();
    void refreshFpsLabel();
    void refreshTriangles();
    bool ensureGraphics();

    Renderer renderer_;
    // The session DataModel. The runner keeps it alive for this view.
    engine_core::DataModel* model_ = nullptr;
    // Root TestTriangles. Refreshed when the hierarchy changes. Heartbeat writes
    // each angle. This thread only reads the atomics.
    std::vector<engine_core::TestTriangle*> triangles_;
    std::vector<engine_core::TestTriangle*> triangleScratch_;
    // The engine that owns model_. Each paint tells its render thread a frame happened.
    engine_core::Engine* engine_ = nullptr;
    // Paints in the current window. The label reads the finished average.
    std::chrono::steady_clock::time_point paintWindowStart_{};
    int paintWindowFrames_ = 0;
    bool paintWindowOpen_ = false;
    std::atomic<int> measuredFps_{0};
    jadefx::Label* fpsLabel_ = nullptr;
    int shownFps_ = -1;
    bool graphicsAttempted_ = false;
    bool graphicsReady_ = false;
};

}  // namespace runner
