#include "ShadowRenderer.hpp"

#include "Renderer.hpp"
#include "ShaderFile.hpp"
#include "ParallelBlocks.hpp"
#include "amesh.hpp"
#include "gl.hpp"
#include "profiler/Profiler.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <string>

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

// source with #define name put in after its #version line.
std::string WithDefine(std::string source, const char* name) {
    const std::size_t line = source.find('\n');
    if (!source.empty()) {
        source.insert(line == std::string::npos ? source.size() : line + 1, std::string("#define ") + name + "\n");
    }
    return source;
}

// The cut-out program's texture unit.
constexpr int kCutoutUnit = 0;
// The widest cascade texel, in world units, that alpha-tests cut-outs. A
// coarser cascade is the far one, holding most of a forest's casters, whose
// texels are too wide to show a leaf's holes: its cut-outs cast whole, depth
// only, so the GPU's early depth test keeps drawing it as cheaply as before.
constexpr float kCutoutTexel = 0.1f;
// Frames a caster counts as moving after it last changed (CasterView::moving).
constexpr std::uint32_t kMovingFrames = 30;

// A surface Shader's part of a caster's fingerprint: which Shader, which
// version, which values, and frame (0 unless its holes follow TIME).
std::uint64_t SurfaceFingerprint(const engine_core::SurfaceBinding& surface, std::uint32_t frame) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::uint64_t part : {static_cast<std::uint64_t>(surface.shader), surface.version,
                                     surface.values_hash(), surface.samplers_hash, static_cast<std::uint64_t>(frame)}) {
        hash = (hash ^ part) * 1099511628211ull;
    }
    return hash | 1u;
}

}  // namespace

bool ShadowRenderer::initialize() {
    depth_ = DepthProgram{};
    cutout_ = DepthProgram{};
    depth_.id = LinkProgram(LoadShader("pipeline/shadow.vert"), LoadShader("pipeline/shadow.frag"), "Shadow");
    if (depth_.id == 0) {
        return false;
    }
    cutout_.id = LinkProgram(WithDefine(LoadShader("pipeline/shadow.vert"), "CUTOUT"),
                             WithDefine(LoadShader("pipeline/shadow.frag"), "CUTOUT"), "Shadow cut-out");
    if (cutout_.id == 0) {
        glDeleteProgram(depth_.id);
        depth_ = DepthProgram{};
        return false;
    }
    for (DepthProgram* program : {&depth_, &cutout_}) {
        program->viewProjection = glGetUniformLocation(program->id, "uViewProjection");
        // Skinned casters read their matrices from the bone texture's unit.
        glUseProgram(program->id);
        glUniform1i(glGetUniformLocation(program->id, "uBones"), kBoneTextureUnit);
    }
    glUniform1i(glGetUniformLocation(cutout_.id, "uDiffuse"), kCutoutUnit);
    glUseProgram(0);
    glGenFramebuffers(1, &atlasFbo_);
    glGenFramebuffers(1, &cascadeFbo_);
    glGenFramebuffers(1, &cacheFbo_);
    atlasStandIn_ = MakeDepth(1, 1, &kFarDepth);
    cascadeStandIn_ = MakeDepth(1, 1, &kFarDepth);
    return true;
}

void ShadowRenderer::shutdown() {
    for (DepthProgram* program : {&depth_, &cutout_}) {
        if (program->id != 0) {
            glDeleteProgram(program->id);
        }
        *program = DepthProgram{};
    }
    if (atlasFbo_ != 0) {
        glDeleteFramebuffers(1, &atlasFbo_);
        atlasFbo_ = 0;
    }
    if (cascadeFbo_ != 0) {
        glDeleteFramebuffers(1, &cascadeFbo_);
        cascadeFbo_ = 0;
    }
    if (cacheFbo_ != 0) {
        glDeleteFramebuffers(1, &cacheFbo_);
        cacheFbo_ = 0;
    }
    for (unsigned* texture : {&atlas_, &cascades_, &cascadeCache_, &atlasStandIn_, &cascadeStandIn_}) {
        DeleteTexture(*texture);
    }
    cascadeCacheSize_ = 0;
    instances_.destroy();
    atlasTextureSize_ = 0;
    atlasTexturePages_ = 0;
    cascadeSize_ = 0;
    planner_.clear();
    // A context made again may draw shadow maps, or atlases, where this one would not.
    refused_ = false;
    capSaid_ = false;
}

