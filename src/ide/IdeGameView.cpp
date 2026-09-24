#include "IdeGameView.hpp"

#include "Engine.hpp"
#include "../runner/Runner.hpp"
#include "../runner/gl.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cmath>

namespace ide {
namespace {

// A full turn takes four seconds of Heartbeat time.
constexpr double kDegreesPerSecond = 90.0;

}  // namespace

IdeGameView::IdeGameView(runner::Runner& runner)
    : IdePane("Scene View", false), angleDegrees_(std::make_shared<std::atomic<double>>(0.0)) {
    setMinSize(64, 64);
    getClassList().add("ide-viewport");
    setBackground(jadefx::Color::rgb8(30, 30, 30));

    // The shell builds this view after Runner::start, while the simulation is
    // still paused, so this bind does not race a running Heartbeat.
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
}

void IdeGameView::renderContent(jadefx::UiRenderer&, float) {
    const jadefx::Scene* scene = getScene();
    if (scene == nullptr || scene->getWidth() <= 0.0 || scene->getHeight() <= 0.0 || getWidth() <= 0.0 ||
        getHeight() <= 0.0) {
        return;
    }
    if (!ensureGraphics()) {
        return;
    }
    renderer_.draw(getAbsoluteX(), getAbsoluteY(), getWidth(), getHeight(), scene->getWidth(), scene->getHeight(),
                   static_cast<float>(angleDegrees_->load()));
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
