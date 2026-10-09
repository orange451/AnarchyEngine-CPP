#include "GameView.hpp"

#include "Brush.hpp"

#include "profiler/Profiler.hpp"

#include "AssetInstances.hpp"
#include "Camera.hpp"
#include "ConvexDecomposition.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "PhysicsObject.hpp"
#include "PlayerController.hpp"
#include "PhysicsWorld.hpp"
#include "SelectionService.hpp"
#include "ScriptAnalysis.hpp"
#include "Runner.hpp"
#include "SceneFeed.hpp"
#include "SceneService.hpp"
#include "SkyMath.hpp"
#include "TerrainDraws.hpp"
#include "ScriptRuntime.hpp"
#include "UiFrameProfile.hpp"
#include "amesh.hpp"
#include "gl.hpp"
#include "ide/IdeIcons.hpp"
#include "UserInputService.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
        cursorX_ = event.x;
        cursorY_ = event.y;
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

    auto eye = jadefx::make<jadefx::ToggleButton>();
    eye->getClassList().add("ide-gui-toggle");
    eye->setSelected(true);
    eye->setGraphic(ide::icon_graphic("Eye.png"));
    jadefx::Tooltip::install(eye.get(), jadefx::make<jadefx::Tooltip>("Show GUI"));
    eye->setOnAction([this](jadefx::ActionEvent&) {
        guiToggle_->setGraphic(ide::icon_graphic(guiToggle_->isSelected() ? "Eye.png" : "EyeClosed.png"));
        refreshOverlays();
    });
    guiToggle_ = eye.get();
    getChildren().add(std::move(eye));

    // Terrain mode's palette (T), hidden until it is turned on.
    terrainBrush_ = std::make_unique<TerrainBrush>(*engine_);
    terrainBrush_->setOnUsed([this] { requestFocus(); });
    auto palette = terrainBrush_->makePalette();
    palette->setVisible(false);
    terrainPalette_ = palette.get();
    getChildren().add(std::move(palette));

    // Brush mode's palette (B) and the measurement beside the pointer.
    brushTool_ = std::make_unique<BrushTool>(*engine_);
    brushTool_->setOnUsed([this] { requestFocus(); });
    auto brushPalette = brushTool_->makePalette();
    brushPalette->setVisible(false);
    brushPalette_ = brushPalette.get();
    getChildren().add(std::move(brushPalette));
    auto readout = jadefx::make<jadefx::Label>("");
    readout->setStyle("font-size: 12px; padding: 2px 6px; background-color: rgba(20, 22, 26, 0.85); border-radius: 4px;");
    readout->setTextFill(jadefx::Color::rgb8(255, 214, 120));
    readout->setVisible(false);
    brushReadout_ = readout.get();
    getChildren().add(std::move(readout));

    // Last, so it draws over everything here and is hit first.
    auto overlay = jadefx::make<ProfilerOverlay>();
    overlay->setVisible(false);
    profilerOverlay_ = overlay.get();
    getChildren().add(std::move(overlay));
    refreshWorkspace();
}

GameView::~GameView() {
    ProfilerUi& ui = ProfilerUi::get();
    if (ui.owner == this) {
        ui.owner = nullptr;
    }
    if (hookedScene_ != nullptr && keyHook_ != 0 && !hookedScene_->isTearingDown()) {
        hookedScene_->removeKeyHook(keyHook_);
    }
}

bool GameView::pointerWanted() const {
    return game_ != nullptr &&
           game_->input().mouse_behavior() != engine_core::UserInputService::kMouseBehaviorDefault &&
           !ProfilerUi::get().shown();
}

void GameView::setPlayerView(bool player) {
    playerView_ = player;
    followCurrentCamera_ = player;
    refreshOverlays();
}

void GameView::refreshOverlays() {
    const bool editing = !playerView_ && !runner_->testing();
    // While the profiler shows here, the list and the eye are under it, so they hide.
    ProfilerUi& ui = ProfilerUi::get();
    if (ui.shown() && ui.owner == nullptr && getScene() != nullptr) {
        ui.owner = this;
    }
    const bool profiling = ui.shown() && ui.owner == this;
    profilerOverlay_->setVisible(profiling);
    cameraBox_->setVisible(editing && !profiling);
    guiToggle_->setVisible(editing && !profiling);
    guiScene_->setVisible(!editing || guiToggle_->isSelected());
    if (!editing && terrainBrush_->active()) {
        terrainBrush_->turnOff();
    }
    terrainPalette_->setVisible(editing && !profiling && terrainBrush_->active());
    if (!editing && brushTool_->active()) {
        brushTool_->turnOff();
    }
    brushPalette_->setVisible(editing && !profiling && brushTool_->active());
    brushPalette_->refresh();
}

BrushModifiers GameView::brushMods(int mods) const {
    BrushModifiers out;
    out.shift = (mods & 0x1) != 0;
    out.control = (mods & (0x2 | 0x8)) != 0;
    out.alt = (mods & 0x4) != 0;
    return out;
}

