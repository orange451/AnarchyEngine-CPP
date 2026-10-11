#include "SceneDraws.hpp"

#include "Matrix4.hpp"
#include "amesh.hpp"

#include <algorithm>

namespace runner {

using engine_core::texture::Usage;

void LoadPrefabDraws(const engine_core::VisualPrefab& prefab, MeshCache& meshes, TextureCache& textures,
                     std::uint32_t& nextSlot, std::vector<MeshDraw>& out) {
    for (const engine_core::VisualMesh& source : prefab.meshes) {
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

void AppendRowDraws(const engine_core::VisualInstance& row, const std::vector<MeshDraw>& templates,
                    std::vector<MeshDraw>& out) {
    // The GameObject's Color tints each Material's, per instance, and its opacity multiplies each Material's.
    const float opacity = 1.f - row.transparency;
    // Its Scale grows the Prefab about the GameObject's origin: each axis, not the position.
    engine_core::Matrix4 scaled = row.world;
    for (int column = 0; column < 3; ++column) {
        for (int axis = 0; axis < 3; ++axis) {
            scaled.m[column * 4 + axis] *= row.scale;
        }
    }
    for (const MeshDraw& model : templates) {
        MeshDraw& draw = out.emplace_back(model);
        draw.model = scaled;
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
