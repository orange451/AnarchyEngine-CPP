#include "SceneDraws.hpp"

#include "Matrix4.hpp"
#include "ParallelBlocks.hpp"
#include "amesh.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace runner {

using engine_core::texture::Usage;

namespace {

// sources as templates drawn from lodNear up to lodFar.
void LoadMeshDraws(const std::vector<engine_core::VisualMesh>& sources, float lodNear, float lodFar,
                   MeshCache& meshes, TextureCache& textures, std::uint32_t& nextSlot, std::vector<MeshDraw>& out) {
    for (const engine_core::VisualMesh& source : sources) {
        // A play session's geometry, while it lasts, else the Mesh's file.
        const anarchy::amesh::GpuMesh* mesh = source.session != nullptr
                                                  ? meshes.getSession(source.mesh, *source.session, source.revision)
                                                  : meshes.get(source.path);
        if (mesh == nullptr) {
            continue;
        }
        MeshDraw draw;
        draw.mesh = mesh;
        draw.texture = textures.get(source.diffuse_texture, Usage::Color, source.diffuse_flip_y, source.diffuse_always_loaded);
        draw.normalTexture = textures.get(source.normal_texture, Usage::Normal, source.normal_flip_y, source.normal_always_loaded);
        draw.roughnessTexture = textures.get(source.roughness_texture, Usage::Mask, source.roughness_flip_y, source.roughness_always_loaded);
        draw.metalnessTexture = textures.get(source.metalness_texture, Usage::Mask, source.metalness_flip_y, source.metalness_always_loaded);
        draw.emissiveTexture = textures.get(source.emissive_texture, Usage::Color, source.emissive_flip_y, source.emissive_always_loaded);
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
        draw.skinned = source.skinned;
        draw.lodNear = lodNear;
        draw.lodFar = lodFar;
        draw.slot = nextSlot++;
        out.push_back(draw);
    }
}

}  // namespace

void LoadPrefabDraws(const engine_core::VisualPrefab& prefab, MeshCache& meshes, TextureCache& textures,
                     std::uint32_t& nextSlot, std::vector<MeshDraw>& out) {
    constexpr float kForever = std::numeric_limits<float>::infinity();
    // The Prefab's own Models up to its first LodLayer; each layer from its
    // distance up to the next one's, the last forever. Of two at one
    // distance, the later one draws.
    LoadMeshDraws(prefab.meshes, 0.f, prefab.lods.empty() ? kForever : prefab.lods.front().distance, meshes,
                  textures, nextSlot, out);
    for (std::size_t index = 0; index < prefab.lods.size(); ++index) {
        const engine_core::VisualLod& lod = prefab.lods[index];
        const float lodFar = index + 1 < prefab.lods.size() ? prefab.lods[index + 1].distance : kForever;
        if (!(lod.distance < lodFar)) {
            continue;
        }
        if (lod.kind == engine_core::LodKind::Model) {
            LoadMeshDraws(lod.meshes, lod.distance, lodFar, meshes, textures, nextSlot, out);
            continue;
        }
        if (lod.kind != engine_core::LodKind::Billboard) {
            continue;  // None: nothing drawn in its band.
        }
        const anarchy::amesh::GpuMesh* quad = meshes.billboardQuad();
        if (quad == nullptr) {
            continue;
        }
        // A card: its texture on a quad turned to the camera, casting nothing.
        MeshDraw draw;
        draw.mesh = quad;
        draw.texture = textures.get(lod.texture, Usage::Color, lod.texture_flip_y, lod.texture_always_loaded);
        draw.roughness = 1.f;
        draw.reflectivity = 0.f;
        draw.castsShadow = false;
        draw.billboard = true;
        draw.billboardSize[0] = lod.width;
        draw.billboardSize[1] = lod.height;
        draw.lodNear = lod.distance;
        draw.lodFar = lodFar;
        draw.slot = nextSlot++;
        out.push_back(draw);
    }
}

bool LightDrawFor(const engine_core::VisualInstance& row, LightDraw& out) {
    if (row.light.kind == engine_core::VisualLight::Kind::None || !row.light.enabled) {
        return false;
    }
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
    out = light;
    return true;
}