void GameView::syncBrushView() {
    engine_core::DraggerView view;
    view.camera = viewCamera_;
    view.fov_degrees = viewFov_;
    view.size = engine_core::Vec2{static_cast<float>(getWidth()), static_cast<float>(getHeight())};
    brushTool_->setView(view);
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

void GameView::reportViewportSize() {
    if (engine_ == nullptr || cameraId_ == 0) {
        return;
    }
    const double width = contentWidth();
    const double height = contentHeight();
    if (cameraId_ == sizedCamera_ && width == sizedWidth_ && height == sizedHeight_) {
        return;
    }
    sizedCamera_ = cameraId_;
    sizedWidth_ = width;
    sizedHeight_ = height;
    const engine_core::Vec2 size{static_cast<float>(width), static_cast<float>(height)};
    engine_->on_simulation([camera = cameraId_, size](engine_core::DataModel& game) {
        if (auto* target = dynamic_cast<engine_core::Camera*>(game.instance(camera))) {
            target->set_viewport_size(size);
        }
    });
}

void GameView::noteCurrentCamera(bool onlyIfNone) {
    if (engine_ == nullptr || cameraId_ == 0) {
        return;
    }
    // Two views may show one camera: the one pressed in gives it its size.
    sizedCamera_ = 0;
    reportViewportSize();
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
    // The profiler frees the pointer while it shows, so its graph can be clicked.
    const bool wanted = pointerWanted();
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
    if (!frameSnapshot_) {
        frameSnapshot_ = testSnapshot_ ? testSnapshot_ : feed_->hold();
    }
    const engine_core::VisualSnapshot& snapshot = *frameSnapshot_;
    // The folder the snapshot's paths are under, not the game's: a project
    // switch changes the game's before the snapshot catches up.
    meshes_.setRoot(snapshot.resources_root);
    textures_.setRoot(snapshot.resources_root);
    collectOutlines(snapshot);
    collectHandles(snapshot);
    // Each Prefab's meshes once, however many GameObjects draw it.
    if (prefabMeshes_.size() < snapshot.prefabs.size()) {
        prefabMeshes_.resize(snapshot.prefabs.size());
    }
    // Each Prefab Model its own slot, from 1, so every GameObject drawing it
    // shares one instanced call. Numbered again each frame, as the list is made again.
    std::uint32_t nextSlot = 1;
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
            draw.texture = textures_.get(source.diffuse_texture, source.diffuse_flip_y);
            draw.normalTexture = textures_.get(source.normal_texture, source.normal_flip_y);
            draw.roughnessTexture = textures_.get(source.roughness_texture, source.roughness_flip_y);
            draw.metalnessTexture = textures_.get(source.metalness_texture, source.metalness_flip_y);
            draw.emissiveTexture = textures_.get(source.emissive_texture, source.emissive_flip_y);
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
            draw.slot = nextSlot++;
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
            light.id = row.id;
            light.shadows = row.light.shadows;
            light.shadowDistance = row.light.shadow_distance;
            lightDraws_.push_back(light);
        }
        if (row.prefab == 0 || row.prefab >= snapshot.prefabs.size()) {
            continue;
        }
        // The GameObject's Color tints each Material's, per instance, and its opacity multiplies each Material's.
        const float opacity = 1.f - row.transparency;
        // Its Scale grows the Prefab about the GameObject's origin: each axis, not the position.
        engine_core::Matrix4 scaled = row.world;
        for (int column = 0; column < 3; ++column) {
            for (int axis = 0; axis < 3; ++axis) {
                scaled.m[column * 4 + axis] *= row.scale;
            }
        }
        for (const MeshDraw& model : prefabMeshes_[row.prefab]) {
            MeshDraw& draw = meshDraws_.emplace_back(model);
            draw.model = scaled;
            draw.owner = row.id;
            draw.tint[0] = row.color.r;
            draw.tint[1] = row.color.g;
            draw.tint[2] = row.color.b;
            draw.transparency = 1.f - (1.f - std::clamp(draw.transparency, 0.f, 1.f)) * opacity;
        }
    }
    // Each Terrain's LOD nodes chosen for this camera and pane (spec decision 2:
    // here, before Renderer::draw, which learns its target size only inside),
    // with its look; uploads not drawn for some seconds are deleted.
    TerrainCamera terrainCamera;
    if (viewFov_ > 0.f) {
        terrainCamera.world = viewCamera_;
        terrainCamera.fov_y_degrees = viewFov_;
    } else {
        // No Camera followed yet: the renderer's own.
        terrainCamera.world = engine_core::matrix4_inverse(renderer_.view());
        terrainCamera.fov_y_degrees = renderer_.fovYDegrees();
    }
    // In framebuffer pixels, as the error formula wants: getWidth() and
    // getHeight() are layout points, the renderer's last draw found how many
    // pixels each is (on a HiDPI display, more than 1).
    const float pixelsPerPoint = std::max(renderer_.pixelsPerPoint(), 0.01f);
    terrainCamera.pane_width = static_cast<int>(std::lround(getWidth() * pixelsPerPoint));
    terrainCamera.pane_height = static_cast<int>(std::lround(getHeight() * pixelsPerPoint));
    const double terrainNow =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - terrainClockStart_).count();
    AppendTerrainDraws(snapshot.terrains, terrainCamera, terrainNow, terrainFades_, meshes_, renderer_, meshDraws_);
    // Brushes: baked cells and single Brushes, one draw per Material.
    for (const engine_core::VisualBrushDraw& source : snapshot.brushes) {
        if (source.look.session == nullptr) {
            continue;
        }
        const anarchy::amesh::GpuMesh* mesh = meshes_.getBrush(source.look.revision, *source.look.session);
        if (mesh == nullptr || source.lod >= mesh->lod_count()) {
            continue;
        }
        const engine_core::VisualMesh& look = source.look;
        MeshDraw& draw = meshDraws_.emplace_back();
        draw.mesh = mesh;
        draw.model = source.world;
        draw.owner = source.owner;
        draw.texture = textures_.get(look.diffuse_texture, look.diffuse_flip_y);
        draw.normalTexture = textures_.get(look.normal_texture, look.normal_flip_y);
        draw.roughnessTexture = textures_.get(look.roughness_texture, look.roughness_flip_y);
        draw.metalnessTexture = textures_.get(look.metalness_texture, look.metalness_flip_y);
        draw.emissiveTexture = textures_.get(look.emissive_texture, look.emissive_flip_y);
        draw.color[0] = look.color.r;
        draw.color[1] = look.color.g;
        draw.color[2] = look.color.b;
        draw.color[3] = look.color.a;
        draw.emissive[0] = look.emissive.r;
        draw.emissive[1] = look.emissive.g;
        draw.emissive[2] = look.emissive.b;
        draw.metalness = look.metalness;
        draw.roughness = look.roughness;
        draw.reflectivity = look.reflectivity;
        draw.transparency = 1.f - (1.f - std::clamp(look.transparency, 0.f, 1.f)) * (1.f - source.transparency);
        draw.lod = static_cast<std::uint8_t>(std::min<std::uint32_t>(source.lod, 255));
        draw.castsShadow = source.casts_shadow;
        draw.slot = 0;
    }
    meshes_.sweepBrushes();
    SceneLighting lighting;
    lighting.ambient[0] = snapshot.lighting.ambient.r;
    lighting.ambient[1] = snapshot.lighting.ambient.g;
    lighting.ambient[2] = snapshot.lighting.ambient.b;
    lighting.exposure = snapshot.lighting.exposure;
    lighting.saturation = snapshot.lighting.saturation;
    lighting.gamma = snapshot.lighting.gamma;
    lighting.antialiasing = snapshot.lighting.antialiasing == 0 ? SceneAntialiasing::None : SceneAntialiasing::FXAA;
    lighting.terrainQuality = snapshot.lighting.terrain_quality == 0   ? SceneQuality::Low
                              : snapshot.lighting.terrain_quality == 2 ? SceneQuality::High
                                                                       : SceneQuality::Medium;
    // The Skybox's images, uploaded linear; a missing or unreadable one draws no sky.
    const engine_core::VisualSky& sky = snapshot.sky;
    if (sky.present) {
        const EnvironmentTexture image = textures_.getEnvironment(sky.image, sky.image_flip_y);
        lighting.sky.image = image.texture;
        lighting.sky.imageRevision = image.revision;
        lighting.sky.exposure = sky.exposure;
        lighting.sky.lightScale = sky.light_scale;
        lighting.sky.rotationDegrees = sky.rotation;
        lighting.sky.tint[0] = sky.tint.r;
        lighting.sky.tint[1] = sky.tint.g;
        lighting.sky.tint[2] = sky.tint.b;
    }
    // The DynamicSky, if it is the sky: SkyMath's sun, moon, and stars, the
    // clouds' drift by the view's clock, and its light first among the suns,
    // so it takes the shadow cascades.
    const engine_core::VisualDynamicSky& dynamic = snapshot.dynamic_sky;
    if (dynamic.present) {
        const SkyState state = ComputeSky(dynamic.time_of_day, dynamic.latitude, dynamic.brightness,
                                          dynamic.cloud_cover, dynamic.cloud_density);
        SceneDynamicSky& out = lighting.dynamicSky;
        out.enabled = true;
        SetSkyState(out, state);
        out.cloudCover = dynamic.cloud_cover;
        out.cloudDensity = dynamic.cloud_density;
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - skyClockStart_).count();
        // In double, wrapped, so the shader's floats stay precise: the clouds jump once in many hours.
        constexpr double kCloudOffsetWrap = 100000.0;
        out.cloudOffset[0] = static_cast<float>(std::fmod(static_cast<double>(dynamic.wind.x) * seconds, kCloudOffsetWrap));
        out.cloudOffset[1] = static_cast<float>(std::fmod(static_cast<double>(dynamic.wind.z) * seconds, kCloudOffsetWrap));
        out.windy = dynamic.wind.x != 0.f || dynamic.wind.z != 0.f;
        out.seconds = seconds;
        out.sunSizeDegrees = dynamic.sun_size;
        out.moonSizeDegrees = dynamic.moon_size;
        out.sunTexture = textures_.get(dynamic.sun_texture, dynamic.sun_flip_y);
        out.moonTexture = textures_.get(dynamic.moon_texture, dynamic.moon_flip_y);
        out.reflectionQuality = dynamic.reflection_quality == 0   ? SceneQuality::Low
                                : dynamic.reflection_quality == 2 ? SceneQuality::High
                                                                  : SceneQuality::Medium;
        out.key = {dynamic.time_of_day, dynamic.latitude, dynamic.cloud_cover, dynamic.cloud_density,
                   static_cast<int>(out.reflectionQuality)};
        if (state.light.intensity > 0.f) {
            lightDraws_.insert(lightDraws_.begin(), SkyLightDraw(state, dynamic.shadows));
        }
    }
    // The BloomEffect, if any; with none, or one turned off, there is no bloom.
    const engine_core::VisualBloom& bloom = snapshot.bloom;
    lighting.bloom.enabled = bloom.present && bloom.enabled;
    lighting.bloom.intensity = bloom.intensity;
    lighting.bloom.size = bloom.size;
    lighting.bloom.threshold = bloom.threshold;
    // The ScreenSpaceReflections, if any; with none, or one turned off, nothing is traced.
    const engine_core::VisualReflections& reflections = snapshot.reflections;
    lighting.reflections.enabled = reflections.present && reflections.enabled;
    lighting.reflections.intensity = reflections.intensity;
    lighting.reflections.maxDistance = reflections.max_distance;
    lighting.reflections.maxRoughness = reflections.max_roughness;
    // The AmbientOcclusionEffect, if any; with none, or one turned off, nothing is shaded.
    const engine_core::VisualAmbientOcclusion& occlusion = snapshot.occlusion;
    lighting.occlusion.enabled = occlusion.present && occlusion.enabled;
    lighting.occlusion.intensity = occlusion.intensity;
    lighting.occlusion.radius = occlusion.radius;
    lighting.occlusion.quality = occlusion.quality == 0   ? SceneQuality::Low
                                 : occlusion.quality == 2 ? SceneQuality::High
                                                          : SceneQuality::Medium;
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
    const double frame = elapsed / static_cast<double>(paintWindowFrames_);
    measuredFps_.store(FramesPerSecond(frame));
    measuredFrameMs_.store(frame * 1000.0);
    lastMeasured_ = now;
    paintWindowStart_ = now;
    paintWindowFrames_ = 0;
}

