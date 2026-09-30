#include "GameView.hpp"

#include "Camera.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "ScriptAnalysis.hpp"
#include "Runner.hpp"
#include "SceneFeed.hpp"
#include "ScriptRuntime.hpp"
#include "amesh.hpp"
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
      feed_(&runner.feed()),
      // A mesh that does not draw says why in the Output console.
      meshes_([this](const std::string& message) {
          if (engine_ != nullptr) {
              engine_->scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error, message);
          }
      }),
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

    auto cameras = jadefx::make<jadefx::ComboBox>();
    cameras->getClassList().add("ide-camera-list");
    cameras->setPromptText("No Camera");
    cameras->setOnAction([this](jadefx::ActionEvent&) {
        const int index = cameraBox_->getSelectionIndex();
        if (index >= 0 && static_cast<std::size_t>(index) < listed_.size()) {
            linkCamera(listed_[static_cast<std::size_t>(index)].guid);
        }
    });
    cameraBox_ = cameras.get();
    getChildren().add(std::move(cameras));
    refreshWorkspace();
}

void GameView::linkCamera(std::string guid) {
    if (guid == cameraGuid_) {
        return;
    }
    cameraGuid_ = std::move(guid);
    cameraId_ = 0;
    for (const CameraChoice& choice : cameras_) {
        if (choice.guid == cameraGuid_) {
            cameraId_ = choice.id;
            break;
        }
    }
}

void GameView::requestCapture(std::function<void(ViewPixels)> done) {
    if (done) {
        captures_.push_back(std::move(done));
    }
}

