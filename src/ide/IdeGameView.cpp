#include "IdeGameView.hpp"

#include "../runner/gl.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cmath>

namespace ide {
namespace {

// A full turn takes four seconds of simulation time, not wall time.
constexpr double kDegreesPerSecond = 90.0;

}  // namespace

IdeGameView::IdeGameView(engine_core::StepEvents& steps) : IdePane("Scene View", false), steps_(&steps) {
    setMinSize(64, 64);
    getClassList().add("ide-viewport");
    setBackground(jadefx::Color::rgb8(30, 30, 30));
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
    // No step event means the simulation is paused or has not caught up.
    // The triangle holds its angle instead of following the window clock.
    if (steps_ != nullptr) {
        angleDegrees_ = std::fmod(angleDegrees_ + steps_->consume() * kDegreesPerSecond, 360.0);
        if (angleDegrees_ < 0) {
            angleDegrees_ += 360.0;
        }
    }
    renderer_.draw(getAbsoluteX(), getAbsoluteY(), getWidth(), getHeight(), scene->getWidth(), scene->getHeight(),
                   static_cast<float>(angleDegrees_));
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