bool GameView::frameTimeCurrent() const {
    // Two windows without a paint: the view is hidden, or the studio stalled.
    constexpr double kStaleSeconds = 0.6;
    return paintWindowOpen_ && measuredFps_.load() > 0 &&
           std::chrono::duration<double>(std::chrono::steady_clock::now() - lastMeasured_).count() < kStaleSeconds;
}

void GameView::layoutChildren() {
    // Layout runs every frame, before the paint, so the list and the link are
    // current when the list lays out and when the paint follows the Camera.
    // One snapshot for this frame's layout and paint, so billboards sit where
    // the 3D draw puts what they float over, however fast the camera turns.
    frameSnapshot_ = testSnapshot_ ? testSnapshot_ : feed_->hold();
    refreshWorkspace();
    // After the link resolves, so a Camera linked this frame is seen from now.
    followCamera(*frameSnapshot_);
    refreshCameraList();
    refreshOverlays();
    guiLayer_->sync();
    BillboardView billboards;
    billboards.view = renderer_.view();
    billboards.fovYDegrees = renderer_.fovYDegrees();
    billboards.paneX = getAbsoluteX();
    billboards.paneY = getAbsoluteY();
    billboards.paneWidth = getWidth();
    billboards.paneHeight = getHeight();
    guiLayer_->placeBillboards(frameSnapshot_->billboards, billboards);
    StackPane::layoutChildren();
    // The GUIs cover the whole view, whatever they would rather be.
    guiScene_->performLayout(contentLeft(), contentTop(), contentWidth(), contentHeight());
    reportViewportSize();
    // The list sits in the top right corner, over the drawing.
    constexpr double kMargin = 6.0;
    constexpr double kListWidth = 160.0;
    const double width = std::max(0.0, std::min(kListWidth, contentWidth() - 2 * kMargin));
    const double height = cameraBox_->measuredHeight(width, contentHeight());
    const double listLeft = contentLeft() + contentWidth() - kMargin - width;
    cameraBox_->performLayout(listLeft, contentTop() + kMargin, width, height);
    if (profilerOverlay_->isVisible()) {
        profilerOverlay_->performLayout(contentLeft() + kMargin, contentTop() + kMargin,
                                        std::max(0.0, contentWidth() - 2 * kMargin),
                                        ProfilerOverlay::heightFor(contentHeight(), ProfilerUi::get().split));
    }
    // The eye sits just left of the list, as tall as it.
    constexpr double kGap = 4.0;
    const double eyeWidth = guiToggle_->measuredWidth(height);
    guiToggle_->performLayout(listLeft - kGap - eyeWidth, contentTop() + kMargin, eyeWidth, height);
    if (terrainPalette_->isVisible()) {
        constexpr double kPaletteWidth = 190.0;
        terrainPalette_->performLayout(contentLeft() + kMargin, contentTop() + kMargin, kPaletteWidth,
                                       terrainPalette_->measuredHeight(kPaletteWidth, contentHeight()));
        terrainBrush_->tick(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                            shiftHeld_);
    }
    if (brushPalette_->isVisible()) {
        constexpr double kPaletteWidth = 210.0;
        brushPalette_->refresh();
        brushPalette_->performLayout(contentLeft() + kMargin, contentTop() + kMargin, kPaletteWidth,
                                     brushPalette_->measuredHeight(kPaletteWidth, contentHeight()));
    }
    const bool measuring = brushTool_->active() && !brushTool_->readout().empty();
    brushReadout_->setVisible(measuring);
    if (measuring) {
        brushReadout_->setText(brushTool_->readout());
        const double w = brushReadout_->measuredWidth(24.0);
        const engine_core::Vec2 at = brushTool_->readoutAt();
        brushReadout_->performLayout(contentLeft() + at.x + 14.0, contentTop() + at.y - 28.0, w, 22.0);
    }
    // Each frame brings a new snapshot of the game, so the view lays out again next frame.
    markLayoutDirty(LayoutDirt::Arrange);
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
    if (followCurrentCamera_) {
        const auto* workspace =
            dynamic_cast<const engine_core::Workspace*>(game_->instance(game_->scene_service("Workspace")));
        const engine_core::InstanceId current = workspace != nullptr ? workspace->current_camera() : 0;
        for (const CameraChoice& choice : cameras_) {
            if (current != 0 && choice.id == current) {
                cameraGuid_ = choice.guid;
                break;
            }
        }
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
    readSelectedBodies();
}

void GameView::readSelectedBodies() {
    const engine_core::SelectionService& selection = game_->selection();
    if (selection.revision() != selectionSeen_) {
        selected_ = selection.get(selectionSeen_);
    }
    outlineScratch_.clear();
    for (const engine_core::InstanceId id : selected_) {
        if (const auto* controller = dynamic_cast<const engine_core::PlayerController*>(game_->instance(id))) {
            if (!game_->in_workspace(id)) {
                continue;
            }
            BodyOutline& outline = outlineScratch_.emplace_back();
            for (BodyOutline& kept : outlines_) {
                if (kept.id == id) {
                    outline = std::move(kept);
                    kept.id = 0;
                    break;
                }
            }
            // Its outline is made from Radius, Height, and the hover gap, kept in size.
            const engine_core::Vec3 size{static_cast<float>(controller->radius()),
                                         static_cast<float>(controller->height()),
                                         static_cast<float>(controller->hover_gap())};
            const bool made = outline.id == id;
            if (!made || outline.size.x != size.x || outline.size.y != size.y || outline.size.z != size.z) {
                outline.size = size;
                engine_core::PhysicsWorld::collision_outline(*controller, outline.lines);
            }
            outline.id = id;
            outline.shape = -1;
            outline.driven = controller->driven_game_object();
            const engine_core::GameObject* driven = outline.driven != 0 ? game_->game_object(outline.driven) : nullptr;
            outline.transform = driven != nullptr ? driven->transform() : controller->transform();
            continue;
        }
        if (const auto* brush = dynamic_cast<const engine_core::Brush*>(game_->instance(id))) {
            if (!game_->in_workspace(id)) {
                continue;
            }
            BodyOutline& outline = outlineScratch_.emplace_back();
            for (BodyOutline& kept : outlines_) {
                if (kept.id == id) {
                    outline = std::move(kept);
                    kept.id = 0;
                    break;
                }
            }
            // Its edges, exactly; made again only when its shape changes.
            if (outline.id != id || outline.meshRevision != brush->shape_revision()) {
                outline.meshRevision = brush->shape_revision();
                outline.lines.clear();
                const engine_core::brush::Shape& shape = brush->shape();
                for (const auto& [a, b] : shape.edges) {
                    const engine_core::brush::DVec3 p = shape.vertices[a];
                    const engine_core::brush::DVec3 q = shape.vertices[b];
                    outline.lines.push_back({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)});
                    outline.lines.push_back({static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z)});
                }
            }
            outline.id = id;
            outline.shape = -2;
            outline.driven = 0;
            outline.transform = brush->transform();
            continue;
        }
        const auto* body = dynamic_cast<const engine_core::PhysicsObject*>(game_->instance(id));
        if (body == nullptr || !game_->in_workspace(id)) {
            continue;
        }
        // Last read's outline, taken over, or a new one.
        BodyOutline& outline = outlineScratch_.emplace_back();
        for (BodyOutline& kept : outlines_) {
            if (kept.id == id) {
                outline = std::move(kept);
                kept.id = 0;
                break;
            }
        }
        const bool made = outline.id == id;
        outline.id = id;
        // Only a Hull or a Custom is made from its Mesh.
        const engine_core::PhysicsObject::Shape shape = body->shape();
        const bool meshed =
            shape == engine_core::PhysicsObject::Shape::Hull || shape == engine_core::PhysicsObject::Shape::Custom;
        const auto* mesh = meshed ? dynamic_cast<const engine_core::Mesh*>(game_->instance(body->mesh_id())) : nullptr;
        const engine_core::InstanceId meshId = mesh != nullptr ? mesh->id() : 0;
        static const std::string kNoPath;
        const std::string& meshPath = mesh != nullptr ? mesh->path() : kNoPath;
        const std::uint64_t meshRevision = mesh != nullptr ? mesh->session_geometry().revision : 0;
        const std::string meshStamp = mesh != nullptr ? mesh->file_stamp() : std::string();
        const bool meshChanged = !made || outline.mesh != meshId || outline.meshPath != meshPath ||
                                 outline.meshRevision != meshRevision || outline.meshStamp != meshStamp;
        if (meshChanged) {
            outline.mesh = meshId;
            outline.meshPath = meshPath;
            outline.meshRevision = meshRevision;
            outline.meshStamp = meshStamp;
            outline.meshPoints.clear();
            outline.meshTriangles.clear();
            // A Mesh with no points to read outlines as no Mesh: the Box it falls back to.
            if (mesh != nullptr && mesh->vertex_positions(outline.meshPoints, &outline.meshTriangles)) {
                outline.meshPoints.clear();
                outline.meshTriangles.clear();
            }
        }
        // An unanchored Custom draws its pieces when they are known; the outline never decomposes.
        // A Hull turned Custom on the same Mesh looks them up too.
        if (meshChanged || outline.shape != static_cast<int>(shape)) {
            outline.meshPieces.clear();
            if (mesh != nullptr && shape == engine_core::PhysicsObject::Shape::Custom && !outline.meshPoints.empty()) {
                engine_core::known_pieces(*mesh, outline.meshPoints, outline.meshTriangles, outline.meshPieces);
            }
        }
        // Its GameObject's scale multiplies its Size, as its body's shape is made.
        const engine_core::Vec3 scale = engine_core::PhysicsWorld::shape_scale(*game_, *body);
        const engine_core::Vec3 bodySize = body->size();
        const engine_core::Vec3 size{bodySize.x * scale.x, bodySize.y * scale.y, bodySize.z * scale.z};
        // Centered in its GameObject's Prefab, which changes as the Prefab's Models and Meshes do.
        const engine_core::Vec3 center = engine_core::PhysicsWorld::shape_center(*game_, *body);
        const auto same = [](engine_core::Vec3 a, engine_core::Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
        if (meshChanged || outline.shape != static_cast<int>(shape) || outline.anchored != body->anchored() ||
            !same(outline.size, size) || !same(outline.center, center)) {
            outline.shape = static_cast<int>(shape);
            outline.size = size;
            outline.anchored = body->anchored();
            outline.center = center;
            engine_core::PhysicsWorld::collision_outline(*body, center, outline.meshPoints, outline.meshTriangles,
                                                         outline.lines, scale, &outline.meshPieces);
        }
        // Its body starts at the GameObject it moves.
        outline.driven = body->driven_game_object();
        const engine_core::GameObject* driven = outline.driven != 0 ? game_->game_object(outline.driven) : nullptr;
        outline.transform = driven != nullptr ? driven->transform() : body->transform();
    }
    outlines_.swap(outlineScratch_);
}

