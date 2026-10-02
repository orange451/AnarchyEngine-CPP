#include "ShadowRenderer.hpp"

#include "Renderer.hpp"
#include "ShaderFile.hpp"
#include "amesh.hpp"
#include "gl.hpp"

#include <algorithm>
#include <cstdio>

namespace runner {
namespace {

using engine_core::Matrix4;

// Polygon offset while a map is drawn: slope, then constant. With the
// normal offset in shadow.glsl, enough that a lit surface does not shadow itself.
constexpr float kSlopeBias = 1.5f;
constexpr float kConstantBias = 3.f;
// A 24-bit depth of 1, as GL_UNSIGNED_INT uploads it: nothing casts.
constexpr unsigned kFarDepth = 0xFFFFFFFFu;

// Hardware depth comparison with 2 by 2 filtering, as sampler*Shadow reads it.
void DepthParameters(GLenum target) {
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(target, GL_TEXTURE_WRAP_S, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
    glTexParameteri(target, GL_TEXTURE_WRAP_T, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
    glTexParameteri(target, RT_GL_TEXTURE_COMPARE_MODE, static_cast<GLint>(RT_GL_COMPARE_REF_TO_TEXTURE));
    glTexParameteri(target, RT_GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(RT_GL_LEQUAL));
}

// A 2D array of layers of 24-bit depth, each size texels square. pixels is
// null or one GL_UNSIGNED_INT depth per texel per layer.
unsigned MakeDepth(int size, int layers, const void* pixels) {
    unsigned texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(RT_GL_TEXTURE_2D_ARRAY, texture);
    glTexImage3D(RT_GL_TEXTURE_2D_ARRAY, 0, static_cast<GLint>(RT_GL_DEPTH_COMPONENT24), size, size, layers, 0,
                 RT_GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, pixels);
    DepthParameters(RT_GL_TEXTURE_2D_ARRAY);
    glBindTexture(RT_GL_TEXTURE_2D_ARRAY, 0);
    return texture;
}

}  // namespace

bool ShadowRenderer::initialize() {
    depth_ = DepthProgram{};
    depth_.id = LinkProgram(LoadShader("pipeline/shadow.vert"), LoadShader("pipeline/shadow.frag"), "Shadow");
    if (depth_.id == 0) {
        return false;
    }
    depth_.model = glGetUniformLocation(depth_.id, "uModel");
    depth_.viewProjection = glGetUniformLocation(depth_.id, "uViewProjection");
    glGenFramebuffers(1, &atlasFbo_);
    glGenFramebuffers(1, &cascadeFbo_);
    atlasStandIn_ = MakeDepth(1, 1, &kFarDepth);
    cascadeStandIn_ = MakeDepth(1, 1, &kFarDepth);
    return true;
}

void ShadowRenderer::shutdown() {
    if (depth_.id != 0) {
        glDeleteProgram(depth_.id);
    }
    depth_ = DepthProgram{};
    if (atlasFbo_ != 0) {
        glDeleteFramebuffers(1, &atlasFbo_);
        atlasFbo_ = 0;
    }
    if (cascadeFbo_ != 0) {
        glDeleteFramebuffers(1, &cascadeFbo_);
        cascadeFbo_ = 0;
    }
    for (unsigned* texture : {&atlas_, &cascades_, &atlasStandIn_, &cascadeStandIn_}) {
        if (*texture != 0) {
            glDeleteTextures(1, texture);
            *texture = 0;
        }
    }
    atlasTextureSize_ = 0;
    atlasTexturePages_ = 0;
    cascadeSize_ = 0;
    planner_.clear();
    // A context made again may draw shadow maps where this one would not.
    refused_ = false;
}

bool ShadowRenderer::makeAtlas(int size, int pages) {
    if (atlas_ != 0) {
        glDeleteTextures(1, &atlas_);
    }
    // GL cannot add a layer in place, so a new page makes the whole array again.
    atlas_ = MakeDepth(size, pages, nullptr);
    atlasTextureSize_ = size;
    atlasTexturePages_ = pages;
    glBindFramebuffer(RT_GL_FRAMEBUFFER, atlasFbo_);
    glFramebufferTextureLayer(RT_GL_FRAMEBUFFER, RT_GL_DEPTH_ATTACHMENT, atlas_, 0, 0);
    // Depth only: no color is drawn or read.
    const GLenum none = RT_GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(RT_GL_NONE);
    if (glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) == RT_GL_FRAMEBUFFER_COMPLETE) {
        return true;
    }
    if (!refused_) {
        std::fprintf(stderr, "This driver will not draw shadow maps; lights are drawn without shadows.\n");
        refused_ = true;
    }
    glDeleteTextures(1, &atlas_);
    atlas_ = 0;
    atlasTextureSize_ = 0;
    atlasTexturePages_ = 0;
    return false;
}

void ShadowRenderer::begin() {
    glDisable(GL_BLEND);
    // Both sides: a plane casts a shadow whichever way it faces.
    glDisable(RT_GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(RT_GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(RT_GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(kSlopeBias, kConstantBias);
    glEnable(GL_SCISSOR_TEST);
}

void ShadowRenderer::end() {
    glDisable(RT_GL_POLYGON_OFFSET_FILL);
    glDisable(GL_SCISSOR_TEST);
}

bool ShadowRenderer::drawCasters(const Matrix4& viewProjection, const std::vector<int>& casters,
                                 const MeshDraw* meshes, bool& asked) {
    glUseProgram(depth_.id);
    glUniformMatrix4fv(depth_.viewProjection, 1, GL_FALSE, viewProjection.m);
    for (const int caster : casters) {
        const MeshDraw& draw = meshes[casterMeshes_[static_cast<std::size_t>(caster)]];
        glUniformMatrix4fv(depth_.model, 1, GL_FALSE, draw.model.m);
        draw.mesh->bind();
        if (!asked && !CanDraw(depth_.id)) {
            return false;
        }
        asked = true;
        draw.mesh->draw(0);
    }
    return true;
}

bool ShadowRenderer::draw(const std::vector<ShadowRequest>& requests, const MeshDraw* meshes, int count,
                          const CameraView& camera, const ShadowSettings& settings) {
    if (refused_) {
        return true;
    }
    // New atlas settings start every map over.
    if (settings.atlasMinSize != plannedMin_ || settings.atlasMaxSize != plannedMax_ ||
        settings.atlasMaxPages != plannedMaxPages_ || settings.minTile != plannedMinTile_ ||
        settings.maxTile != plannedMaxTile_) {
        planner_.clear();
        plannedMin_ = settings.atlasMinSize;
        plannedMax_ = settings.atlasMaxSize;
        plannedMaxPages_ = settings.atlasMaxPages;
        plannedMinTile_ = settings.minTile;
        plannedMaxTile_ = settings.maxTile;
    }
    casters_.clear();
    casterMeshes_.clear();
    for (int index = 0; index < count; ++index) {
        const MeshDraw& mesh = meshes[index];
        // See-through surfaces cast nothing, as in the legacy engine.
        if (mesh.mesh == nullptr || !mesh.mesh->valid() || mesh.transparency > 0.f) {
            continue;
        }
        ShadowCaster caster;
        caster.mesh = reinterpret_cast<std::uintptr_t>(mesh.mesh);
        caster.revision = mesh.mesh->generation();
        caster.owner = mesh.owner;
        caster.model = mesh.model;
        caster.bounds = WorldBounds(mesh.model, mesh.mesh->bounds_min(), mesh.mesh->bounds_max());
        casters_.push_back(caster);
        casterMeshes_.push_back(index);
    }

    const ShadowPlan plan = planner_.plan(requests, casters_, camera, settings);
    if ((planner_.atlasSize() != atlasTextureSize_ || planner_.atlasPages() != atlasTexturePages_) &&
        !makeAtlas(planner_.atlasSize(), planner_.atlasPages())) {
        return true;
    }
    if (!plan.draws.empty()) {
        glBindFramebuffer(RT_GL_FRAMEBUFFER, atlasFbo_);
        begin();
        bool asked = false;
        // Each page's tiles together, so each page is attached once.
        std::vector<const TileDraw*> byPage;
        byPage.reserve(plan.draws.size());
        for (const TileDraw& tile : plan.draws) {
            byPage.push_back(&tile);
        }
        std::stable_sort(byPage.begin(), byPage.end(),
                         [](const TileDraw* a, const TileDraw* b) { return a->tile.page < b->tile.page; });
        int attached = -1;
        for (const TileDraw* draw : byPage) {
            const TileDraw& tile = *draw;
            if (tile.tile.page != attached) {
                glFramebufferTextureLayer(RT_GL_FRAMEBUFFER, RT_GL_DEPTH_ATTACHMENT, atlas_, 0, tile.tile.page);
                attached = tile.tile.page;
            }
            glViewport(tile.tile.x, tile.tile.y, tile.tile.size, tile.tile.size);
            glScissor(tile.tile.x, tile.tile.y, tile.tile.size, tile.tile.size);
            glClear(GL_DEPTH_BUFFER_BIT);
            if (!drawCasters(tile.viewProjection, tile.casters, meshes, asked)) {
                end();
                return false;
            }
        }
        end();
    }
    planner_.commit();
    return true;
}

ShadowLookup ShadowRenderer::lookup(std::uint64_t key) const {
    ShadowLookup out;
    const LocalShadow* shadow = planner_.find(key);
    // Refused, every light is drawn unshadowed, not from a map that no longer changes.
    if (refused_ || shadow == nullptr || atlas_ == 0) {
        return out;
    }
    const float atlas = static_cast<float>(atlasTextureSize_);
    out.kind = shadow->kind == ShadowKind::Spot ? ShadowLookup::kSpot : ShadowLookup::kPoint;
    out.matrices[0] = shadow->viewProjection;
    out.light[0] = shadow->position.x;
    out.light[1] = shadow->position.y;
    out.light[2] = shadow->position.z;
    out.nearFar[0] = shadow->nearZ;
    out.nearFar[1] = shadow->farZ;
    out.texel[0] = shadow->texelPerDistance;
    out.texelUv = 1.f / atlas;
    out.faceScale = shadow->faceScale;
    const int tileCount = shadow->kind == ShadowKind::Spot ? 1 : 6;
    for (int t = 0; t < tileCount; ++t) {
        out.tiles[t][0] = static_cast<float>(shadow->tiles[t].x) / atlas;
        out.tiles[t][1] = static_cast<float>(shadow->tiles[t].y) / atlas;
        out.tiles[t][2] = static_cast<float>(shadow->tiles[t].size) / atlas;
        out.tiles[t][3] = static_cast<float>(shadow->tiles[t].page);
    }
    return out;
}

bool ShadowRenderer::drawSun(const SunRequest* sun, const MeshDraw* meshes, const CameraView& camera,
                             const ShadowSettings& settings) {
    if (refused_) {
        return true;
    }
    const int size = std::max(settings.cascadeSize, 16);
    if (sun != nullptr && size != cascadeSize_) {
        if (cascades_ != 0) {
            glDeleteTextures(1, &cascades_);
        }
        cascades_ = MakeDepth(size, kMaxCascades, nullptr);
        cascadeSize_ = size;
        glBindFramebuffer(RT_GL_FRAMEBUFFER, cascadeFbo_);
        const GLenum none = RT_GL_NONE;
        glDrawBuffers(1, &none);
        glReadBuffer(RT_GL_NONE);
        planner_.forgetCascades();
    }
    const std::vector<CascadeDraw> draws = planner_.planCascades(sun, casters_, camera, settings);
    if (draws.empty()) {
        return true;
    }
    // Unreadable until these draws commit: a layer redrawn before one fails
    // would no longer match the matrices the last commit kept.
    planner_.forgetCascades();
    glBindFramebuffer(RT_GL_FRAMEBUFFER, cascadeFbo_);
    begin();
    glViewport(0, 0, size, size);
    glScissor(0, 0, size, size);
    bool asked = false;
    for (const CascadeDraw& draw : draws) {
        glFramebufferTextureLayer(RT_GL_FRAMEBUFFER, RT_GL_DEPTH_ATTACHMENT, cascades_, 0, draw.layer);
        if (glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) != RT_GL_FRAMEBUFFER_COMPLETE) {
            if (!refused_) {
                std::fprintf(stderr, "This driver will not draw shadow maps; lights are drawn without shadows.\n");
                refused_ = true;
            }
            end();
            return true;
        }
        glClear(GL_DEPTH_BUFFER_BIT);
        if (!drawCasters(draw.viewProjection, draw.casters, meshes, asked)) {
            end();
            return false;
        }
    }
    end();
    planner_.commitCascades();
    return true;
}

ShadowLookup ShadowRenderer::sunLookup() const {
    ShadowLookup out;
    const CascadeShadow* shadow = planner_.cascades();
    if (refused_ || shadow == nullptr || cascades_ == 0) {
        return out;
    }
    out.kind = ShadowLookup::kCascades;
    out.cascades = shadow->count;
    for (int i = 0; i < shadow->count; ++i) {
        out.matrices[i] = shadow->viewProjection[i];
        out.texel[i] = shadow->texelWorld[i];
    }
    out.texelUv = 1.f / static_cast<float>(cascadeSize_);
    return out;
}

}  // namespace runner
