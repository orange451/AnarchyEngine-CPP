#include "ViewportRenderer.hpp"

#include "SceneDraws.hpp"
#include "gl.hpp"
#include "profiler/Profiler.hpp"

#include "jadefx/jadefx.hpp"

// Only GLFW's proc lookup: the GL names come from gl.hpp.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>

namespace runner {

namespace {

// The pooled targets of one pane Renderer: enough for a few sizes of panes in turn.
constexpr std::size_t kTargetPool = std::size_t(64) << 20;
// Skies whose cubes stay made at once.
constexpr int kSkyCache = 3;

// The framebuffer, viewport, and scissor a pane render changes, put back after.
struct SavedTarget {
    GLint framebuffer = 0;
    GLint viewport[4] = {};
    bool scissor = false;

    SavedTarget() {
        glGetIntegerv(RT_GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport);
        scissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    }
    ~SavedTarget() {
        glBindFramebuffer(RT_GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        if (scissor) {
            glEnable(GL_SCISSOR_TEST);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    }
    SavedTarget(const SavedTarget&) = delete;
    SavedTarget& operator=(const SavedTarget&) = delete;
};

}  // namespace

ViewportRenderer::ViewportRenderer(MeshCache* meshes, TextureCache* textures)
    : ownMeshes_(meshes == nullptr ? std::make_unique<MeshCache>() : nullptr),
      ownTextures_(textures == nullptr ? std::make_unique<TextureCache>() : nullptr),
      meshes_(meshes != nullptr ? meshes : ownMeshes_.get()),
      textures_(textures != nullptr ? textures : ownTextures_.get()) {}

ViewportRenderer::~ViewportRenderer() = default;

void ViewportRenderer::setScene(engine_core::InstanceId pane,
                                std::shared_ptr<const engine_core::VisualViewport> scene) {
    queue_.setScene(pane, std::move(scene));
}

void ViewportRenderer::setResourcesRoot(const std::filesystem::path& root) {
    if (ownMeshes_) {
        ownMeshes_->setRoot(root);
    }
    if (ownTextures_) {
        ownTextures_->setRoot(root);
    }
}

bool ViewportRenderer::ensureGraphics() {
    if (attempted_) {
        return ready_;
    }
    attempted_ = true;
    const bool loaded = LoadGl([](const char* name) -> void* { return reinterpret_cast<void*>(glfwGetProcAddress(name)); });
    ready_ = loaded && renderer_.initialize();
    if (ready_) {
        renderer_.setTransparentOutput(true);
        ShadowSettings noShadows;
        noShadows.enabled = false;
        renderer_.setShadowSettings(noShadows);
        renderer_.setTargetPool(kTargetPool);
        renderer_.setSkyCacheSize(kSkyCache);
    }
    return ready_;
}

ViewportRenderer::Target* ViewportRenderer::ensureTarget(engine_core::InstanceId pane, int width, int height) {
    Target& target = targets_[pane];
    if (target.framebuffer != 0 && target.width == width && target.height == height) {
        return &target;
    }
    deleteTarget(target);
    glGenTextures(1, &target.color);
    glBindTexture(GL_TEXTURE_2D, target.color);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    // Composited scaled to the pane, so filtered.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.color, 0);
    const bool complete = glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) == RT_GL_FRAMEBUFFER_COMPLETE;
    target.width = width;
    target.height = height;
    if (!complete) {
        deleteTarget(target);
        return nullptr;
    }
    return &target;
}

void ViewportRenderer::deleteTarget(Target& target) {
    if (target.framebuffer != 0) {
        glDeleteFramebuffers(1, &target.framebuffer);
        target.framebuffer = 0;
    }
    if (target.color != 0) {
        glDeleteTextures(1, &target.color);
        target.color = 0;
    }
    target.width = 0;
    target.height = 0;
}

ViewportQueue::Result ViewportRenderer::renderNow(const ViewportQueue::Job& job) {
    if (job.scene == nullptr || job.width <= 0 || job.height <= 0 || !ensureGraphics()) {
        return ViewportQueue::Result::NotReady;
    }
    const engine_core::VisualViewport& scene = *job.scene;
    SavedTarget saved;
    Target* target = ensureTarget(job.pane, job.width, job.height);
    if (target == nullptr) {
        return ViewportQueue::Result::NotReady;
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, target->framebuffer);
    glViewport(0, 0, job.width, job.height);
    glDisable(GL_SCISSOR_TEST);
    if (!scene.has_camera) {
        // With no Camera the pane shows only its background and children.
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        return ViewportQueue::Result::Complete;
    }

    // The pane's Prefabs, each a slot of its own from 1, then its rows.
    if (prefabDraws_.size() < scene.prefabs.size()) {
        prefabDraws_.resize(scene.prefabs.size());
    }
    std::uint32_t nextSlot = 1;
    for (std::size_t index = 0; index < scene.prefabs.size(); ++index) {
        prefabDraws_[index].clear();
        if (scene.prefabs[index] != nullptr) {
            LoadPrefabDraws(*scene.prefabs[index], *meshes_, *textures_, nextSlot, prefabDraws_[index]);
        }
    }
    draws_.clear();
    lights_.clear();
    for (const engine_core::VisualInstance& row : scene.rows) {
        LightDraw light;
        if (LightDrawFor(row, light)) {
            // A pane's lights cast no shadows.
            light.shadows = false;
            lights_.push_back(light);
        }
        if (row.prefab != 0 && row.prefab < scene.prefabs.size()) {
            AppendRowDraws(row, prefabDraws_[row.prefab], draws_);
        }
    }
    // A new Lighting's tone settings, the pane's Ambient, and its own sky.
    SceneLighting lighting;
    lighting.ambient[0] = scene.ambient.r;
    lighting.ambient[1] = scene.ambient.g;
    lighting.ambient[2] = scene.ambient.b;
    SkyFor(scene.sky, *textures_, lighting.sky);
    renderer_.setLighting(lighting);
    renderer_.setCamera(scene.camera_world, scene.field_of_view);
    const double width = job.width;
    const double height = job.height;
    const bool drawn = renderer_.draw(0, 0, width, height, width, height, draws_.empty() ? nullptr : draws_.data(),
                                      static_cast<int>(draws_.size()), lights_.empty() ? nullptr : lights_.data(),
                                      static_cast<int>(lights_.size()));
    if (!drawn) {
        return ViewportQueue::Result::NotReady;
    }
    // Drawn, but blurry or skyless while something still loads: drawn again next frame.
    const bool skyMissing = scene.sky.present && !scene.sky.image.empty() && lighting.sky.image == 0;
    if (textures_->loading() || skyMissing) {
        return ViewportQueue::Result::Incomplete;
    }
    return ViewportQueue::Result::Complete;
}

void ViewportRenderer::composite(jadefx::UiRenderer& ui, engine_core::InstanceId pane, double x, double y,
                                 double width, double height, float opacity) {
    jadefx::Painter painter(ui);
    const float scale = painter.pixelsPerPoint();
    queue_.shown(pane, ViewportPixels(width, scale), ViewportPixels(height, scale));
    int drawnWidth = 0;
    int drawnHeight = 0;
    const auto found = targets_.find(pane);
    if (found == targets_.end() || found->second.color == 0 || !queue_.image(pane, drawnWidth, drawnHeight)) {
        return;
    }
    painter.drawTexture(found->second.color, static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                        static_cast<float>(height), opacity, jadefx::TextureAlpha::Premultiplied, true);
}

void ViewportRenderer::renderPending() {
    PROFILE_SCOPE("Viewport panes", profiler::Group::Render);
    if (!ensureGraphics()) {
        return;
    }
    if (ownTextures_) {
        // What finished loading since last frame goes up before anything draws.
        ownTextures_->pump();
    }
    queue_.renderPending([this](const ViewportQueue::Job& job) { return renderNow(job); });
    queue_.takeForgotten(forgotten_);
    for (const engine_core::InstanceId pane : forgotten_) {
        const auto found = targets_.find(pane);
        if (found != targets_.end()) {
            deleteTarget(found->second);
            targets_.erase(found);
        }
    }
    forgotten_.clear();
}

void ViewportRenderer::release() {
    queue_.clear();
    queue_.takeForgotten(forgotten_);
    forgotten_.clear();
    for (auto& entry : targets_) {
        deleteTarget(entry.second);
    }
    targets_.clear();
    renderer_.shutdown();
    if (ownMeshes_) {
        ownMeshes_->clear();
    }
    if (ownTextures_) {
        ownTextures_->clear();
    }
    attempted_ = false;
    ready_ = false;
}

bool ViewportRenderer::read(engine_core::InstanceId pane, ViewPixels& out) {
    out = ViewPixels{};
    const auto found = targets_.find(pane);
    if (found == targets_.end() || found->second.framebuffer == 0) {
        return false;
    }
    const Target& target = found->second;
    SavedTarget saved;
    glBindFramebuffer(RT_GL_FRAMEBUFFER, target.framebuffer);
    std::vector<unsigned char> bottomUp(static_cast<std::size_t>(target.width) * target.height * 4);
    glReadPixels(0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
    out.width = target.width;
    out.height = target.height;
    out.rgba.resize(bottomUp.size());
    const std::size_t row = static_cast<std::size_t>(target.width) * 4;
    for (int y = 0; y < target.height; ++y) {
        std::copy_n(bottomUp.data() + row * static_cast<std::size_t>(target.height - 1 - y), row,
                    out.rgba.data() + row * static_cast<std::size_t>(y));
    }
    return true;
}

}  // namespace runner
