#include "GameView.hpp"

#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "ScriptAnalysis.hpp"
#include "TestTriangle.hpp"
#include "Runner.hpp"
#include "gl.hpp"
#include "UserInputService.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>

namespace runner {
namespace {

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

GameView::GameView(Runner& runner, std::string name, bool closable)
    : ide::IdePane(std::move(name), closable),
      game_(&runner.simulation().datamodel()),
      engine_(&runner.simulation()) {
    setIconFile("Camera.png");
    setMinSize(64, 64);
    // Its color is the theme's --ide-viewport-color, through the studio's stylesheet.
    getClassList().add("ide-viewport");

    auto label = jadefx::make<jadefx::Label>("0 FPS");
    label->getClassList().add("ide-fps");
    label->setMouseTransparent(true);
    fpsLabel_ = label.get();
    getChildren().add(std::move(label));
    refreshTriangles();
}

void GameView::requestCapture(std::function<void(ViewPixels)> done) {
    if (done) {
        captures_.push_back(std::move(done));
    }
}

void GameView::notePaint() {
    const auto now = std::chrono::steady_clock::now();
    if (!paintWindowOpen_) {
        paintWindowOpen_ = true;
        paintWindowStart_ = now;
        paintWindowFrames_ = 0;
        return;
    }
    ++paintWindowFrames_;
    const double elapsed = std::chrono::duration<double>(now - paintWindowStart_).count();
    // One fast paint must not become the number on the label. A quarter-second
    // of paints is long enough to be a real rate and short enough to follow a change.
    constexpr double kWindowSeconds = 0.25;
    if (elapsed < kWindowSeconds || paintWindowFrames_ <= 0) {
        return;
    }
    measuredFps_.store(FramesPerSecond(elapsed / static_cast<double>(paintWindowFrames_)));
    paintWindowStart_ = now;
    paintWindowFrames_ = 0;
}

void GameView::refreshFpsLabel() {
    if (fpsLabel_ == nullptr) {
        return;
    }
    const int fps = measuredFps_.load();
    if (fps <= 0 || fps == shownFps_) {
        return;
    }
    shownFps_ = fps;
    fpsLabel_->setText(std::to_string(fps) + " FPS");
}

void GameView::layoutChildren() {
    refreshFpsLabel();
    StackPane::layoutChildren();
}

void GameView::refreshTriangles() {
    if (game_ == nullptr) {
        return;
    }
    // The simulation thread may be inside a step. Skip this frame rather than
    // waiting it out. The previous list stays drawable.
    engine_core::DataModelLock lock(*game_, engine_core::DataModelLock::Read, std::chrono::milliseconds(1));
    if (!lock.owns()) {
        return;
    }
    triangleScratch_.clear();
    // What renders is in Workspace, at any depth, in tree order.
    walkScratch_.clear();
    if (const engine_core::InstanceId workspace = game_->scene_service("Workspace"); workspace != 0) {
        walkScratch_.push_back(workspace);
    }
    while (!walkScratch_.empty()) {
        const engine_core::InstanceId id = walkScratch_.back();
        walkScratch_.pop_back();
        if (auto* triangle = dynamic_cast<engine_core::TestTriangle*>(game_->instance(id))) {
            triangleScratch_.push_back(triangle);
        }
        const std::size_t first = walkScratch_.size();
        for (engine_core::InstanceId child = game_->first_child(id); child != 0; child = game_->next_sibling(child)) {
            walkScratch_.push_back(child);
        }
        std::reverse(walkScratch_.begin() + static_cast<std::ptrdiff_t>(first), walkScratch_.end());
    }
    if (triangleScratch_.size() == triangles_.size()) {
        bool same = true;
        for (std::size_t i = 0; i < triangleScratch_.size(); ++i) {
            if (triangleScratch_[i] != triangles_[i]) {
                same = false;
                break;
            }
        }
        if (same) {
            return;
        }
    }
    triangles_.swap(triangleScratch_);
}

void GameView::renderChildren(jadefx::UiRenderer&, float) {}

void GameView::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    notePaint();
    refreshTriangles();
    const jadefx::Scene* scene = getScene();
    if (scene != nullptr && scene->getWidth() > 0.0 && scene->getHeight() > 0.0 && getWidth() > 0.0 &&
        getHeight() > 0.0 && ensureGraphics()) {
        std::vector<TriangleDraw> draws;
        draws.reserve(triangles_.size());
        for (engine_core::TestTriangle* triangle : triangles_) {
            if (triangle == nullptr) {
                continue;
            }
            const engine_core::Vec3 position = triangle->position();
            TriangleDraw draw;
            draw.angleDegrees = static_cast<float>(triangle->angle_degrees());
            draw.x = position.x;
            draw.y = position.y;
            draw.z = position.z;
            draws.push_back(draw);
        }
        // The same color as the pane around the drawing, so no seam shows.
        const jadefx::Color& clear = computedStyle().background.color;
        renderer_.setClearColor(clear.r, clear.g, clear.b);
        renderer_.draw(getAbsoluteX(), getAbsoluteY(), getWidth(), getHeight(), scene->getWidth(), scene->getHeight(),
                       draws.data(), static_cast<int>(draws.size()));
        // Read before the children paint, so the FPS label is not in the picture.
        if (!captures_.empty()) {
            ViewPixels pixels;
            renderer_.read(getAbsoluteX(), getAbsoluteY(), getWidth(), getHeight(), scene->getWidth(),
                           scene->getHeight(), pixels);
            std::vector<std::function<void(ViewPixels)>> waiting;
            waiting.swap(captures_);
            for (const auto& done : waiting) {
                done(pixels);
            }
        }
    }
    // Painted after the clear, so the label stays on top of the viewport.
    Node::renderChildren(renderer, opacity);
    // The render thread waits on this when it is uncapped, so its step follows
    // the paint instead of looping again as soon as the step itself returns.
    if (engine_ != nullptr) {
        engine_->note_client_frame();
        // This paint runs on the UI thread. The engine render thread is waiting
        // on the frame note above, so publishing analysis here is not RenderThread.
        engine_->analysis().pump();
    }
}

void GameView::sceneChanged(jadefx::Scene* previous) {
    // Leaving a live scene releases the GL objects. The context that created
    // them is still current: a move between windows shuts down before the new
    // window paints, and that paint creates them again. Scene teardown runs
    // after JadeFX has destroyed the context, so those names are left alone.
    if (previous == nullptr || previous->isTearingDown() || getScene() == previous) {
        return;
    }
    renderer_.shutdown();
    graphicsAttempted_ = false;
    graphicsReady_ = false;
    graphicsTries_ = 0;
}

float GameView::localX(double x) const { return static_cast<float>(x - getAbsoluteX()); }

float GameView::localY(double y) const { return static_cast<float>(y - getAbsoluteY()); }

void GameView::handleMousePressed(const jadefx::MouseEvent& event) {
    // Keys go to the focused node, so a click is how a player gives the game the keyboard.
    requestFocus();
    if (game_ != nullptr) {
        game_->input().post_mouse_button(event.button, true, localX(event.x), localY(event.y));
    }
    IdePane::handleMousePressed(event);
}

void GameView::handleMouseReleased(const jadefx::MouseEvent& event) {
    if (game_ != nullptr) {
        game_->input().post_mouse_button(event.button, false, localX(event.x), localY(event.y));
    }
    IdePane::handleMouseReleased(event);
}

void GameView::handleMouseDragged(const jadefx::MouseEvent& event) {
    if (game_ != nullptr) {
        game_->input().post_mouse_move(localX(event.x), localY(event.y));
    }
    IdePane::handleMouseDragged(event);
}

void GameView::handleMouseMoved(const jadefx::MouseEvent& event) {
    if (game_ != nullptr) {
        game_->input().post_mouse_move(localX(event.x), localY(event.y));
    }
    IdePane::handleMouseMoved(event);
}

void GameView::handleScroll(jadefx::ScrollEvent& event) {
    if (game_ != nullptr) {
        game_->input().post_wheel(localX(event.x), localY(event.y), static_cast<float>(event.deltaY));
    }
    IdePane::handleScroll(event);
}

void GameView::handleKey(jadefx::KeyEvent& event) {
    // A held key repeats. InputBegan fires once, on the first press.
    if (game_ != nullptr && !event.repeat) {
        const int key = engine_core::UserInputService::key_code_from_glfw(event.key);
        game_->input().post_key(key, event.pressed);
    }
    IdePane::handleKey(event);
}

void GameView::handleFocusLost() {
    // The release will go to whatever has focus now, so end the held keys here.
    if (game_ != nullptr) {
        game_->input().post_focus_lost();
    }
    IdePane::handleFocusLost();
}

bool GameView::ensureGraphics() {
    // A failed setup tries again a few times, a couple of seconds apart, rather
    // than leaving the view black for the session.
    const auto now = std::chrono::steady_clock::now();
    if (graphicsAttempted_ && (graphicsReady_ || graphicsTries_ >= kGraphicsTries || now < graphicsRetryAt_)) {
        return graphicsReady_;
    }
    graphicsAttempted_ = true;
    ++graphicsTries_;
    graphicsRetryAt_ = now + std::chrono::seconds(2);
    const bool loaded = LoadGl([](const char* name) -> void* {
        return reinterpret_cast<void*>(glfwGetProcAddress(name));
    });
    graphicsReady_ = loaded && renderer_.initialize();
    return graphicsReady_;
}

}  // namespace runner