bool ShadowRenderer::makeAtlas(int size, int pages) {
    DeleteTexture(atlas_);
    atlasTextureSize_ = 0;
    atlasTexturePages_ = 0;
    // GL cannot add a layer in place, so a new page makes the whole array
    // again. Past the GPU's limits, or its memory, glTexImage3D says so.
    // glGetError reports the oldest error still unread, which may be another
    // pass's: drain those first so only MakeDepth's own failure counts here.
    // The drain is bounded, so a lost context cannot spin it.
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
    }
    unsigned texture = MakeDepth(size, pages, nullptr);
    bool made = glGetError() == GL_NO_ERROR;
    glBindFramebuffer(RT_GL_FRAMEBUFFER, atlasFbo_);
    if (made) {
        glFramebufferTextureLayer(RT_GL_FRAMEBUFFER, RT_GL_DEPTH_ATTACHMENT, texture, 0, 0);
        // Depth only: no color is drawn or read.
        const GLenum none = RT_GL_NONE;
        glDrawBuffers(1, &none);
        glReadBuffer(RT_GL_NONE);
        made = glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) == RT_GL_FRAMEBUFFER_COMPLETE;
    }
    if (!made) {
        DeleteTexture(texture);
        return false;
    }
    atlas_ = texture;
    atlasTextureSize_ = size;
    atlasTexturePages_ = pages;
    return true;
}