void GameView::collectMeshes() {
    meshDraws_.clear();
    // The open project's resources folder, which Project keeps on the game.
    meshes_.setRoot(game_ != nullptr ? game_->resources_root() : std::filesystem::path());
    const engine_core::VisualSnapshot& snapshot = feed_->latest();
    followCamera(snapshot);
    // Each Prefab's meshes once, however many GameObjects draw it.
    if (prefabMeshes_.size() < snapshot.prefabs.size()) {
        prefabMeshes_.resize(snapshot.prefabs.size());
    }
    for (std::size_t index = 0; index < snapshot.prefabs.size(); ++index) {
        std::vector<const anarchy::amesh::GpuMesh*>& loaded = prefabMeshes_[index];
        loaded.clear();
        for (const engine_core::VisualMesh& source : snapshot.prefabs[index].meshes) {
            // A play session's geometry, while it lasts, else the Mesh's file.
            const anarchy::amesh::GpuMesh* mesh = source.session != nullptr
                                                      ? meshes_.getSession(source.mesh, *source.session, source.revision)
                                                      : meshes_.get(source.path);
            if (mesh != nullptr) {
                loaded.push_back(mesh);
            }
        }
    }
    // Uploads for sessions no Mesh draws now, as after a Stop. None this frame uses.
    meshes_.sweepSessions();
    for (const engine_core::VisualInstance& row : snapshot.instances) {
        if (!row.alive || row.prefab == 0 || row.prefab >= snapshot.prefabs.size()) {
            continue;
        }
        for (const anarchy::amesh::GpuMesh* mesh : prefabMeshes_[row.prefab]) {
            meshDraws_.push_back(MeshDraw{mesh, row.world});
        }
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
    // Layout runs every frame, before the paint, so the list and the link are
    // current when the list lays out and when the paint follows the Camera.
    refreshWorkspace();
    refreshCameraList();
    StackPane::layoutChildren();
    // The list sits in the top right corner, over the drawing, clear of the
    // FPS label on the left when the view is wide enough for both.
    constexpr double kMargin = 6.0;
    constexpr double kListWidth = 160.0;
    const double width = std::max(0.0, std::min(kListWidth, contentWidth() - 2 * kMargin));
    const double height = cameraBox_->measuredHeight(width, contentHeight());
    cameraBox_->performLayout(contentLeft() + contentWidth() - kMargin - width, contentTop() + kMargin, width, height);
}

void GameView::refreshWorkspace() {
    if (game_ == nullptr) {
        return;
    }
    // The simulation thread may be inside a step. Skip this frame rather than
    // waiting it out. The previous list and link stay.
    engine_core::DataModelLock lock(*game_, engine_core::DataModelLock::Read, std::chrono::milliseconds(1));
    if (!lock.owns()) {
        return;
    }
    // Assigned in place, so names that did not change keep their buffers.
    std::size_t cameraCount = 0;
    // The list offers the Cameras in Workspace, at any depth, in tree order.
    walkScratch_.clear();
    if (const engine_core::InstanceId workspace = game_->scene_service("Workspace"); workspace != 0) {
        walkScratch_.push_back(workspace);
    }
    while (!walkScratch_.empty()) {
        const engine_core::InstanceId id = walkScratch_.back();
        walkScratch_.pop_back();
        if (dynamic_cast<engine_core::Camera*>(game_->instance(id)) != nullptr) {
            if (cameraCount == cameraScratch_.size()) {
                cameraScratch_.emplace_back();
            }
            CameraChoice& choice = cameraScratch_[cameraCount++];
            choice.id = id;
            choice.guid = game_->guid(id);
            choice.name = game_->name(id);
        }
        const std::size_t first = walkScratch_.size();
        for (engine_core::InstanceId child = game_->first_child(id); child != 0; child = game_->next_sibling(child)) {
            walkScratch_.push_back(child);
        }
        std::reverse(walkScratch_.begin() + static_cast<std::ptrdiff_t>(first), walkScratch_.end());
    }
    cameraScratch_.resize(cameraCount);
    if (cameraScratch_ != cameras_) {
        cameras_.swap(cameraScratch_);
    }
    // Another place drops the link, so the view takes that place's Camera.
    std::string place = game_->guid(0);
    if (place != placeGuid_) {
        placeGuid_ = std::move(place);
        cameraGuid_.clear();
    }
    if (cameraGuid_.empty() && !cameras_.empty()) {
        cameraGuid_ = cameras_.front().guid;
    }
    cameraId_ = 0;
    for (const CameraChoice& choice : cameras_) {
        if (choice.guid == cameraGuid_) {
            cameraId_ = choice.id;
            break;
        }
    }
}

void GameView::refreshCameraList() {
    if (cameras_ == listed_ && cameraGuid_ == listedGuid_) {
        return;
    }
    if (cameras_ != listed_) {
        listed_ = cameras_;
        std::vector<std::string> names;
        names.reserve(listed_.size());
        for (const CameraChoice& choice : listed_) {
            names.push_back(choice.name);
        }
        cameraBox_->getItems().setAll(std::move(names));
    }
    listedGuid_ = cameraGuid_;
    int selected = -1;
    for (std::size_t index = 0; index < listed_.size(); ++index) {
        if (listed_[index].guid == cameraGuid_) {
            selected = static_cast<int>(index);
            break;
        }
    }
    // A linked Camera that is gone shows the prompt.
    cameraBox_->select(selected);
}

void GameView::followCamera(const engine_core::VisualSnapshot& snapshot) {
    if (cameraId_ == 0) {
        return;
    }
    for (const engine_core::VisualInstance& row : snapshot.instances) {
        if (row.id == cameraId_) {
            if (row.alive && row.field_of_view > 0.f) {
                renderer_.setCamera(row.world, row.field_of_view);
            }
            return;
        }
    }
}

void GameView::renderChildren(jadefx::UiRenderer&, float) {}

void GameView::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    notePaint();
    const jadefx::Scene* scene = getScene();
    if (scene != nullptr && scene->getWidth() > 0.0 && scene->getHeight() > 0.0 && getWidth() > 0.0 &&
        getHeight() > 0.0 && ensureGraphics()) {
        // The same color as the pane around the drawing, so no seam shows.
        const jadefx::Color& clear = computedStyle().background.color;
        renderer_.setClearColor(clear.r, clear.g, clear.b);
        collectMeshes();
        renderer_.draw(getAbsoluteX(), getAbsoluteY(), getWidth(), getHeight(), scene->getWidth(), scene->getHeight(),
                       meshDraws_.data(), static_cast<int>(meshDraws_.size()));
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
    meshes_.clear();
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