namespace {

// Its Scale grows the Prefab about the GameObject's origin: each axis, not the position.
engine_core::Matrix4 ScaledWorld(const engine_core::VisualInstance& row) {
    engine_core::Matrix4 scaled = row.world;
    for (int column = 0; column < 3; ++column) {
        for (int axis = 0; axis < 3; ++axis) {
            scaled.m[column * 4 + axis] *= row.scale;
        }
    }
    return scaled;
}

// draw, a copy of one of the row's templates, made the row's: placed, tinted, faded, and posed.
void MakeRowDraw(const engine_core::VisualInstance& row, const engine_core::Matrix4& scaled, float opacity,
                 MeshDraw& draw) {
    draw.model = scaled;
    // A billboard's quad is a unit square: its width along the row's x, its height along its up.
    if (draw.billboard) {
        for (int axis = 0; axis < 3; ++axis) {
            draw.model.m[axis] *= draw.billboardSize[0];
            draw.model.m[4 + axis] *= draw.billboardSize[1];
        }
    }
    draw.owner = row.id;
    draw.tint[0] = row.color.r;
    draw.tint[1] = row.color.g;
    draw.tint[2] = row.color.b;
    draw.transparency = 1.f - (1.f - std::clamp(draw.transparency, 0.f, 1.f)) * opacity;
    // A Model on the Prefab's skeleton moves with the row's pose, which
    // the caller keeps alive through the frame.
    if (draw.skinned && row.pose != nullptr && !row.pose->palette.empty()) {
        draw.bones = row.pose->palette.data();
        draw.boneCount = static_cast<int>(row.pose->palette.size() / 12);
        draw.poseRevision = row.pose->revision;
        const float* low = draw.mesh->bounds_min();
        const float* high = draw.mesh->bounds_max();
        const float poseLow[3] = {row.pose->low.x, row.pose->low.y, row.pose->low.z};
        const float poseHigh[3] = {row.pose->high.x, row.pose->high.y, row.pose->high.z};
        for (int axis = 0; axis < 3; ++axis) {
            draw.poseMin[axis] = std::min(low[axis], poseLow[axis]);
            draw.poseMax[axis] = std::max(high[axis], poseHigh[axis]);
        }
    }
}

}  // namespace

void AppendRowDraws(const engine_core::VisualInstance& row, const std::vector<MeshDraw>& templates,
                    std::vector<MeshDraw>& out) {
    // The GameObject's Color tints each Material's, per instance, and its opacity multiplies each Material's.
    const float opacity = 1.f - row.transparency;
    const engine_core::Matrix4 scaled = ScaledWorld(row);
    for (const MeshDraw& model : templates) {
        MakeRowDraw(row, scaled, opacity, out.emplace_back(model));
    }
}

MeshDraw* WriteRowDraws(const engine_core::VisualInstance& row, const std::vector<MeshDraw>& templates,
                        MeshDraw* out) {
    const float opacity = 1.f - row.transparency;
    const engine_core::Matrix4 scaled = ScaledWorld(row);
    for (const MeshDraw& model : templates) {
        *out = model;
        MakeRowDraw(row, scaled, opacity, *out);
        ++out;
    }
    return out;
}

