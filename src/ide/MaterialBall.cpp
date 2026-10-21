#include "MaterialBall.hpp"

#include "Matrix4.hpp"
#include "MeshShapes.hpp"
#include "runner/gl.hpp"

// Only GLFW's proc lookup: the GL names come from runner/gl.hpp.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

namespace ide {

using engine_core::texture::Usage;

namespace {

constexpr float kFovYDegrees = 30.f;
constexpr int kSegments = 64;
// How much of the picture's half width the ball spans.
constexpr double kFill = 0.92;
constexpr float kPi = 3.14159265358979f;

// How far from a unit ball the camera stands for it to span kFill of the view.
float CameraDistance() {
    const double half = kFovYDegrees * 0.5 * kPi / 180.0;
    return static_cast<float>(1.0 / std::sin(std::atan(kFill * std::tan(half))));
}

}  // namespace

MaterialBall::MaterialBall(int render_size, int size) : render_size_(render_size), size_(size) {}

MaterialBall::~MaterialBall() {
    // Without GL: the renderer's and the framebuffer's names go with their context.
    sphere_.forget();
    cube_.forget();
    plane_.forget();
}

void MaterialBall::setRoot(const std::filesystem::path& root) {
    textures_.setRoot(root);
    meshes_.setRoot(root);
}

bool MaterialBall::ensureGraphics() {
    if (attempted_) {
        return ready_;
    }
    attempted_ = true;
    const bool loaded = runner::LoadGl([](const char* name) -> void* {
        return reinterpret_cast<void*>(glfwGetProcAddress(name));
    });
    if (!loaded || !renderer_.initialize()) {
        return false;
    }
    // Lit as a new place's Lighting lights it, the renderer's default.
    renderer_.setCamera(engine_core::matrix4_translation(0.f, 0.f, CameraDistance()), kFovYDegrees);
    renderer_.setClearColor(0.12f, 0.12f, 0.12f);

    anarchy::amesh::Data data;
    engine_core::add_sphere(data, 1.f, kSegments, engine_core::Vec3{0.f, 0.f, 0.f});
    sphere_.upload(data);
    anarchy::amesh::Data box;
    engine_core::add_box(box, engine_core::Vec3{1.25f, 1.25f, 1.25f}, engine_core::Vec3{0.f, 0.f, 0.f});
    cube_.upload(box);
    anarchy::amesh::Data plane;
    engine_core::add_plane(plane, 2.f, 2.f, engine_core::Vec3{0.f, 0.f, 0.f});
    plane_.upload(plane);

    runner::GLint previous = 0;
    glGetIntegerv(runner::RT_GL_FRAMEBUFFER_BINDING, &previous);
    glGenTextures(1, &color_);
    glBindTexture(runner::GL_TEXTURE_2D, color_);
    glTexImage2D(runner::GL_TEXTURE_2D, 0, static_cast<runner::GLint>(runner::GL_RGBA8), render_size_, render_size_,
                 0, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(runner::GL_TEXTURE_2D, runner::GL_TEXTURE_MIN_FILTER, static_cast<runner::GLint>(runner::RT_GL_NEAREST));
    glTexParameteri(runner::GL_TEXTURE_2D, runner::GL_TEXTURE_MAG_FILTER, static_cast<runner::GLint>(runner::RT_GL_NEAREST));
    glBindTexture(runner::GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &framebuffer_);
    glBindFramebuffer(runner::RT_GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(runner::RT_GL_FRAMEBUFFER, runner::RT_GL_COLOR_ATTACHMENT0, runner::GL_TEXTURE_2D, color_, 0);
    ready_ = glCheckFramebufferStatus(runner::RT_GL_FRAMEBUFFER) == runner::RT_GL_FRAMEBUFFER_COMPLETE &&
             sphere_.valid();
    glBindFramebuffer(runner::RT_GL_FRAMEBUFFER, static_cast<runner::GLuint>(previous));
    return ready_;
}

bool MaterialBall::draw(const MaterialLook& look, runner::ViewPixels& out) {
    out = runner::ViewPixels{};
    if (!ensureGraphics()) {
        return true;
    }
    runner::ViewPixels picture;
    if (!render(look, sphere_, engine_core::matrix4_identity(), picture)) {
        return false;
    }
    // Just inside the true circle: the sphere's facets stop short of it by a sliver.
    out = cut_ball(picture, kFill * std::cos(kPi / kSegments), size_);
    return true;
}

bool MaterialBall::drawView(const MaterialLook& look, runner::ViewPixels& out) {
    out = runner::ViewPixels{};
    if (!ensureGraphics()) {
        return true;
    }
    const anarchy::amesh::GpuMesh* mesh = &sphere_;
    engine_core::Matrix4 base = engine_core::matrix4_identity();
    if (mesh_ == PreviewMesh::Cube) {
        mesh = &cube_;
    } else if (mesh_ == PreviewMesh::Plane) {
        // Facing up as built: stood up to face the camera.
        mesh = &plane_;
        base = engine_core::matrix4_axis_angle(engine_core::Vec3{1.f, 0.f, 0.f}, kPi * 0.5);
    } else if (mesh_ == PreviewMesh::Mesh) {
        const anarchy::amesh::GpuMesh* file = mesh_path_.empty() ? nullptr : meshes_.get(mesh_path_);
        if (file != nullptr && file->valid()) {
            // Centered and scaled to the ball's radius.
            const float* low = file->bounds_min();
            const float* high = file->bounds_max();
            const float extent = std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]}) * 0.5f;
            const float scale = extent > 0.f ? 1.f / extent : 1.f;
            base = engine_core::matrix4_identity();
            base.m[0] = base.m[5] = base.m[10] = scale;
            base.m[12] = -(low[0] + high[0]) * 0.5f * scale;
            base.m[13] = -(low[1] + high[1]) * 0.5f * scale;
            base.m[14] = -(low[2] + high[2]) * 0.5f * scale;
            mesh = file;
        }
    }
    const engine_core::Matrix4 turn =
        engine_core::matrix4_multiply(engine_core::matrix4_axis_angle(engine_core::Vec3{0.f, 1.f, 0.f}, yaw_),
                                      engine_core::matrix4_axis_angle(engine_core::Vec3{1.f, 0.f, 0.f}, pitch_));
    runner::ViewPixels picture;
    if (!render(look, *mesh, engine_core::matrix4_multiply(turn, base), picture)) {
        return false;
    }
    out = runner::FitWithin(picture, size_);
    return true;
}

