#include "GameView.hpp"

#include "Camera.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "ScriptAnalysis.hpp"
#include "Runner.hpp"
#include "SceneFeed.hpp"
#include "SceneService.hpp"
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
      runner_(&runner),
      feed_(&runner.feed()),
      // A mesh that does not draw says why in the Output console.
      meshes_([this](const std::string& message) {
          if (engine_ != nullptr) {
              engine_->scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error, message);
          }
      }),
      // So does a texture.
      textures_([this](const std::string& message) {
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
    // The game hears MouseButton2 and 3 too; holding the right button turns the
    // scene camera. IdePane does nothing with a press, so no button starts a
    // tab drag or a dock action here.
    setReceivesAllButtons(true);

    // First, so the label and the list draw over the GUIs and are hit before them.
    GuiInput input;
    input.pressed = [this](const jadefx::MouseEvent& event, bool keepFocus) {
        // A click on a GUI still gives the game the keyboard, unless what was
        // clicked takes the keys itself, as a TextField does.
        if (!keepFocus) {
            requestFocus();
        }
        noteCurrentCamera();
        if (game_ != nullptr) {
            game_->input().post_mouse_button(event.button, true, localX(event.x), localY(event.y), true);
        }
    };
    input.released = [this](const jadefx::MouseEvent& event) {
        if (game_ != nullptr) {
            game_->input().post_mouse_button(event.button, false, localX(event.x), localY(event.y), true);
        }
    };
    input.dragged = [this](const jadefx::MouseEvent& event) {
        if (game_ != nullptr) {
            game_->input().post_mouse_move(localX(event.x), localY(event.y), true);
        }
    };
    input.moved = input.dragged;
    auto gui = jadefx::make<GuiLayer>(runner.simulation(), std::move(input));
    guiLayer_ = gui.get();
    // The GUIs have a cascade of their own: the studio's theme and stylesheets
    // stop at the SubScene, and the game's default sheet starts there.
    auto guiScene = jadefx::make<jadefx::SubScene>(std::move(gui));
    guiScene->setPickOnBounds(false);
    guiScene->setUserAgentStylesheet(GuiLayer::defaultStylesheet());
    guiScene_ = guiScene.get();
    getChildren().add(std::move(guiScene));

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
            noteCurrentCamera();
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

void GameView::noteCurrentCamera(bool onlyIfNone) {
    if (engine_ == nullptr || cameraId_ == 0) {
        return;
    }
    engine_->on_simulation([camera = cameraId_, onlyIfNone](engine_core::DataModel& game) {
        if (auto* workspace = dynamic_cast<engine_core::Workspace*>(game.instance(game.scene_service("Workspace")))) {
            if (!onlyIfNone || workspace->current_camera() == 0) {
                workspace->set_current_camera(camera);
            }
        }
    });
}

void GameView::syncPointerLock() {
    jadefx::Scene* scene = getScene();
    if (game_ == nullptr || scene == nullptr) {
        return;
    }
    if (!isFocused()) {
        // Another view of this same Scene may hold the lock now; only the
        // focused view may lock it or read whether it let the lock go. This
        // is a safety net for a lock this view still holds from just before
        // focus moved elsewhere (handleFocusLost is the usual way it drops).
        if (pointerLocked_) {
            scene->setPointerLocked(false);
            pointerLocked_ = false;
        }
        return;
    }
    engine_core::UserInputService& input = game_->input();
    if (pointerLocked_ && !scene->isPointerLocked()) {
        // The scene let the pointer go, as when the window lost focus. The view
        // lets go of the focus too, so it does not lock again until clicked.
        pointerLocked_ = false;
        scene->releaseFocus(this);
        return;
    }
    const bool wanted = input.mouse_behavior() != engine_core::UserInputService::kMouseBehaviorDefault;
    if (wanted != pointerLocked_) {
        scene->setPointerLocked(wanted);
        pointerLocked_ = wanted;
        if (wanted) {
            input.note_lock_started();
        }
    }
    if (pointerLocked_) {
        double dx = 0;
        double dy = 0;
        scene->takePointerDelta(dx, dy);
        input.post_mouse_delta(static_cast<float>(dx), static_cast<float>(dy));
    }
}

void GameView::requestCapture(std::function<void(ViewPixels)> done) {
    if (done) {
        captures_.push_back(std::move(done));
    }
}

void GameView::collectMeshes() {
    meshDraws_.clear();
    const engine_core::VisualSnapshot& snapshot = feed_->latest();
    // The folder the snapshot's paths are under, not the game's: a project
    // switch changes the game's before the snapshot catches up.
    meshes_.setRoot(snapshot.resources_root);
    textures_.setRoot(snapshot.resources_root);
    followCamera(snapshot);
    // Each Prefab's meshes once, however many GameObjects draw it.
    if (prefabMeshes_.size() < snapshot.prefabs.size()) {
        prefabMeshes_.resize(snapshot.prefabs.size());
    }
    for (std::size_t index = 0; index < snapshot.prefabs.size(); ++index) {
        std::vector<MeshDraw>& loaded = prefabMeshes_[index];
        loaded.clear();
        for (const engine_core::VisualMesh& source : snapshot.prefabs[index].meshes) {
            // A play session's geometry, while it lasts, else the Mesh's file.
            const anarchy::amesh::GpuMesh* mesh = source.session != nullptr
                                                      ? meshes_.getSession(source.mesh, *source.session, source.revision)
                                                      : meshes_.get(source.path);
            if (mesh == nullptr) {
                continue;
            }
            MeshDraw draw;
            draw.mesh = mesh;
            draw.texture = textures_.get(source.diffuse_texture);
            draw.normalTexture = textures_.get(source.normal_texture);
            draw.roughnessTexture = textures_.get(source.roughness_texture);
            draw.metalnessTexture = textures_.get(source.metalness_texture);
            draw.color[0] = source.color.r;
            draw.color[1] = source.color.g;
            draw.color[2] = source.color.b;
            draw.color[3] = source.color.a;
            draw.emissive[0] = source.emissive.r;
            draw.emissive[1] = source.emissive.g;
            draw.emissive[2] = source.emissive.b;
            draw.metalness = source.metalness;
            draw.roughness = source.roughness;
            draw.reflectivity = source.reflectivity;
            draw.transparency = source.transparency;
            loaded.push_back(draw);
        }
    }
    // Uploads for sessions no Mesh draws now, as after a Stop. None this frame uses.
    meshes_.sweepSessions();
    lightDraws_.clear();
    for (const engine_core::VisualInstance& row : snapshot.instances) {
        if (!row.alive) {
            continue;
        }
        if (row.light.kind != engine_core::VisualLight::Kind::None && row.light.enabled) {
            LightDraw light;
            switch (row.light.kind) {
            case engine_core::VisualLight::Kind::Spot:
                light.kind = LightDraw::Kind::Spot;
                break;
            case engine_core::VisualLight::Kind::Directional:
                light.kind = LightDraw::Kind::Directional;
                break;
            default:
                light.kind = LightDraw::Kind::Point;
                break;
            }
            const engine_core::Vec3 position = engine_core::matrix4_position(row.world);
            light.position[0] = position.x;
            light.position[1] = position.y;
            light.position[2] = position.z;
            // Down the row's -Z, as a Camera looks. The renderer makes it unit length.
            light.direction[0] = -row.world.m[8];
            light.direction[1] = -row.world.m[9];
            light.direction[2] = -row.world.m[10];
            if (light.kind == LightDraw::Kind::Directional) {
                // Its Direction is toward the light, so it shines the other way.
                for (int axis = 0; axis < 3; ++axis) {
                    light.direction[axis] = -row.light.direction[axis];
                }
            }
            std::copy(row.light.color, row.light.color + 3, light.color);
            light.intensity = row.light.intensity;
            light.radius = row.light.radius;
            light.outerFovDegrees = row.light.outer_fov;
            light.innerFovScale = row.light.inner_fov_scale;
            lightDraws_.push_back(light);
        }
        if (row.prefab == 0 || row.prefab >= snapshot.prefabs.size()) {
            continue;
        }
        // The GameObject's Color tints each Material's, and its opacity multiplies each Material's.
        const float opacity = 1.f - row.transparency;
        for (const MeshDraw& model : prefabMeshes_[row.prefab]) {
            MeshDraw& draw = meshDraws_.emplace_back(model);
            draw.model = row.world;
            draw.color[0] *= row.color.r;
            draw.color[1] *= row.color.g;
            draw.color[2] *= row.color.b;
            draw.transparency = 1.f - (1.f - std::clamp(draw.transparency, 0.f, 1.f)) * opacity;
        }
    }
    SceneLighting lighting;
    lighting.ambient[0] = snapshot.lighting.ambient.r;
    lighting.ambient[1] = snapshot.lighting.ambient.g;
    lighting.ambient[2] = snapshot.lighting.ambient.b;
    lighting.exposure = snapshot.lighting.exposure;
    lighting.saturation = snapshot.lighting.saturation;
    lighting.gamma = snapshot.lighting.gamma;
    // The Skybox's images, uploaded linear; a missing or unreadable one draws no sky.
    const engine_core::VisualSky& sky = snapshot.sky;
    if (sky.present) {
        const EnvironmentTexture image = textures_.getEnvironment(sky.image);
        const EnvironmentTexture reflections = textures_.getEnvironment(sky.reflections);
        lighting.sky.image = image.texture;
        lighting.sky.imageRevision = image.revision;
        lighting.sky.reflections = reflections.texture;
        lighting.sky.reflectionsRevision = reflections.revision;
        lighting.sky.exposure = sky.exposure;
        lighting.sky.rotationDegrees = sky.rotation;
        lighting.sky.tint[0] = sky.tint.r;
        lighting.sky.tint[1] = sky.tint.g;
        lighting.sky.tint[2] = sky.tint.b;
    }
    renderer_.setLighting(lighting);
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
    guiLayer_->sync();
    StackPane::layoutChildren();
    // The GUIs cover the whole view, whatever they would rather be.
    guiScene_->performLayout(contentLeft(), contentTop(), contentWidth(), contentHeight());
    // The list sits in the top right corner, over the drawing, clear of the
    // FPS label on the left when the view is wide enough for both.
    constexpr double kMargin = 6.0;
    constexpr double kListWidth = 160.0;
    const double width = std::max(0.0, std::min(kListWidth, contentWidth() - 2 * kMargin));
    const double height = cameraBox_->measuredHeight(width, contentHeight());
    cameraBox_->performLayout(contentLeft() + contentWidth() - kMargin - width, contentTop() + kMargin, width, height);
}

void GameView::refreshWorkspace() {
    const engine_core::InstanceId was = cameraId_;
    readWorkspace();
    // A link that just resolved, as after a load, gives the place its
    // CurrentCamera when it has none, without waiting for a click. Posted
    // after the read lock is let go: a paused edit takes the write lock.
    if (cameraId_ != 0 && cameraId_ != was) {
        noteCurrentCamera(true);
    }
}

void GameView::readWorkspace() {
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
    syncPointerLock();
    const jadefx::Scene* scene = getScene();
    if (scene != nullptr && scene->getWidth() > 0.0 && scene->getHeight() > 0.0 && getWidth() > 0.0 &&
        getHeight() > 0.0 && ensureGraphics()) {
        // The same color as the pane around the drawing, so no seam shows.
        const jadefx::Color& clear = computedStyle().background.color;
        renderer_.setClearColor(clear.r, clear.g, clear.b);
        renderer_.setGridVisible(runner_->sceneGrid());
        collectMeshes();
        const bool drawn = renderer_.draw(getAbsoluteX(), getAbsoluteY(), getWidth(), getHeight(), scene->getWidth(),
                                          scene->getHeight(), meshDraws_.data(), static_cast<int>(meshDraws_.size()),
                                          lightDraws_.data(), static_cast<int>(lightDraws_.size()));
        // Read before the children paint, so the FPS label is not in the picture.
        // A frame the driver was not ready for shows only the clear, so a capture waits for the next.
        if (drawn && !captures_.empty()) {
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
    // A lock belongs to the window the view is leaving.
    if (pointerLocked_) {
        if (previous != nullptr && !previous->isTearingDown()) {
            previous->setPointerLocked(false);
        }
        pointerLocked_ = false;
    }
    // Leaving a live scene releases the GL objects. The context that created
    // them is still current: a move between windows shuts down before the new
    // window paints, and that paint creates them again. Scene teardown runs
    // after JadeFX has destroyed the context, so those names are left alone.
    if (previous == nullptr || previous->isTearingDown() || getScene() == previous) {
        return;
    }
    renderer_.shutdown();
    meshes_.clear();
    textures_.clear();
    graphicsAttempted_ = false;
    graphicsReady_ = false;
    graphicsTries_ = 0;
}

float GameView::localX(double x) const { return static_cast<float>(x - getAbsoluteX()); }

float GameView::localY(double y) const { return static_cast<float>(y - getAbsoluteY()); }

void GameView::handleMousePressed(const jadefx::MouseEvent& event) {
    // Keys go to the focused node, so a click is how a player gives the game the keyboard.
    requestFocus();
    noteCurrentCamera();
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
    // Shift+Esc deselects the view, and so frees the pointer whatever a script
    // keeps asking for. The game does not hear it.
    if (event.pressed && event.key == jadefx::Key::Escape && event.shift && !event.control && !event.alt &&
        !event.meta) {
        event.consume();
        if (jadefx::Scene* scene = getScene()) {
            scene->releaseFocus(this);
        }
        return;
    }
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
    // Two views can share one Scene. Dropping the lock here, before the node
    // that took focus next paints, keeps this view from freeing the scene's
    // pointer out from under a view that just took it.
    if (pointerLocked_) {
        if (jadefx::Scene* scene = getScene()) {
            scene->setPointerLocked(false);
        }
        pointerLocked_ = false;
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
