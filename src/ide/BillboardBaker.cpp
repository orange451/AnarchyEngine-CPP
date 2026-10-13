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
#include <fstream>
#include <system_error>
#include <vector>

namespace ide {

namespace {

// The card's lens: narrow, so a tree's near and far branches are nearly one size.
constexpr float kLensDegrees = 2.f;
// Past this the camera would put the far side beyond the renderer's far plane.
constexpr float kMostDistance = 3000.f;
constexpr float kPi = 3.14159265358979f;
// How far apart, out of 255, a map's drawn pixels may be and still be one number.
constexpr int kSameValue = 3;

std::uint32_t fnv1a(const std::string& bytes) {
    std::uint32_t hash = 2166136261u;
    for (const char c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return hash;
}

unsigned char unit_byte(float value) {
    return static_cast<unsigned char>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
}

// The exact sRGB curve, as tonemap.frag's linearToSrgb.
float linear_to_srgb(float c) {
    c = std::max(c, 0.f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

runner::ViewPixels blank(int width, int height) {
    runner::ViewPixels pixels;
    pixels.width = width;
    pixels.height = height;
    pixels.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 255);
    return pixels;
}

// Over the drawn pixels (color's alpha above 0), whether channel 0 of map stays
// within kSameValue, and their mean, 0 to 1.
bool one_value(const runner::ViewPixels& color, const runner::ViewPixels& map, float& mean) {
    int low = 255;
    int high = 0;
    double sum = 0.0;
    std::size_t drawn = 0;
    for (std::size_t i = 0; i * 4 + 3 < color.rgba.size(); ++i) {
        if (color.rgba[i * 4 + 3] == 0) {
            continue;
        }
        const int value = map.rgba[i * 4];
        low = std::min(low, value);
        high = std::max(high, value);
        sum += value;
        ++drawn;
    }
    mean = drawn > 0 ? static_cast<float>(sum / static_cast<double>(drawn) / 255.0) : 0.f;
    return high - low <= kSameValue;
}

}  // namespace

CardFrame card_frame(const engine_core::Vec3& low, const engine_core::Vec3& high) {
    const float reach = std::max({std::abs(low.x), std::abs(high.x), std::abs(low.z), std::abs(high.z)});
    CardFrame frame;
    frame.width = 2.f * reach;
    frame.height = std::max(high.y, 0.f);
    return frame;
}

CardMaps shrink_card(const runner::GBufferPixels& gbuffer) {
    CardMaps out;
    if (gbuffer.empty()) {
        return out;
    }
    const int width = std::max(1, gbuffer.width / 2);
    const int height = std::max(1, gbuffer.height / 2);
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    out.color = blank(width, height);
    out.normal = blank(width, height);
    out.roughness = blank(width, height);
    out.metalness = blank(width, height);
    out.emissive = blank(width, height);
    runner::ViewPixels* const maps[] = {&out.color, &out.normal, &out.roughness, &out.metalness, &out.emissive};
    std::vector<char> drawn(pixels, 0);
    std::deque<std::size_t> reached;
    double reflectivity = 0.0;
    double drawnWeight = 0.0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float weight = 0.f;
            float color[3] = {0.f, 0.f, 0.f};
            float normal[3] = {0.f, 0.f, 0.f};
            float material[3] = {0.f, 0.f, 0.f};
            float glow[3] = {0.f, 0.f, 0.f};
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(x * 2 + dx, gbuffer.width - 1);
                    const int sy = std::min(y * 2 + dy, gbuffer.height - 1);
                    const std::size_t at =
                        static_cast<std::size_t>(sy) * static_cast<std::size_t>(gbuffer.width) + static_cast<std::size_t>(sx);
                    const float alpha = gbuffer.albedo[at * 4 + 3] / 255.f;
                    weight += alpha;
                    for (std::size_t c = 0; c < 3; ++c) {
                        color[c] += gbuffer.albedo[at * 4 + c] * alpha;
                        normal[c] += gbuffer.normal[at * 3 + c] * alpha;
                        material[c] += gbuffer.material[at * 4 + c] * alpha;
                        glow[c] += gbuffer.emissive[at * 3 + c] * alpha;
                    }
                }
            }
            if (!(weight > 0.f)) {
                continue;
            }
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            unsigned char* c = out.color.rgba.data() + at * 4;
            for (int k = 0; k < 3; ++k) {
                c[k] = static_cast<unsigned char>(std::lround(std::clamp(color[k] / weight, 0.f, 255.f)));
            }
            c[3] = unit_byte(weight / 4.f);
            const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            unsigned char* n = out.normal.rgba.data() + at * 4;
            for (int k = 0; k < 3; ++k) {
                n[k] = unit_byte((length > 0.f ? normal[k] / length : (k == 2 ? 1.f : 0.f)) * 0.5f + 0.5f);
            }
            // The G-buffer's material target: metalness, roughness, reflectivity.
            const auto metal = static_cast<unsigned char>(std::lround(material[0] / weight));
            const auto rough = static_cast<unsigned char>(std::lround(material[1] / weight));
            std::fill_n(out.metalness.rgba.data() + at * 4, 3, metal);
            std::fill_n(out.roughness.rgba.data() + at * 4, 3, rough);
            reflectivity += material[2] / 255.f;
            drawnWeight += weight;
            unsigned char* e = out.emissive.rgba.data() + at * 4;
            for (int k = 0; k < 3; ++k) {
                e[k] = unit_byte(linear_to_srgb(glow[k] / weight));
            }
            drawn[at] = 1;
            reached.push_back(at);
        }
    }
    if (drawnWeight > 0.0) {
        out.reflectivity = static_cast<float>(reflectivity / drawnWeight);
    }
    // Outward from every drawn pixel, a step a pixel: each clear one takes
    // every map's values from the first drawn one to reach it, and stays clear.
    while (!reached.empty()) {
        const std::size_t at = reached.front();
        reached.pop_front();
        const int x = static_cast<int>(at % static_cast<std::size_t>(width));
        const int y = static_cast<int>(at / static_cast<std::size_t>(width));
        const int around[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto& next : around) {
            if (next[0] < 0 || next[1] < 0 || next[0] >= width || next[1] >= height) {
                continue;
            }
            const std::size_t to = static_cast<std::size_t>(next[1]) * static_cast<std::size_t>(width) + static_cast<std::size_t>(next[0]);
            if (drawn[to] != 0) {
                continue;
            }
            drawn[to] = 1;
            for (runner::ViewPixels* map : maps) {
                std::copy_n(map->rgba.data() + at * 4, 3, map->rgba.data() + to * 4);
            }
            out.color.rgba[to * 4 + 3] = 0;
            reached.push_back(to);
        }
    }
    // With nothing drawn at all, every pixel stays clear.
    for (std::size_t i = 0; i < pixels; ++i) {
        if (drawn[i] == 0) {
            out.color.rgba[i * 4 + 3] = 0;
        }
    }
    return out;
}