namespace {

template <typename T>
bool SameBits(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

std::uint64_t GenerationOf(const MeshDraw& draw) { return draw.mesh != nullptr ? draw.mesh->generation() : 0; }

// now against the templates kept, with their meshes' generations then. Only
// now's meshes are read: a kept one may be gone, but one that is the same
// pointer as now's is now's.
bool SameTemplates(const std::vector<std::vector<MeshDraw>>& now, const std::vector<std::vector<MeshDraw>>& kept,
                   const std::vector<std::vector<std::uint64_t>>& generations) {
    if (now.size() != kept.size()) {
        return false;
    }
    for (std::size_t prefab = 0; prefab < now.size(); ++prefab) {
        if (!std::equal(now[prefab].begin(), now[prefab].end(), kept[prefab].begin(), kept[prefab].end(), SameDraw)) {
            return false;
        }
        for (std::size_t m = 0; m < now[prefab].size(); ++m) {
            if (GenerationOf(now[prefab][m]) != generations[prefab][m]) {
                return false;
            }
        }
    }
    return true;
}

// Chunks of rows RowDrawCache gives each worker thread at a time.
constexpr std::size_t kChunkGrain = 16;

}  // namespace

bool SameDraw(const MeshDraw& a, const MeshDraw& b) {
    return a.mesh == b.mesh && SameBits(a.model, b.model) && a.texture == b.texture && SameBits(a.color, b.color) &&
           a.normalTexture == b.normalTexture && a.roughnessTexture == b.roughnessTexture &&
           a.metalnessTexture == b.metalnessTexture && a.emissiveTexture == b.emissiveTexture &&
           SameBits(a.emissive, b.emissive) && SameBits(a.metalness, b.metalness) &&
           SameBits(a.roughness, b.roughness) && SameBits(a.reflectivity, b.reflectivity) &&
           SameBits(a.transparency, b.transparency) && a.owner == b.owner && SameBits(a.tint, b.tint) &&
           a.slot == b.slot && a.terrainLook == b.terrainLook && SameBits(a.terrainFade, b.terrainFade) &&
           a.terrainFadeIn == b.terrainFadeIn && a.terrainLevel == b.terrainLevel && a.shadowOnly == b.shadowOnly &&
           a.lod == b.lod && a.castsShadow == b.castsShadow && a.terrainSurfaceA == b.terrainSurfaceA &&
           a.terrainSurfaceB == b.terrainSurfaceB && a.terrainSurfaceC == b.terrainSurfaceC &&
           a.terrainLayerCount == b.terrainLayerCount && a.skinned == b.skinned && a.bones == b.bones &&
           a.poseRevision == b.poseRevision && a.boneCount == b.boneCount && SameBits(a.poseMin, b.poseMin) &&
           SameBits(a.poseMax, b.poseMax) && SameBits(a.lodNear, b.lodNear) && SameBits(a.lodFar, b.lodFar) &&
           a.billboard == b.billboard && SameBits(a.billboardSize, b.billboardSize);
}

void RowDrawCache::update(const engine_core::VisualRows& rows, const std::vector<std::vector<MeshDraw>>& templates,
                          std::vector<MeshDraw>& draws, std::vector<LightDraw>& lights,
                          std::vector<DrawRange>& written) {
    constexpr std::size_t kRows = engine_core::VisualRows::kChunkRows;
    written.clear();
    // New templates draw every row again; so does a draws vector that lost what was left in it.
    const bool sameTemplates = SameTemplates(templates, templates_, generations_);
    if (!sameTemplates) {
        templates_ = templates;
        generations_.resize(templates.size());
        for (std::size_t prefab = 0; prefab < templates.size(); ++prefab) {
            generations_[prefab].resize(templates[prefab].size());
            for (std::size_t m = 0; m < templates[prefab].size(); ++m) {
                generations_[prefab][m] = GenerationOf(templates[prefab][m]);
            }
        }
    }
    const bool keep = sameTemplates && draws.size() >= drawn_;
    const std::size_t chunkCount = rows.chunk_count();
    const std::size_t prefabCount = templates.size();
    chunks_.resize(chunkCount);
    recount_.clear();
    for (std::size_t k = 0; k < chunkCount; ++k) {
        if (!keep || chunks_[k].rows.get() != rows.chunk(k)) {
            recount_.push_back(k);
        }
    }
    // Each changed chunk's draws counted, and its lights found.
    ParallelBlocks(recount_.size(), kChunkGrain, [&](std::size_t, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const std::size_t k = recount_[i];
            Chunk& chunk = chunks_[k];
            chunk.lights.clear();
            std::size_t count = 0;
            const std::size_t rowEnd = std::min(rows.size(), (k + 1) * kRows);
            for (std::size_t index = k * kRows; index < rowEnd; ++index) {
                const engine_core::VisualInstance& row = rows[index];
                if (!row.alive) {
                    continue;
                }
                LightDraw light;
                if (LightDrawFor(row, light)) {
                    chunk.lights.push_back(light);
                }
                if (row.prefab != 0 && row.prefab < prefabCount) {
                    count += templates[row.prefab].size();
                }
            }
            chunk.count = count;
        }
    });
    // Where each chunk's draws go now. A kept chunk that moved, behind one
    // whose count changed, is written again at its new place.
    rewrite_.clear();
    std::size_t at = 0;
    std::size_t next = 0;
    for (std::size_t k = 0; k < chunkCount; ++k) {
        Chunk& chunk = chunks_[k];
        const bool counted = next < recount_.size() && recount_[next] == k;
        if (counted) {
            ++next;
        }
        if (counted || chunk.first != at) {
            rewrite_.push_back(k);
        }
        chunk.first = at;
        at += chunk.count;
    }
    draws.resize(at);
    ParallelBlocks(rewrite_.size(), kChunkGrain, [&](std::size_t, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const std::size_t k = rewrite_[i];
            MeshDraw* out = draws.data() + chunks_[k].first;
            const std::size_t rowEnd = std::min(rows.size(), (k + 1) * kRows);
            for (std::size_t index = k * kRows; index < rowEnd; ++index) {
                const engine_core::VisualInstance& row = rows[index];
                if (!row.alive || row.prefab == 0 || row.prefab >= prefabCount) {
                    continue;
                }
                out = WriteRowDraws(row, templates[row.prefab], out);
            }
        }
    });
    for (const std::size_t k : rewrite_) {
        Chunk& chunk = chunks_[k];
        chunk.rows = rows.shared_chunk(k);
        if (chunk.count == 0) {
            continue;
        }
        if (!written.empty() && written.back().first + written.back().count == chunk.first) {
            written.back().count += chunk.count;
        } else {
            written.push_back(DrawRange{chunk.first, chunk.count});
        }
    }
    for (const Chunk& chunk : chunks_) {
        lights.insert(lights.end(), chunk.lights.begin(), chunk.lights.end());
    }
    drawn_ = at;
}

