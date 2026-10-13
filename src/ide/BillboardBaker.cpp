#include "BillboardBaker.hpp"

#include "Matrix4.hpp"
#include "runner/RenderMath.hpp"
#include "runner/SceneDraws.hpp"
#include "runner/gl.hpp"

// Only GLFW's proc lookup: the GL names come from runner/gl.hpp.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

namespace ide {

namespace {

// The card's lens: narrow, so a tree's near and far branches are nearly one size.
constexpr float kLensDegrees = 2.f;
// Past this the camera would put the far side beyond the renderer's far plane.
constexpr float kMostDistance = 3000.f;
constexpr float kPi = 3.14159265358979f;

std::uint32_t fnv1a(const std::string& bytes) {
    std::uint32_t hash = 2166136261u;
    for (const char c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return hash;
}

}  // namespace

CardFrame card_frame(const engine_core::Vec3& low, const engine_core::Vec3& high) {
    const float reach = std::max({std::abs(low.x), std::abs(high.x), std::abs(low.z), std::abs(high.z)});
    CardFrame frame;
    frame.width = 2.f * reach;
    frame.height = std::max(high.y, 0.f);
    return frame;
}

runner::ViewPixels shrink_card(const runner::ViewPixels& render) {
    runner::ViewPixels out;
    if (render.empty()) {
        return out;
    }
    out.width = std::max(1, render.width / 2);
    out.height = std::max(1, render.height / 2);
    const std::size_t pixels = static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height);
    out.rgba.assign(pixels * 4, 0);
    std::vector<char> drawn(pixels, 0);
    std::deque<std::size_t> reached;
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            float weight = 0.f;
            float color[3] = {0.f, 0.f, 0.f};
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(x * 2 + dx, render.width - 1);
                    const int sy = std::min(y * 2 + dy, render.height - 1);
                    const unsigned char* p =
                        render.rgba.data() +
                        (static_cast<std::size_t>(sy) * static_cast<std::size_t>(render.width) + static_cast<std::size_t>(sx)) * 4;
                    const float alpha = p[3] / 255.f;
                    weight += alpha;
                    for (int c = 0; c < 3; ++c) {
                        color[c] += p[c] * alpha;
                    }
                }
            }
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) + static_cast<std::size_t>(x);
            unsigned char* q = out.rgba.data() + at * 4;
            if (weight > 0.f) {
                for (int c = 0; c < 3; ++c) {
                    q[c] = static_cast<unsigned char>(std::lround(std::clamp(color[c] / weight, 0.f, 255.f)));
                }
                q[3] = static_cast<unsigned char>(std::lround(std::clamp(weight / 4.f * 255.f, 0.f, 255.f)));
                drawn[at] = 1;
                reached.push_back(at);
            }
        }
    }
    // Outward from every drawn pixel, a step a pixel: each clear one takes the
    // color of the first drawn one to reach it, and stays clear.
    while (!reached.empty()) {
        const std::size_t at = reached.front();
        reached.pop_front();
        const int x = static_cast<int>(at % static_cast<std::size_t>(out.width));
        const int y = static_cast<int>(at / static_cast<std::size_t>(out.width));
        const int around[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto& next : around) {
            if (next[0] < 0 || next[1] < 0 || next[0] >= out.width || next[1] >= out.height) {
                continue;
            }
            const std::size_t to =
                static_cast<std::size_t>(next[1]) * static_cast<std::size_t>(out.width) + static_cast<std::size_t>(next[0]);
            if (drawn[to] != 0) {
                continue;
            }
            drawn[to] = 1;
            std::copy_n(out.rgba.data() + at * 4, 3, out.rgba.data() + to * 4);
            reached.push_back(to);
        }
    }
    return out;
}

std::string card_file(const std::string& prefab_name, const std::string& png) {
    std::string name;
    for (const char c : prefab_name) {
        const bool kept = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                          c == '_';
        name += kept ? c : '_';
    }
    if (name.empty()) {
        name = "Prefab";
    }
    char hash[9];
    std::snprintf(hash, sizeof(hash), "%08x", static_cast<unsigned>(fnv1a(png)));
    return "textures/billboards/" + name + "." + hash + ".png";
}

BillboardBaker::BillboardBaker(int height) : height_(std::max(16, height)) {}

BillboardBaker::~BillboardBaker() = default;

void BillboardBaker::setRoot(const std::filesystem::path& root) {
    meshes_.setRoot(root);
    textures_.setRoot(root);
}

bool BillboardBaker::ensureGraphics() {
    if (attempted_) {
        return ready_;
    }
    attempted_ = true;
    const bool loaded = runner::LoadGl([](const char* name) -> void* {
        return reinterpret_cast<void*>(glfwGetProcAddress(name));
    });
    ready_ = loaded && renderer_.initialize();
    if (ready_) {
        // Only the G-buffer is read: no shadows to make for it.
        runner::ShadowSettings none;
        none.enabled = false;
        renderer_.setShadowSettings(none);
    }
    return ready_;
}