bool MaterialBall::render(const MaterialLook& look, const anarchy::amesh::GpuMesh& mesh,
                          const engine_core::Matrix4& model, runner::ViewPixels& render) {
    // A preview is drawn once, not every frame, so it waits for its textures
    // whole (never blurry) and asks to be drawn again until they are.
    textures_.pump();
    runner::MeshDraw ball;
    ball.mesh = &mesh;
    ball.model = model;
    ball.texture = textures_.get(look.diffuse_texture, Usage::Color, look.diffuse_flip_y, true);
    ball.normalTexture = textures_.get(look.normal_texture, Usage::Normal, look.normal_flip_y, true);
    ball.roughnessTexture = textures_.get(look.roughness_texture, Usage::Mask, look.roughness_flip_y, true);
    ball.metalnessTexture = textures_.get(look.metalness_texture, Usage::Mask, look.metalness_flip_y, true);
    ball.emissiveTexture = textures_.get(look.emissive_texture, Usage::Color, look.emissive_flip_y, true);
    // The Material's Shader, its samplers loaded as their hints say, as a scene's draws are.
    ball.surface = look.surface;
    if (look.surface != nullptr && look.surface->parse != nullptr) {
        std::size_t sampler = 0;
        for (const engine_core::ShaderParam& param : look.surface->parse->params) {
            if (param.type != engine_core::ShaderParamType::Sampler) {
                continue;
            }
            if (sampler >= std::size(ball.surfaceTextures) || sampler >= look.surface->samplers.size()) {
                break;
            }
            const engine_core::SurfaceBinding::Sampler& bound = look.surface->samplers[sampler];
            const Usage usage = param.srgb ? Usage::Color : param.normal ? Usage::Normal : Usage::Mask;
            ball.surfaceTextures[sampler] = textures_.get(bound.path, usage, bound.flip_y, true);
            ++sampler;
        }
    }
    if (textures_.loading()) {
        return false;
    }
    ball.color[0] = look.color.r;
    ball.color[1] = look.color.g;
    ball.color[2] = look.color.b;
    ball.color[3] = look.color.a;
    ball.emissive[0] = look.emissive.r;
    ball.emissive[1] = look.emissive.g;
    ball.emissive[2] = look.emissive.b;
    ball.metalness = look.metalness;
    ball.roughness = look.roughness;
    ball.reflectivity = look.reflectivity;
    ball.transparency = look.transparency;

    // Light travels along direction: the key down and away from the upper left, the fill from the right.
    runner::LightDraw lights[2];
    lights[0].kind = runner::LightDraw::Kind::Directional;
    lights[0].direction[0] = 0.6f;
    lights[0].direction[1] = -0.7f;
    lights[0].direction[2] = -0.5f;
    lights[0].color[0] = 1.f;
    lights[0].color[1] = 0.96f;
    lights[0].color[2] = 0.9f;
    lights[0].intensity = 1.6f;
    lights[1].kind = runner::LightDraw::Kind::Directional;
    lights[1].direction[0] = -0.8f;
    lights[1].direction[1] = -0.1f;
    lights[1].direction[2] = -0.4f;
    lights[1].color[0] = 0.7f;
    lights[1].color[1] = 0.8f;
    lights[1].color[2] = 1.f;
    lights[1].intensity = 0.35f;

    runner::GLint framebuffer = 0;
    runner::GLint viewport[4] = {};
    glGetIntegerv(runner::RT_GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(runner::GL_VIEWPORT, viewport);
    const bool scissor = glIsEnabled(runner::GL_SCISSOR_TEST) == runner::GL_TRUE;
    glBindFramebuffer(runner::RT_GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, render_size_, render_size_);
    glDisable(runner::GL_SCISSOR_TEST);
    const double side = render_size_;
    const bool drawn = renderer_.draw(0, 0, side, side, side, side, &ball, 1, lights, 2);
    if (drawn) {
        renderer_.read(0, 0, side, side, side, side, render);
    }
    glBindFramebuffer(runner::RT_GL_FRAMEBUFFER, static_cast<runner::GLuint>(framebuffer));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (scissor) {
        glEnable(runner::GL_SCISSOR_TEST);
    }
    return drawn;
}

void MaterialBall::release() {
    renderer_.shutdown();
    textures_.clear();
    sphere_.destroy();
    cube_.destroy();
    plane_.destroy();
    meshes_.clear();
    if (framebuffer_ != 0) {
        glDeleteFramebuffers(1, &framebuffer_);
        framebuffer_ = 0;
    }
    if (color_ != 0) {
        glDeleteTextures(1, &color_);
        color_ = 0;
    }
    attempted_ = false;
    ready_ = false;
}

}  // namespace ide