std::string card_file(const std::string& prefab_name, const std::string& map, const std::string& png) {
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
    return "textures/billboards/" + name + "." + map + "." + hash + ".png";
}

bool save_card(const CardMaps& maps, const std::filesystem::path& root, const std::string& prefab_name,
               CardFiles& files, std::string& why) {
    files = CardFiles{};
    files.reflectivity = maps.reflectivity;
    const auto save = [&](const runner::ViewPixels& pixels, bool alpha, const char* map, std::string& path) {
        const std::string png = alpha ? runner::EncodePngWithAlpha(pixels) : runner::EncodePng(pixels);
        path = card_file(prefab_name, map, png);
        const std::filesystem::path file = root / std::filesystem::u8path(path);
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        std::ofstream out(file, std::ios::binary);
        out << png;
        if (!out) {
            why = "The card could not be saved to " + file.u8string();
            return false;
        }
        return true;
    };
    // Whether a drawn pixel of map passes keep.
    const auto any_drawn = [&maps](const runner::ViewPixels& map, auto keep) {
        for (std::size_t i = 0; i * 4 + 3 < maps.color.rgba.size(); ++i) {
            if (maps.color.rgba[i * 4 + 3] != 0 && keep(map.rgba.data() + i * 4)) {
                return true;
            }
        }
        return false;
    };
    if (!save(maps.color, true, "color", files.color)) {
        return false;
    }
    // A flat card, every normal straight out of it, needs no normal map.
    const bool bent = any_drawn(maps.normal, [](const unsigned char* n) {
        return std::abs(n[0] - 128) > kSameValue || std::abs(n[1] - 128) > kSameValue || n[2] < 255 - kSameValue;
    });
    if (bent && !save(maps.normal, false, "normal", files.normal)) {
        return false;
    }
    // A map multiplies its Material's number: with a map, the number is 1.
    if (!one_value(maps.color, maps.roughness, files.roughness)) {
        files.roughness = 1.f;
        if (!save(maps.roughness, false, "roughness", files.roughness_map)) {
            return false;
        }
    }
    if (!one_value(maps.color, maps.metalness, files.metalness)) {
        files.metalness = 1.f;
        if (!save(maps.metalness, false, "metalness", files.metalness_map)) {
            return false;
        }
    }
    // Glow only where something glows.
    const bool glows = any_drawn(maps.emissive, [](const unsigned char* e) {
        return e[0] > kSameValue || e[1] > kSameValue || e[2] > kSameValue;
    });
    if (glows && !save(maps.emissive, false, "glow", files.emissive)) {
        return false;
    }
    return true;
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
                          const engine_core::Vec3& high, CardMaps& out) {
    out = CardMaps{};
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
    runner::GBufferPixels gbuffer;
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
                renderer_.readGBuffer(gbuffer);
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
    out = shrink_card(gbuffer);
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