void RowDrawCache::clear() {
    chunks_.clear();
    templates_.clear();
    generations_.clear();
    drawn_ = 0;
}

void AppendBrushDraws(const std::vector<engine_core::VisualBrushDraw>& brushes, MeshCache& meshes,
                      TextureCache& textures, std::vector<MeshDraw>& out) {
    for (const engine_core::VisualBrushDraw& source : brushes) {
        if (source.look.session == nullptr) {
            continue;
        }
        const anarchy::amesh::GpuMesh* mesh = meshes.getBrush(source.look.revision, *source.look.session);
        if (mesh == nullptr || source.lod >= mesh->lod_count()) {
            continue;
        }
        const engine_core::VisualMesh& look = source.look;
        MeshDraw& draw = out.emplace_back();
        draw.mesh = mesh;
        draw.model = source.world;
        draw.owner = source.owner;
        draw.texture = textures.get(look.diffuse_texture, Usage::Color, look.diffuse_flip_y, look.diffuse_always_loaded);
        draw.normalTexture = textures.get(look.normal_texture, Usage::Normal, look.normal_flip_y, look.normal_always_loaded);
        draw.roughnessTexture = textures.get(look.roughness_texture, Usage::Mask, look.roughness_flip_y, look.roughness_always_loaded);
        draw.metalnessTexture = textures.get(look.metalness_texture, Usage::Mask, look.metalness_flip_y, look.metalness_always_loaded);
        draw.emissiveTexture = textures.get(look.emissive_texture, Usage::Color, look.emissive_flip_y, look.emissive_always_loaded);
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
}

void SkyFor(const engine_core::VisualSky& sky, TextureCache& textures, SceneSky& out) {
    // The Skybox's images, uploaded linear; a missing or unreadable one draws no sky.
    if (!sky.present) {
        return;
    }
    const EnvironmentTexture image = textures.getEnvironment(sky.image, sky.image_flip_y);
    out.image = image.texture;
    out.imageRevision = image.revision;
    out.exposure = sky.exposure;
    out.lightScale = sky.light_scale;
    out.rotationDegrees = sky.rotation;
    out.tint[0] = sky.tint.r;
    out.tint[1] = sky.tint.g;
    out.tint[2] = sky.tint.b;
}

}  // namespace runner