void GameView::collectOutlines(const engine_core::VisualSnapshot& snapshot) {
    outlinePoints_.clear();
    for (const BodyOutline& outline : outlines_) {
        engine_core::Matrix4 world = outline.transform;
        // The GameObject's row is where its mesh is drawn this frame.
        if (outline.driven != 0) {
            for (const engine_core::VisualInstance& row : snapshot.instances) {
                if (row.id == outline.driven) {
                    if (row.alive) {
                        world = row.world;
                    }
                    break;
                }
            }
        }
        const engine_core::Matrix4 pose = engine_core::PhysicsWorld::body_pose(world);
        for (const engine_core::Vec3& point : outline.lines) {
            const engine_core::Vec3 placed = engine_core::matrix4_point(pose, point);
            outlinePoints_.insert(outlinePoints_.end(), {placed.x, placed.y, placed.z});
        }
    }
    terrainBrush_->appendOutline(outlinePoints_);
    toolLines_.clear();
    brushTool_->appendLines(toolLines_);
    renderer_.setToolLines(toolLines_.data(), static_cast<int>(toolLines_.size() / 7));
    renderer_.setOutlines(outlinePoints_.data(), static_cast<int>(outlinePoints_.size() / 3));
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

void GameView::collectHandles(const engine_core::VisualSnapshot& snapshot) {
    handleVertices_.clear();
    // Brush mode moves brushes itself; the Move tool's arrows would only be in the way.
    if (!brushTool_->active() && viewFov_ > 0.f && getWidth() > 0.0 && getHeight() > 0.0) {
        engine_core::DraggerView view;
        view.camera = viewCamera_;
        view.fov_degrees = viewFov_;
        view.size = engine_core::Vec2{static_cast<float>(getWidth()), static_cast<float>(getHeight())};
        for (const engine_core::VisualDragger& row : snapshot.draggers) {
            engine_core::handle_mesh(row.frame, view, row.hovered, row.active, handleScratch_);
            handleVertices_.insert(handleVertices_.end(), handleScratch_.begin(), handleScratch_.end());
        }
    }
    renderer_.setHandles(handleVertices_.data(), static_cast<int>(handleVertices_.size()));
}

void GameView::setSnapshotForTest(std::shared_ptr<const engine_core::VisualSnapshot> snapshot) {
    testSnapshot_ = std::move(snapshot);
}

void GameView::followCamera(const engine_core::VisualSnapshot& snapshot) {
    if (cameraId_ == 0) {
        return;
    }
    for (const engine_core::VisualInstance& row : snapshot.instances) {
        if (row.id == cameraId_) {
            if (row.alive && row.field_of_view > 0.f) {
                renderer_.setCamera(row.world, row.field_of_view);
                viewCamera_ = row.world;
                viewFov_ = row.field_of_view;
            }
            return;
        }
    }
}

void GameView::renderChildren(jadefx::UiRenderer&, float) {}

void GameView::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    // The window paints on this thread, the profiler's UI row.
    register_ui_thread();
    PROFILE_SCOPE("Scene View", profiler::Group::Render);
    notePaint();
    syncPointerLock();
    const jadefx::Scene* scene = getScene();
    if (scene != nullptr && scene->getWidth() > 0.0 && scene->getHeight() > 0.0 && getWidth() > 0.0 &&
        getHeight() > 0.0 && ensureGraphics()) {
        // The same color as the pane around the drawing, so no seam shows.
        const jadefx::Color& clear = computedStyle().background.color;
        renderer_.setClearColor(clear.r, clear.g, clear.b);
        renderer_.setGridVisible(runner_->sceneGrid());
        {
            PROFILE_SCOPE("Snapshot read", profiler::Group::Engine);
            collectMeshes();
        }
        // The scene depth under the mouse, so a billboard the scene hides takes no clicks.
        renderer_.setDepthProbe(cursorX_, cursorY_);
        const bool drawn = renderer_.draw(getAbsoluteX(), getAbsoluteY(), getWidth(), getHeight(), scene->getWidth(),
                                          scene->getHeight(), meshDraws_.data(), static_cast<int>(meshDraws_.size()),
                                          lightDraws_.data(), static_cast<int>(lightDraws_.size()));
        // ANARCHY_RENDER_STATS set prints the draw's counts once a second, for measuring culling and instancing.
        static const bool printStats = std::getenv("ANARCHY_RENDER_STATS") != nullptr;
        if (printStats && drawn) {
            const auto now = std::chrono::steady_clock::now();
            if (now - statsPrinted_ >= std::chrono::seconds(1)) {
                statsPrinted_ = now;
                const RenderStats& stats = renderer_.stats();
                std::fprintf(stderr, "render stats: %d draws, %d visible, %d culled, %d runs, %d instanced calls\n",
                             stats.draws, stats.visible, stats.culled, stats.runs, stats.instancedCalls);
            }
        }
        guiLayer_->setSceneDepth(renderer_.sceneDepth());
        guiLayer_->setCursorDepth(renderer_.probedDepth());
        // Read before the children paint, so the overlays are not in the picture.
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
    } else {
        // No scene drawn this paint: nothing for a billboard to hide behind, and no depth under the cursor.
        guiLayer_->setSceneDepth(SceneDepth{});
        guiLayer_->setCursorDepth(std::nullopt);
    }
    // What the threads recorded since the last paint, for the overlay drawn next.
    if (profiler::enabled()) {
        profiler::collect();
    }
    // Painted after the clear, so the label stays on top of the viewport.
    {
        PROFILE_SCOPE("GUIs and overlays", profiler::Group::Engine);
        Node::renderChildren(renderer, opacity);
    }
    // The render thread waits on this when it is uncapped, so its step follows
    // the paint instead of looping again as soon as the step itself returns.
    if (engine_ != nullptr) {
        engine_->note_client_frame();
        // This paint runs on the UI thread. The engine render thread is waiting
        // on the frame note above, so publishing analysis here is not RenderThread.
        engine_->analysis().pump();
    }
    // The frame is painted: let the snapshot go, so the feed can reuse its buffer.
    frameSnapshot_.reset();
}