bool BillboardBaker::ensureTarget(int width, int height) {
    if (framebuffer_ != 0 && target_width_ == width && target_height_ == height) {
        return true;
    }
    if (framebuffer_ != 0) {
        glDeleteFramebuffers(1, &framebuffer_);
        framebuffer_ = 0;
    }
    if (color_ != 0) {
        glDeleteTextures(1, &color_);
        color_ = 0;
    }
    glGenTextures(1, &color_);
    glBindTexture(runner::GL_TEXTURE_2D, color_);
    glTexImage2D(runner::GL_TEXTURE_2D, 0, static_cast<runner::GLint>(runner::GL_RGBA8), width, height, 0,
                 runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(runner::GL_TEXTURE_2D, runner::GL_TEXTURE_MIN_FILTER, static_cast<runner::GLint>(runner::RT_GL_NEAREST));
    glTexParameteri(runner::GL_TEXTURE_2D, runner::GL_TEXTURE_MAG_FILTER, static_cast<runner::GLint>(runner::RT_GL_NEAREST));
    glBindTexture(runner::GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &framebuffer_);
    glBindFramebuffer(runner::RT_GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(runner::RT_GL_FRAMEBUFFER, runner::RT_GL_COLOR_ATTACHMENT0, runner::GL_TEXTURE_2D, color_, 0);
    const bool complete = glCheckFramebufferStatus(runner::RT_GL_FRAMEBUFFER) == runner::RT_GL_FRAMEBUFFER_COMPLETE;
    target_width_ = width;
    target_height_ = height;
    return complete;
}

bool BillboardBaker::bake(const engine_core::VisualPrefab& prefab, const engine_core::Vec3& low,
                          const engine_core::Vec3& high, runner::ViewPixels& out) {
    out = runner::ViewPixels{};
    const CardFrame frame = card_frame(low, high);
    if (!(frame.width > 0.f) || !(frame.height > 0.f) || !ensureGraphics()) {
        return true;
    }
    // Its own Models, every texture whole: a card is drawn once.
    engine_core::VisualPrefab own = prefab;
    own.lods.clear();
    for (engine_core::VisualMesh& mesh : own.meshes) {
        mesh.diffuse_always_loaded = true;
        mesh.normal_always_loaded = true;
        mesh.roughness_always_loaded = true;
        mesh.metalness_always_loaded = true;
        mesh.emissive_always_loaded = true;
    }
    textures_.pump();
    std::vector<runner::MeshDraw> templates;
    std::uint32_t slot = 1;
    runner::LoadPrefabDraws(own, meshes_, textures_, slot, templates);
    if (textures_.loading()) {
        return false;
    }
    if (templates.empty()) {
        return true;
    }
    engine_core::VisualInstance row;
    row.id = 1;
    row.prefab = 1;
    std::vector<runner::MeshDraw> draws;
    runner::AppendRowDraws(row, templates, draws);

    // height_ pixels tall and as wide as the frame, drawn at twice that and shrunk.
    const int width = std::clamp(static_cast<int>(std::lround(height_ * frame.width / frame.height)), 8, height_ * 2);
    const int render_width = width * 2;
    const int render_height = height_ * 2;
    runner::GLint framebuffer = 0;
    runner::GLint viewport[4] = {};
    glGetIntegerv(runner::RT_GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(runner::GL_VIEWPORT, viewport);
    const bool scissor = glIsEnabled(runner::GL_SCISSOR_TEST) == runner::GL_TRUE;
    bool drawn = false;
    runner::ViewPixels albedo;
    if (ensureTarget(render_width, render_height)) {
        // Level with the card's middle, far enough off that the lens takes in
        // its height, and well clear of its nearest part.
        const float reach = frame.width * 0.5f;
        const float half = std::tan(kLensDegrees * 0.5f * kPi / 180.f);
        const float distance = std::min(std::max(frame.height * 0.5f / half, reach * 4.f + 1.f), kMostDistance);
        const float lens = 2.f * std::atan(frame.height * 0.5f / distance) * 180.f / kPi;
        const float middle = frame.height * 0.5f;
        renderer_.setCamera(
            engine_core::matrix4_look_at({0.f, middle, distance}, {0.f, middle, 0.f}, {0.f, 1.f, 0.f}), lens);
        glBindFramebuffer(runner::RT_GL_FRAMEBUFFER, framebuffer_);
        glViewport(0, 0, render_width, render_height);
        glDisable(runner::GL_SCISSOR_TEST);
        const double w = render_width;
        const double h = render_height;
        drawn = renderer_.draw(0, 0, w, h, w, h, draws.data(), static_cast<int>(draws.size()), nullptr, 0) &&
                renderer_.readAlbedo(albedo);
    }
    glBindFramebuffer(runner::RT_GL_FRAMEBUFFER, static_cast<runner::GLuint>(framebuffer));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (scissor) {
        glEnable(runner::GL_SCISSOR_TEST);
    }
    // Not drawn: GL is not ready yet, as in the first frame after the renderer is made.
    if (!drawn) {
        return false;
    }
    out = shrink_card(albedo);
    return true;
}

void BillboardBaker::release() {
    renderer_.shutdown();
    textures_.clear();
    meshes_.clear();
    if (framebuffer_ != 0) {
        glDeleteFramebuffers(1, &framebuffer_);
        framebuffer_ = 0;
    }
    if (color_ != 0) {
        glDeleteTextures(1, &color_);
        color_ = 0;
    }
    target_width_ = 0;
    target_height_ = 0;
    attempted_ = false;
    ready_ = false;
}

}  // namespace ide