void ShadowRenderer::refuse(int size, int pages) {
    if (!refused_) {
        std::fprintf(stderr,
                     "This driver will not draw shadow maps (a %d x %d depth array (%d pages) could not be drawn to); "
                     "lights are drawn without shadows.\n",
                     size, size, pages);
        refused_ = true;
    }
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

int ShadowRenderer::addCasterRuns(const std::vector<int>& casters, const MeshDraw* meshes, bool cutouts) {
    const int begin = static_cast<int>(casterRuns_.size());
    casterOrder_.assign(casters.begin(), casters.end());
    // Caster i is mesh i. Those cast whole first, then the cut-outs by
    // texture, so each program and texture is set once.
    const auto cutoutOf = [&](int caster) { return cutouts && meshes[caster].cutout ? meshes[caster].texture : 0u; };
    // A caster whose surface Shader cuts holes casts through its shadow program
    // where cut-outs are tested; the binding names it, and so its values.
    const auto surfaceOf = [&](int caster) -> const engine_core::SurfaceBinding* {
        const MeshDraw& draw = meshes[caster];
        return cutouts && draw.surface != nullptr && draw.surface->cuts && surfaces_.variant ? draw.surface.get()
                                                                                             : nullptr;
    };
    std::stable_sort(casterOrder_.begin(), casterOrder_.end(), [&](int a, int b) {
        const engine_core::SurfaceBinding* surfaceA = surfaceOf(a);
        const engine_core::SurfaceBinding* surfaceB = surfaceOf(b);
        if (surfaceA != surfaceB) {
            return std::less<const void*>()(surfaceA, surfaceB);
        }
        const unsigned cutoutA = cutoutOf(a);
        const unsigned cutoutB = cutoutOf(b);
        if (cutoutA != cutoutB) {
            return cutoutA < cutoutB;
        }
        return std::less<const void*>()(meshes[a].mesh, meshes[b].mesh);
    });
    const engine_core::SurfaceBinding* lastSurface = nullptr;
    const SurfacePrograms::Variant* lastVariant = nullptr;
    for (const int caster : casterOrder_) {
        const MeshDraw& draw = meshes[caster];
        const unsigned cutout = cutoutOf(caster);
        const engine_core::SurfaceBinding* surface = surfaceOf(caster);
        if (surface != lastSurface) {
            lastSurface = surface;
            lastVariant = surface != nullptr ? surfaces_.variant(draw) : nullptr;
        }
        const SurfacePrograms::Variant* variant = surface != nullptr ? lastVariant : nullptr;
        if (static_cast<int>(casterRuns_.size()) == begin || casterRuns_.back().mesh != draw.mesh ||
            casterRuns_.back().cutout != cutout || casterRuns_.back().variant != variant ||
            (variant != nullptr && casterRuns_.back().draw->surface.get() != surface)) {
            casterRuns_.push_back(
                CasterRun{draw.mesh, cutout, static_cast<int>(casterRows_.size()), 0, variant, &draw});
        }
        ++casterRuns_.back().count;
        // Depth reads the world matrix, and a skinned caster its bones.
        InstanceData row{};
        std::copy(draw.model.m, draw.model.m + 16, row.model);
        row.boneBase = items_ != nullptr ? static_cast<float>(items_[caster].boneBase) : -1.f;
        casterRows_.push_back(row);
    }
    return begin;
}

bool ShadowRenderer::drawCasters(const Matrix4& viewProjection, int first, int end, bool& asked) {
    const DepthProgram* program = &depth_;
    glUseProgram(depth_.id);
    glUniformMatrix4fv(depth_.viewProjection, 1, GL_FALSE, viewProjection.m);
    glActiveTexture(GL_TEXTURE0 + kCutoutUnit);
    unsigned bound = 0;
    bool askedCutout = false;
    // The surface programs asked about, and the one in use.
    std::vector<unsigned> askedSurface;
    unsigned surfaceProgram = 0;
    bool surfaceBound = false;
    for (int index = first; index < end; ++index) {
        const CasterRun& run = casterRuns_[static_cast<std::size_t>(index)];
        if (run.variant != nullptr) {
            const RenderProgram& custom = run.variant->program;
            if (custom.id != surfaceProgram) {
                glUseProgram(custom.id);
                glUniformMatrix4fv(custom.viewProjection, 1, GL_FALSE, viewProjection.m);
                surfaceProgram = custom.id;
                program = nullptr;
            }
            surfaces_.bind(*run.variant, *run.draw);
            surfaceBound = true;
            // bind moved the active unit; the cut-outs' texture is set again.
            glActiveTexture(GL_TEXTURE0 + kCutoutUnit);
            bound = ~0u;
            run.mesh->bind();
            instances_.attach(run.first);
            if (std::find(askedSurface.begin(), askedSurface.end(), custom.id) == askedSurface.end()) {
                if (!CanDraw(custom.id)) {
                    surfaces_.done();
                    return false;
                }
                askedSurface.push_back(custom.id);
            }
            run.mesh->draw_instanced(0, run.count);
            ++calls_;
            continue;
        }
        const DepthProgram* wanted = run.cutout != 0 ? &cutout_ : &depth_;
        if (wanted != program) {
            program = wanted;
            surfaceProgram = 0;
            glUseProgram(program->id);
            glUniformMatrix4fv(program->viewProjection, 1, GL_FALSE, viewProjection.m);
        }
        if (run.cutout != bound) {
            glBindTexture(GL_TEXTURE_2D, run.cutout);
            bound = run.cutout;
        }
        run.mesh->bind();
        instances_.attach(run.first);
        bool& programAsked = program == &cutout_ ? askedCutout : asked;
        if (!programAsked && !CanDraw(program->id)) {
            if (surfaceBound) {
                surfaces_.done();
            }
            return false;
        }
        programAsked = true;
        run.mesh->draw_instanced(0, run.count);
        ++calls_;
    }
    if (surfaceBound) {
        surfaces_.done();
    }
    return true;
}

bool ShadowRenderer::draw(const std::vector<ShadowRequest>& requests, const MeshDraw* meshes, int count,
                          const Sphere* spheres, const CameraView& camera, const ShadowSettings& settings) {
    calls_ = 0;
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
    {
        PROFILE_SCOPE("Shadow casters", profiler::Group::Render);
        // Each changed mesh's caster, blocks of meshes on worker threads; the
        // rest are as the last frame left them.
        constexpr std::size_t kGrain = 8192;
        const auto total = static_cast<std::size_t>(std::max(count, 0));
        bool remade = casterOwners_.size() != total;
        // Made from scratch, nothing counts as moving: only a change seen frame to frame does.
        const bool fresh = castersAll_ || meshes != castersMeshes_;
        ++casterFrame_;
        if (fresh) {
            castersChanged_.assign(1, DrawRange{0, total});
            remade = true;
            casterMoving_.assign(total, 0);
            casterMovedAt_.assign(total, 0);
            movingCasters_.clear();
        } else {
            for (DrawRange& range : castersChanged_) {
                range.count = range.first >= total ? 0 : std::min(range.count, total - range.first);
            }
            NormalizeRanges(castersChanged_);
        }
        casterOwners_.resize(total);
        casterFingerprints_.resize(total);
        casterWatched_.resize(total, 0);
        casterSurface_.resize(total, 0);
        casterMoving_.resize(total, 0);
        casterMovedAt_.resize(total, 0);
        std::atomic<bool> changed{false};
        // A caster that really changed, seen frame to frame: moving from now (2 until listed).
        const auto moved = [&](std::size_t index) {
            changed.store(true, std::memory_order_relaxed);
            if (!fresh) {
                casterMovedAt_[index] = casterFrame_;
                if (casterMoving_[index] == 0) {
                    casterMoving_[index] = 2;
                }
            }
        };
        ParallelRanges(castersChanged_, kGrain, [&](std::size_t, std::size_t begin, std::size_t end) {
            for (std::size_t index = begin; index < end; ++index) {
                const MeshDraw& mesh = meshes[index];
                const std::uint64_t ownerWas = casterOwners_[index];
                const std::uint64_t fingerprintWas = casterFingerprints_[index];
                // See-through surfaces cast nothing, as in the legacy engine.
                // A terrain node (terrainLook) is always opaque, so it always casts,
                // but of two levels cross-fading only the one fading in, whole.
                // A surface Shader's no_shadow casts nothing; render_mode
                // transparent is see-through, so it casts nothing either.
                const engine_core::SurfaceBinding* surface = mesh.surface.get();
                if (mesh.mesh == nullptr || !mesh.mesh->valid() || !mesh.castsShadow ||
                    (mesh.transparency > 0.f && mesh.terrainLook == 0) ||
                    (mesh.terrainLook != 0 && !mesh.terrainFadeIn) ||
                    (surface != nullptr && (surface->modes.no_shadow || surface->modes.transparent))) {
                    casterWatched_[index] = 0;
                    casterSurface_[index] = 0;
                    casterOwners_[index] = 0;
                    casterFingerprints_[index] = 0;
                    if (ownerWas != 0 || fingerprintWas != 0) {
                        moved(index);
                    }
                    continue;
                }
                ShadowCaster caster;
                caster.mesh = reinterpret_cast<std::uintptr_t>(mesh.mesh);
                caster.revision = mesh.mesh->generation();
                caster.pose = mesh.bones != nullptr ? mesh.poseRevision : 0;
                caster.cutout = mesh.cutout ? mesh.texture : 0;
                caster.owner = mesh.owner;
                // Only a shader that cuts holes changes the shadow.
                const bool cuts = surface != nullptr && surface->cuts;
                const bool timed = cuts && surface->reads_time;
                casterWatched_[index] = cuts ? (timed ? 2 : 1) : 0;
                if (cuts) {
                    caster.surface = SurfaceFingerprint(*surface, timed ? casterFrame_ : 0);
                }
                casterSurface_[index] = caster.surface;
                caster.model = mesh.model;
                caster.bounds = spheres[index];
                casterOwners_[index] = mesh.owner;
                casterFingerprints_[index] = ShadowCasterFingerprint(caster);
                if (casterOwners_[index] != ownerWas || casterFingerprints_[index] != fingerprintWas) {
                    moved(index);
                }
            }
        });
        // A caster whose shader cuts holes moves when its values do, its draw
        // unchanged, and every frame when the holes follow TIME.
        for (std::size_t index = 0; index < total; ++index) {
            if (casterWatched_[index] == 0 || casterFingerprints_[index] == 0) {
                continue;
            }
            const MeshDraw& mesh = meshes[index];
            if (mesh.surface == nullptr) {
                continue;
            }
            const std::uint64_t part =
                SurfaceFingerprint(*mesh.surface, casterWatched_[index] == 2 ? casterFrame_ : 0);
            if (part == casterSurface_[index]) {
                continue;
            }
            casterSurface_[index] = part;
            ShadowCaster caster;
            caster.mesh = reinterpret_cast<std::uintptr_t>(mesh.mesh);
            caster.revision = mesh.mesh->generation();
            caster.pose = mesh.bones != nullptr ? mesh.poseRevision : 0;
            caster.cutout = mesh.cutout ? mesh.texture : 0;
            caster.owner = mesh.owner;
            caster.surface = part;
            caster.model = mesh.model;
            caster.bounds = spheres[index];
            const std::uint64_t fingerprint = ShadowCasterFingerprint(caster);
            if (fingerprint != casterFingerprints_[index]) {
                casterFingerprints_[index] = fingerprint;
                moved(index);
                if (!fresh && casterMoving_[index] == 2) {
                    casterMoving_[index] = 1;
                    movingCasters_.push_back(static_cast<int>(index));
                }
            }
        }
        // The newly moving listed; those still a while settle, and the
        // cascades draw them with the rest again.
        if (!fresh) {
            for (const DrawRange& range : castersChanged_) {
                for (std::size_t index = range.first; index < range.first + range.count; ++index) {
                    if (casterMoving_[index] == 2) {
                        casterMoving_[index] = 1;
                        movingCasters_.push_back(static_cast<int>(index));
                    }
                }
            }
        }
        std::size_t stillMoving = 0;
        for (const int index : movingCasters_) {
            const auto at = static_cast<std::size_t>(index);
            if (at >= total) {
                continue;
            }
            if (casterFrame_ - casterMovedAt_[at] >= kMovingFrames) {
                casterMoving_[at] = 0;
                changed.store(true, std::memory_order_relaxed);
                continue;
            }
            movingCasters_[stillMoving++] = index;
        }
        movingCasters_.resize(stillMoving);
        if (remade || changed.load(std::memory_order_relaxed) || hidden_ != revisionHidden_ ||
            hiddenRevision_ != revisionHiddenAt_) {
            ++casterRevision_;
            revisionHidden_ = hidden_;
            revisionHiddenAt_ = hiddenRevision_;
        }
        castersAll_ = false;
        castersChanged_.clear();
        castersMeshes_ = meshes;
        casterView_ = CasterView{total,           spheres, casterOwners_.data(), casterFingerprints_.data(),
                                 hidden_,         casterRevision_};
        casterView_.moving = movingCasters_.empty() ? nullptr : casterMoving_.data();
        // Only when it covers these casters: drawn items, from the same draws.
        if (live_ != nullptr && items_ != nullptr) {
            casterView_.live = live_->data();
            casterView_.liveCount = live_->size();
        }
    }

    ShadowPlan plan = planner_.planView(requests, casterView_, camera, settings);
    const int size = planner_.atlasSize();
    const int pages = planner_.atlasPages();
    if (size != atlasTextureSize_ || pages != atlasTexturePages_) {
        // What works now, within the settings and no bigger than what was
        // asked for, or the smallest atlas when there is none yet.
        const int workingSize =
            std::min(size, atlas_ != 0 ? std::min(atlasTextureSize_, settings.atlasMaxSize) : settings.atlasMinSize);
        const int workingPages =
            std::min(pages, atlas_ != 0 ? std::clamp(settings.atlasMaxPages, 1, atlasTexturePages_) : 1);
        if (!makeAtlas(size, pages)) {
            // Even the smallest will not do: no shadows, said once.
            if ((size == workingSize && pages == workingPages) || !makeAtlas(workingSize, workingPages)) {
                refuse(workingSize, workingPages);
                return true;
            }
            // Kept at what works, with the lights fitted to it.
            if (!capSaid_) {
                std::fprintf(stderr,
                             "The GPU will not make a shadow atlas of %d pages %d texels square; it keeps %d of %d, "
                             "and lights that look small get softer shadows.\n",
                             pages, size, workingPages, workingSize);
                capSaid_ = true;
            }
            planner_.capAtlas(workingSize, workingPages);
        }
        // A texture made again holds nothing yet, whatever the planner
        // thinks: every map is unreadable until drawn into it, planned again.
        planner_.forgetMaps();
        plan = planner_.planView(requests, casterView_, camera, settings);
    }
    if (!plan.draws.empty()) {
        // Each page's tiles together, so each page is attached once.
        std::vector<const TileDraw*> byPage;
        byPage.reserve(plan.draws.size());
        for (const TileDraw& tile : plan.draws) {
            byPage.push_back(&tile);
        }
        std::stable_sort(byPage.begin(), byPage.end(),
                         [](const TileDraw* a, const TileDraw* b) { return a->tile.page < b->tile.page; });
        // Every tile's casters as runs, uploaded once.
        casterRuns_.clear();
        casterRows_.clear();
        tileRuns_.clear();
        for (const TileDraw* draw : byPage) {
            const int first = addCasterRuns(draw->casters, meshes, true);
            tileRuns_.emplace_back(first, static_cast<int>(casterRuns_.size()));
        }
        instances_.upload(casterRows_.data(), static_cast<int>(casterRows_.size()));
        glBindFramebuffer(RT_GL_FRAMEBUFFER, atlasFbo_);
        begin();
        bool asked = false;
        int attached = -1;
        for (std::size_t t = 0; t < byPage.size(); ++t) {
            const TileDraw& tile = *byPage[t];
            if (tile.tile.page != attached) {
                glFramebufferTextureLayer(RT_GL_FRAMEBUFFER, RT_GL_DEPTH_ATTACHMENT, atlas_, 0, tile.tile.page);
                attached = tile.tile.page;
            }
            glViewport(tile.tile.x, tile.tile.y, tile.tile.size, tile.tile.size);
            glScissor(tile.tile.x, tile.tile.y, tile.tile.size, tile.tile.size);
            glClear(GL_DEPTH_BUFFER_BIT);
            if (!drawCasters(tile.viewProjection, tileRuns_[t].first, tileRuns_[t].second, asked)) {
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
        DeleteTexture(cascades_);
        cascades_ = MakeDepth(size, kMaxCascades, nullptr);
        cascadeSize_ = size;
        glBindFramebuffer(RT_GL_FRAMEBUFFER, cascadeFbo_);
        const GLenum none = RT_GL_NONE;
        glDrawBuffers(1, &none);
        glReadBuffer(RT_GL_NONE);
        planner_.forgetCascades();
    }
    // The kept still casters are lost with a texture made again, or none yet.
    if (cascadeCacheSize_ != size) {
        planner_.forgetCascadeCache();
    }
    std::vector<CascadeDraw> draws;
    {
        PROFILE_SCOPE("Cascade plan", profiler::Group::Render);
        draws = planner_.planCascadesView(sun, casterView_, camera, settings);
    }
    if (draws.empty()) {
        return true;
    }
    const bool keeps = std::any_of(draws.begin(), draws.end(),
                                   [](const CascadeDraw& draw) { return draw.kind != CascadeDraw::Kind::All; });
    if (keeps && cascadeCacheSize_ != size) {
        DeleteTexture(cascadeCache_);
        cascadeCache_ = MakeDepth(size, kMaxCascades, nullptr);
        cascadeCacheSize_ = size;
        glBindFramebuffer(RT_GL_FRAMEBUFFER, cacheFbo_);
        const GLenum none = RT_GL_NONE;
        glDrawBuffers(1, &none);
        glReadBuffer(RT_GL_NONE);
    }
    // Unreadable until these draws commit: a layer redrawn before one fails
    // would no longer match the matrices the last commit kept.
    planner_.forgetCascades();
    // Each draw's runs: its casters' (every one, or the still ones), then its moving ones'.
    std::vector<int> movingFirst;
    {
        PROFILE_SCOPE("Cascade casters", profiler::Group::Render);
        casterRuns_.clear();
        casterRows_.clear();
        tileRuns_.clear();
        for (const CascadeDraw& draw : draws) {
            const bool cutouts = draw.texelWorld <= kCutoutTexel;
            const int first = addCasterRuns(draw.casters, meshes, cutouts);
            tileRuns_.emplace_back(first, static_cast<int>(casterRuns_.size()));
            movingFirst.push_back(addCasterRuns(draw.moving, meshes, cutouts));
        }
        instances_.upload(casterRows_.data(), static_cast<int>(casterRows_.size()));
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, cascadeFbo_);
    begin();
    glViewport(0, 0, size, size);
    glScissor(0, 0, size, size);
    bool asked = false;
    for (std::size_t c = 0; c < draws.size(); ++c) {
        const CascadeDraw& draw = draws[c];
        const int movingEnd = c + 1 < draws.size() ? tileRuns_[c + 1].first : static_cast<int>(casterRuns_.size());
        if (draw.kind != CascadeDraw::Kind::Moving) {
            // Every caster into the cascade, or the still ones into the layer kept of them.
            const bool kept = draw.kind == CascadeDraw::Kind::Still;
            glBindFramebuffer(RT_GL_FRAMEBUFFER, kept ? cacheFbo_ : cascadeFbo_);
            glFramebufferTextureLayer(RT_GL_FRAMEBUFFER, RT_GL_DEPTH_ATTACHMENT, kept ? cascadeCache_ : cascades_, 0,
                                      draw.layer);
            if (glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) != RT_GL_FRAMEBUFFER_COMPLETE) {
                refuse(size, 1);
                end();
                return true;
            }
            glClear(GL_DEPTH_BUFFER_BIT);
            if (!drawCasters(draw.viewProjection, tileRuns_[c].first, tileRuns_[c].second, asked)) {
                end();
                return false;
            }
        }
        if (draw.kind != CascadeDraw::Kind::All) {
            copyKeptLayer(draw.layer, size);
            if (!drawCasters(draw.viewProjection, movingFirst[c], movingEnd, asked)) {
                end();
                return false;
            }
        }
    }
    end();
    planner_.commitCascades();
    return true;
}

void ShadowRenderer::copyKeptLayer(int layer, int size) {
    glBindFramebuffer(RT_GL_READ_FRAMEBUFFER, cacheFbo_);
    glFramebufferTextureLayer(RT_GL_READ_FRAMEBUFFER, RT_GL_DEPTH_ATTACHMENT, cascadeCache_, 0, layer);
    glBindFramebuffer(RT_GL_DRAW_FRAMEBUFFER, cascadeFbo_);
    glFramebufferTextureLayer(RT_GL_DRAW_FRAMEBUFFER, RT_GL_DEPTH_ATTACHMENT, cascades_, 0, layer);
    glBlitFramebuffer(0, 0, size, size, 0, 0, size, size, GL_DEPTH_BUFFER_BIT, RT_GL_NEAREST);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, cascadeFbo_);
}

void ShadowRenderer::noteDraws(bool all, const std::vector<DrawRange>& changed) {
    // Frames that draw no shadows pile their changes up; past a few thousand
    // ranges, making every caster again is as cheap.
    if (all || castersChanged_.size() + changed.size() > 4096) {
        castersAll_ = true;
        castersChanged_.clear();
        return;
    }
    if (!castersAll_) {
        castersChanged_.insert(castersChanged_.end(), changed.begin(), changed.end());
    }
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