void GameView::sceneChanged(jadefx::Scene* previous) {
    if (getScene() == nullptr && ProfilerUi::get().owner == this) {
        ProfilerUi::get().owner = nullptr;
    }
    if (hookedScene_ != nullptr && hookedScene_ != getScene()) {
        if (keyHook_ != 0 && !hookedScene_->isTearingDown()) {
            hookedScene_->removeKeyHook(keyHook_);
        }
        hookedScene_ = nullptr;
        keyHook_ = 0;
    }
    // A game has no menu bar to hold the profiler's keys, so its view takes them.
    if (playerView_ && getScene() != nullptr && hookedScene_ == nullptr) {
        hookedScene_ = getScene();
        keyHook_ = hookedScene_->addKeyHook([](jadefx::KeyEvent& key) {
            if (!key.pressed || key.repeat || !key.shortcut() || key.shift || key.alt) {
                return;
            }
            if (key.key == jadefx::Key::F6) {
                ProfilerUi::get().toggleShown();
                key.consume();
            } else if (key.key == jadefx::Key::P && ProfilerUi::get().shown()) {
                ProfilerUi::get().togglePaused();
                key.consume();
            }
        });
    }
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

std::optional<engine_core::DraggerRay> GameView::rayAt(double x, double y) const {
    if (viewFov_ <= 0.f || getWidth() <= 0.0 || getHeight() <= 0.0) {
        return std::nullopt;
    }
    engine_core::DraggerView view;
    view.camera = viewCamera_;
    view.fov_degrees = viewFov_;
    view.size = engine_core::Vec2{static_cast<float>(getWidth()), static_cast<float>(getHeight())};
    return engine_core::viewport_ray(view, engine_core::Vec2{localX(x), localY(y)});
}

float GameView::localX(double x) const { return static_cast<float>(x - getAbsoluteX()); }

float GameView::localY(double y) const { return static_cast<float>(y - getAbsoluteY()); }

void GameView::handleMousePressed(const jadefx::MouseEvent& event) {
    // Keys go to the focused node, so a click is how a player gives the game the keyboard.
    requestFocus();
    ProfilerUi::get().owner = this;
    noteCurrentCamera();
    // Terrain mode keeps the left button for its brush; the rest (the
    // camera's right button) still reaches the game.
    if (terrainBrush_->active() && event.button == 0) {
        shiftHeld_ = event.shift();
        if (const auto ray = rayAt(event.x, event.y)) {
            terrainBrush_->press(*ray, event.shift());
        }
        IdePane::handleMousePressed(event);
        return;
    }
    if (brushTool_->active() && event.button == 0) {
        syncBrushView();
        if (const auto ray = rayAt(event.x, event.y)) {
            brushTool_->press(*ray, brushMods(event.mods), event.clickCount);
        }
        IdePane::handleMousePressed(event);
        return;
    }
    if (game_ != nullptr) {
        game_->input().post_mouse_button(event.button, true, localX(event.x), localY(event.y));
    }
    IdePane::handleMousePressed(event);
}

void GameView::handleMouseReleased(const jadefx::MouseEvent& event) {
    if (terrainBrush_->active() && event.button == 0) {
        terrainBrush_->release();
        IdePane::handleMouseReleased(event);
        return;
    }
    if (brushTool_->active() && event.button == 0) {
        brushTool_->release(brushMods(event.mods));
        IdePane::handleMouseReleased(event);
        return;
    }
    if (game_ != nullptr) {
        game_->input().post_mouse_button(event.button, false, localX(event.x), localY(event.y));
    }
    IdePane::handleMouseReleased(event);
}

void GameView::handleMouseDragged(const jadefx::MouseEvent& event) {
    cursorX_ = event.x;
    cursorY_ = event.y;
    if (terrainBrush_->active()) {
        shiftHeld_ = event.shift();
        terrainBrush_->hover(rayAt(event.x, event.y));
    }
    if (brushTool_->active() && !pointerWanted()) {
        syncBrushView();
        brushTool_->hover(rayAt(event.x, event.y), brushMods(event.mods));
    }
    if (game_ != nullptr) {
        game_->input().post_mouse_move(localX(event.x), localY(event.y));
    }
    IdePane::handleMouseDragged(event);
}

void GameView::handleMouseMoved(const jadefx::MouseEvent& event) {
    cursorX_ = event.x;
    cursorY_ = event.y;
    if (terrainBrush_->active()) {
        terrainBrush_->hover(rayAt(event.x, event.y));
    }
    if (brushTool_->active() && !pointerWanted()) {
        syncBrushView();
        brushTool_->hover(rayAt(event.x, event.y), brushMods(event.mods));
    }
    if (game_ != nullptr) {
        game_->input().post_mouse_move(localX(event.x), localY(event.y));
    }
    IdePane::handleMouseMoved(event);
}

void GameView::handleHoverChanged() {
    if (!isHovered()) {
        cursorX_ = -1;
        cursorY_ = -1;
    }
    IdePane::handleHoverChanged();
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
    if (event.key == jadefx::Key::LeftShift || event.key == jadefx::Key::RightShift) {
        shiftHeld_ = event.pressed;
    }
    // Terrain mode: T turns it on and off (edit mode only).
    const bool editing = !playerView_ && !runner_->testing();
    if (editing && event.pressed && !event.repeat && !event.alt && !event.meta) {
        if (event.key == jadefx::Key::B && !event.control && !event.shift && !pointerWanted()) {
            if (terrainBrush_->active()) {
                terrainBrush_->turnOff();
            }
            brushTool_->toggle();
            refreshOverlays();
            event.consume();
            return;
        }
        if (event.key == jadefx::Key::T && !event.control && !event.shift) {
            if (brushTool_->active()) {
                brushTool_->turnOff();
            }
            terrainBrush_->toggle();
            terrainPalette_->refresh();
            refreshOverlays();
            event.consume();
            return;
        }
    }
    // Brush mode's keys, except while the camera flies (the right button holds the pointer).
    if (editing && brushTool_->active() && !pointerWanted() && brushTool_->key(event)) {
        brushPalette_->refresh();
        event.consume();
        return;
    }
    // A held key repeats. InputBegan fires once, on the first press.
    if (game_ != nullptr && !event.repeat) {
        const int key = engine_core::UserInputService::key_code_from_glfw(event.key);
        game_->input().post_key(key, event.pressed);
    }
    IdePane::handleKey(event);
}

void GameView::handleFocusGained() {
    // The profiler follows the Scene View last given the keyboard.
    ProfilerUi::get().owner = this;
    IdePane::handleFocusGained();
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
