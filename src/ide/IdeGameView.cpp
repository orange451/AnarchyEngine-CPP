#include "IdeGameView.hpp"

#include "Engine.hpp"
#include "TestTriangle.hpp"
#include "../runner/Runner.hpp"
#include "../runner/gl.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cmath>
#include <string>
#include <vector>

namespace ide {
namespace {

std::vector<engine_core::TestTriangle*> FindSceneTriangles(runner::Runner& runner) {
    std::vector<engine_core::TestTriangle*> found;
    engine_core::DataModel& model = runner.simulation().datamodel();
    for (engine_core::InstanceId id = model.first_child(model.id()); id != 0; id = model.next_sibling(id)) {
        if (auto* triangle = dynamic_cast<engine_core::TestTriangle*>(model.instance(id))) {
            found.push_back(triangle);
        }
    }
    return found;
}

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
      triangles_(FindSceneTriangles(runner)),
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
        std::vector<runner::TriangleDraw> draws;
        draws.reserve(triangles_.size());
        for (engine_core::TestTriangle* triangle : triangles_) {
            if (triangle == nullptr) {
                continue;
            }
            const engine_core::Vec3 position = triangle->position();
            runner::TriangleDraw draw;
            draw.angleDegrees = static_cast<float>(triangle->angle_degrees());
            draw.x = position.x;
            draw.y = position.y;
            draw.z = position.z;
            draws.push_back(draw);
        }
        renderer_.draw(getAbsoluteX(), getAbsoluteY(), getWidth(), getHeight(), scene->getWidth(), scene->getHeight(),
                       draws.data(), static_cast<int>(draws.size()));
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
