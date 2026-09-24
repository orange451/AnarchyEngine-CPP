#include "IdeGameView.hpp"

#include "Engine.hpp"
#include "../runner/Runner.hpp"
#include "../runner/gl.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cmath>
#include <string>

namespace ide {
namespace {

// A full turn takes four seconds of Heartbeat time.
constexpr double kDegreesPerSecond = 90.0;

int FramesPerSecond(double dt) {
    const double frames = 1.0 / dt;
    if (!std::isfinite(frames) || frames <= 0.0) {
        return 0;
    }
    constexpr double kMaxFrames = 1000000.0;
    if (frames > kMaxFrames) {
        return static_cast<int>(kMaxFrames);
    }
    return static_cast<int>(std::lround(frames));
}

}  // namespace

IdeGameView::IdeGameView(runner::Runner& runner)
    : IdePane("Scene View", false),
      angleDegrees_(std::make_shared<std::atomic<double>>(0.0)),
      renderDt_(std::make_shared<std::atomic<double>>(0.0)) {
    setMinSize(64, 64);
    getClassList().add("ide-viewport");
    setBackground(jadefx::Color::rgb8(30, 30, 30));

    auto label = jadefx::make<jadefx::Label>("0 FPS");
    label->getClassList().add("ide-fps");
    label->setMouseTransparent(true);
    fpsLabel_ = label.get();
    getChildren().add(std::move(label));

    // The shell builds this view after prepare and before start, so neither
    // loop is running. PreRender keeps running while the simulation is paused,
    // and a bind after start would race that loop.
    std::weak_ptr<std::atomic<double>> angle = angleDegrees_;
    runner.simulation().scheduler().bind(engine_core::Phase::Heartbeat, [angle](double dt) {
        const std::shared_ptr<std::atomic<double>> current = angle.lock();
        if (!current) {
            return;
        }
        double next = std::fmod(current->load() + dt * kDegreesPerSecond, 360.0);
        if (next < 0) {
            next += 360.0;
        }
        current->store(next);
    });

    std::weak_ptr<std::atomic<double>> sample = renderDt_;
    runner.simulation().scheduler().bind(engine_core::Phase::PreRender, [sample](double dt) {
        if (!(dt > 0.0)) {
            return;
        }
        const std::shared_ptr<std::atomic<double>> current = sample.lock();
        if (!current) {
            return;
        }
        current->store(dt);
    });
}

void IdeGameView::refreshFpsLabel() {
    if (fpsLabel_ == nullptr) {
        return;
    }
    const double dt = renderDt_->load();
    if (!(dt > 0.0)) {
        return;
    }
    const int fps = FramesPerSecond(dt);
    if (fps == shownFps_) {
        return;
    }
    shownFps_ = fps;
    fpsLabel_->setText(std::to_string(fps) + " FPS");
}

void IdeGameView::layoutChildren() {
    refreshFpsLabel();
    StackPane::layoutChildren();
}

void IdeGameView::renderChildren(jadefx::UiRenderer&, float) {}

void IdeGameView::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    const jadefx::Scene* scene = getScene();
    if (scene != nullptr && scene->getWidth() > 0.0 && scene->getHeight() > 0.0 && getWidth() > 0.0 &&
        getHeight() > 0.0 && ensureGraphics()) {
        renderer_.draw(getAbsoluteX(), getAbsoluteY(), getWidth(), getHeight(), scene->getWidth(), scene->getHeight(),
                       static_cast<float>(angleDegrees_->load()));
    }
    // Painted after the clear, so the label stays on top of the viewport.
    Node::renderChildren(renderer, opacity);
}

void IdeGameView::sceneChanged(jadefx::Scene* previous) {
    // Leaving a live scene can release the GL objects. The context is still
    // current then. Scene teardown runs after JadeFX has destroyed the context,
    // so those names are left for the process to reclaim.
    if (getScene() != nullptr || previous == nullptr || previous->isTearingDown()) {
        return;
    }
    renderer_.shutdown();
    graphicsAttempted_ = false;
    graphicsReady_ = false;
}

bool IdeGameView::ensureGraphics() {
    if (graphicsAttempted_) {
        return graphicsReady_;
    }
    graphicsAttempted_ = true;
    const bool loaded = runner::LoadGl([](const char* name) -> void* {
        return reinterpret_cast<void*>(glfwGetProcAddress(name));
    });
    graphicsReady_ = loaded && renderer_.initialize();
    return graphicsReady_;
}

}  // namespace ide
