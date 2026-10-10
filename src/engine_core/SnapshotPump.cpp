#include "SnapshotPump.hpp"

#include "AmbientOcclusionEffect.hpp"
#include "AssetInstances.hpp"
#include "BloomEffect.hpp"
#include "ColorSpace.hpp"
#include "Dragger.hpp"
#include "DynamicSky.hpp"
#include "Camera.hpp"
#include "GameObject.hpp"
#include "Gui.hpp"
#include "Light.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "PVInstance.hpp"
#include "ScreenSpaceReflections.hpp"
#include "Skybox.hpp"
#include "TerrainWorld.hpp"
#include "WireframeAdornment.hpp"

#include <algorithm>

namespace engine_core {
namespace {

static_assert(VisualLighting{}.exposure == static_cast<float>(Lighting::kDefaultExposure) &&
                  VisualLighting{}.saturation == static_cast<float>(Lighting::kDefaultSaturation) &&
                  VisualLighting{}.gamma == static_cast<float>(Lighting::kDefaultGamma) &&
                  VisualLighting{}.antialiasing == static_cast<int>(Lighting::kDefaultAntialiasing) &&
                  VisualLighting{}.terrain_quality == static_cast<int>(Lighting::kDefaultTerrainQuality) &&
                  VisualLighting{}.tone_mapping == static_cast<int>(Lighting::kDefaultToneMapping) &&
                  VisualLighting{}.shading_model == static_cast<int>(Lighting::kDefaultShadingModel),
              "a place with no Lighting draws with Lighting's defaults");

static_assert(VisualBloom{}.enabled == BloomEffect::kDefaultEnabled &&
                  VisualBloom{}.intensity == static_cast<float>(BloomEffect::kDefaultIntensity) &&
                  VisualBloom{}.size == static_cast<float>(BloomEffect::kDefaultSize) &&
                  VisualBloom{}.threshold == static_cast<float>(BloomEffect::kDefaultThreshold),
              "a place with no BloomEffect carries BloomEffect's defaults");

static_assert(VisualReflections{}.enabled == ScreenSpaceReflections::kDefaultEnabled &&
                  VisualReflections{}.intensity == static_cast<float>(ScreenSpaceReflections::kDefaultIntensity) &&
                  VisualReflections{}.max_distance == static_cast<float>(ScreenSpaceReflections::kDefaultMaxDistance) &&
                  VisualReflections{}.max_roughness == static_cast<float>(ScreenSpaceReflections::kDefaultMaxRoughness),
              "a place with no ScreenSpaceReflections carries its defaults");

static_assert(VisualAmbientOcclusion{}.enabled == AmbientOcclusionEffect::kDefaultEnabled &&
                  VisualAmbientOcclusion{}.intensity == static_cast<float>(AmbientOcclusionEffect::kDefaultIntensity) &&
                  VisualAmbientOcclusion{}.radius == static_cast<float>(AmbientOcclusionEffect::kDefaultRadius) &&
                  VisualAmbientOcclusion{}.quality == static_cast<int>(AmbientOcclusionEffect::kDefaultQuality),
              "a place with no AmbientOcclusionEffect carries its defaults");

float field_of_view_of(const SpatialObject& object) {
    const auto* camera = dynamic_cast<const Camera*>(&object);
    return camera != nullptr ? static_cast<float>(camera->field_of_view()) : 0.f;
}

float unit(double value) { return static_cast<float>(std::clamp(value, 0.0, 1.0)); }

VisualLight light_of(const SpatialObject& object) {
    VisualLight out;
    const auto* light = dynamic_cast<const Light*>(&object);
    if (light == nullptr) {
        return out;
    }
    out.kind = VisualLight::Kind::Point;
    out.enabled = light->enabled();
    const ColorRgb color = light->color();
    out.color[0] = srgb_to_linear(color.r);
    out.color[1] = srgb_to_linear(color.g);
    out.color[2] = srgb_to_linear(color.b);
    out.intensity = static_cast<float>(light->intensity());
    out.radius = static_cast<float>(light->radius());
    out.shadows = light->shadows();
    if (const auto* spot = dynamic_cast<const SpotLight*>(light)) {
        out.kind = VisualLight::Kind::Spot;
        out.outer_fov = static_cast<float>(spot->outer_fov());
        out.inner_fov_scale = static_cast<float>(spot->inner_fov_scale());
    }
    return out;
}

VisualLight light_of(const DirectionalLight& sun) {
    VisualLight out;
    out.kind = VisualLight::Kind::Directional;
    out.enabled = sun.enabled();
    const ColorRgb color = sun.color();
    out.color[0] = srgb_to_linear(color.r);
    out.color[1] = srgb_to_linear(color.g);
    out.color[2] = srgb_to_linear(color.b);
    out.intensity = static_cast<float>(sun.intensity());
    out.shadows = sun.shadows();
    out.shadow_distance = static_cast<float>(sun.shadow_distance());
    const Vec3 direction = sun.direction();
    out.direction[0] = direction.x;
    out.direction[1] = direction.y;
    out.direction[2] = direction.z;
    return out;
}

bool is_light(const DataModel* instance) {
    return dynamic_cast<const Light*>(instance) != nullptr || dynamic_cast<const DirectionalLight*>(instance) != nullptr;
}

// Whether id has a row: a SpatialObject or DirectionalLight in Workspace or Core,
// or any light under Lighting.
bool has_row(const DataModel& game, InstanceId id) {
    return game.in_workspace(id) || game.in_core(id) || (game.in_lighting(id) && is_light(game.instance(id)));
}

// A DirectionalLight's row: no Transform, no Prefab, only what it shines.
VisualInstance sun_row(const DirectionalLight& sun) {
    VisualInstance row;
    row.id = sun.id();
    row.alive = true;
    row.light = light_of(sun);
    return row;
}

// The live T that asset's reference at index holds, or null.
template <typename T>
const T* ReferencedAs(const DataModel& game, const ReferenceAsset& asset, std::size_t index) {
    const LuaSlot slot = asset.reference(index);
    return slot.kind == LuaSlot::Kind::Instance ? dynamic_cast<const T*>(game.instance(slot.id)) : nullptr;
}

}  // namespace

void SnapshotPump::reserve(std::size_t instances) {
    base_.instances.reserve(instances);
    buffers_[0].instances.reserve(instances);
    buffers_[1].instances.reserve(instances);
    overrides_.reserve(instances);
    // Keyed by slot, so it covers every slot whatever the row capacity.
    base_ids_.reserve(DataModel::kMaxInstances);
}

void SnapshotPump::begin_prerender_window(DataModel& game) {
    window_open_ = true;
    game.set_prerender_window(true);
}

void SnapshotPump::end_prerender_window(DataModel& game) {
    game.set_prerender_window(false);
    window_open_ = false;
}

void SnapshotPump::override_visual(const SnapshotOverride& override) {
    if (!window_open_ || thread_role() != ThreadRole::Render) {
        contract_fail("SnapshotOverride is only valid inside RenderStepped or PreRender");
    }
    if (overrides_.size() == overrides_.capacity()) {
        contract_fail("snapshot override capacity exhausted");
    }
    overrides_.push_back(override);
}

void SnapshotPump::set_camera(const Matrix4& camera) {
    if (!window_open_ || thread_role() != ThreadRole::Render) {
        contract_fail("camera snapshot writes happen inside RenderStepped or PreRender");
    }
    pending_camera_ = camera;
    camera_pending_ = true;
}

void SnapshotPump::set_terrain_world(const TerrainWorld* terrains) { terrain_world_ = terrains; }

VisualInstance* SnapshotPump::base_find(InstanceId id) {
    const int position = base_ids_.position(id);
    return position < 0 ? nullptr : &base_.instances[static_cast<std::size_t>(position)];
}

void SnapshotPump::erase_base(InstanceId id) {
    if (const VisualInstance* inst = base_find(id)) {
        release_prefab(inst->prefab);
    }
    const int position = base_ids_.erase(id);
    if (position < 0) {
        return;
    }
    // base_ids_ swapped its last id into position; mirror that on the rows.
    base_.instances[static_cast<std::size_t>(position)] = base_.instances.back();
    base_.instances.pop_back();
}

void SnapshotPump::apply_live(DataModel& game, const Invalidation& change) {
    if (any(change.fields, VisualField::Removed) || !game.alive(change.id)) {
        erase_base(change.id);
        return;
    }
    // Only SpatialObjects and DirectionalLights under Workspace, and lights under
    // Lighting, have rows. This drops the row of one that left, and ignores a
    // change to one that was never in.
    if (!has_row(game, change.id)) {
        erase_base(change.id);
        return;
    }
    const SpatialObject* object = game.spatial_object(change.id);
    if (object == nullptr) {
        const auto* sun = dynamic_cast<const DirectionalLight*>(game.instance(change.id));
        if (sun == nullptr) {
            return;
        }
        if (VisualInstance* row = base_find(change.id)) {
            row->light = light_of(*sun);
            return;
        }
        if (base_.instances.size() == base_.instances.capacity()) {
            contract_fail("snapshot instance capacity exhausted");
        }
        base_ids_.insert(change.id);
        base_.instances.push_back(sun_row(*sun));
        return;
    }
    // A row that joins reads every field: none was kept while it was out.
    bool whole = any(change.fields, VisualField::Ancestry);
    VisualInstance* inst = base_find(change.id);
    if (inst == nullptr) {
        if (base_.instances.size() == base_.instances.capacity()) {
            contract_fail("snapshot instance capacity exhausted");
        }
        base_ids_.insert(change.id);
        base_.instances.push_back(VisualInstance{});
        inst = &base_.instances.back();
        inst->id = change.id;
        whole = true;
    }
    if (whole || any(change.fields, VisualField::Transform)) {
        inst->world = object->transform();
        inst->transform_origin = change.origin;
    }
    // Only a GameObject draws: a Camera or a Light keeps a row's defaults.
    const auto* drawn = dynamic_cast<const GameObject*>(object);
    if (drawn != nullptr && (whole || any(change.fields, VisualField::Prefab))) {
        set_row_prefab(*inst, drawn->prefab_guid());
    }
    if (whole || any(change.fields, VisualField::Camera)) {
        inst->field_of_view = field_of_view_of(*object);
    }
    if (drawn != nullptr && (whole || any(change.fields, VisualField::Appearance))) {
        inst->color = drawn->color();
        inst->transparency = unit(drawn->transparency());
    }
    if (drawn != nullptr && (whole || any(change.fields, VisualField::Scale))) {
        inst->scale = static_cast<float>(drawn->scale());
    }
    if (whole || any(change.fields, VisualField::Light)) {
        inst->light = light_of(*object);
    }
    inst->alive = true;
}

void SnapshotPump::resync(DataModel& game) {
    base_.instances.clear();
    base_ids_.clear();
    prefab_entries_.clear();
    free_prefab_entries_.clear();
    prefab_by_guid_.clear();
    game.for_each_rendered([&](const SpatialObject& object) {
        VisualInstance inst;
        inst.id = object.id();
        inst.world = object.transform();
        inst.alive = true;
        inst.transform_origin = WriteOrigin::Simulation;
        if (const auto* drawn = dynamic_cast<const GameObject*>(&object)) {
            inst.prefab = acquire_prefab(drawn->prefab_guid());
            inst.color = drawn->color();
            inst.transparency = unit(drawn->transparency());
            inst.scale = static_cast<float>(drawn->scale());
        }
        inst.field_of_view = field_of_view_of(object);
        inst.light = light_of(object);
        base_ids_.insert(object.id());
        base_.instances.push_back(inst);
    });
    // The render query sees only SpatialObjects in Workspace. DirectionalLights
    // there, and every light under Lighting, are found by walking those two
    // services. A resync is rare, so the walk is cheap enough.
    std::vector<InstanceId> walk;
    for (const char* service : {"Workspace", "Lighting"}) {
        if (const InstanceId root = game.scene_service(service); root != 0) {
            walk.push_back(root);
        }
    }
    while (!walk.empty()) {
        const InstanceId id = walk.back();
        walk.pop_back();
        const DataModel* instance = game.instance(id);
        const bool sun = dynamic_cast<const DirectionalLight*>(instance) != nullptr;
        // A Light in Workspace or Core already has its row from the query.
        const bool lit =
            !game.in_workspace(id) && !game.in_core(id) && dynamic_cast<const Light*>(instance) != nullptr;
        if (sun || lit) {
            if (base_.instances.size() == base_.instances.capacity()) {
                contract_fail("snapshot instance capacity exhausted");
            }
            VisualInstance row;
            if (sun) {
                row = sun_row(*static_cast<const DirectionalLight*>(instance));
            } else {
                // Under Lighting: it shines.
                const auto& light = *static_cast<const Light*>(instance);
                row.id = id;
                row.world = light.transform();
                row.light = light_of(light);
            }
            base_ids_.insert(id);
            base_.instances.push_back(row);
        }
        for (InstanceId child = game.first_child(id); child != 0; child = game.next_sibling(child)) {
            walk.push_back(child);
        }
    }
}

std::uint32_t SnapshotPump::acquire_prefab(const std::string& guid) {
    if (guid.empty()) {
        return 0;
    }
    if (const auto found = prefab_by_guid_.find(guid); found != prefab_by_guid_.end()) {
        ++prefab_entries_[found->second].rows;
        return found->second;
    }
    if (prefab_entries_.empty()) {
        prefab_entries_.emplace_back();  // entry 0: no Prefab
    }
    std::uint32_t entry = 0;
    if (!free_prefab_entries_.empty()) {
        entry = free_prefab_entries_.back();
        free_prefab_entries_.pop_back();
    } else {
        entry = static_cast<std::uint32_t>(prefab_entries_.size());
        prefab_entries_.emplace_back();
    }
    PrefabEntry& slot = prefab_entries_[entry];
    slot.guid = guid;
    slot.cached = 0;
    slot.rows = 1;
    prefab_by_guid_.emplace(guid, entry);
    return entry;
}

void SnapshotPump::release_prefab(std::uint32_t entry) {
    if (entry == 0 || entry >= prefab_entries_.size()) {
        return;
    }
    PrefabEntry& slot = prefab_entries_[entry];
    if (slot.rows == 0 || --slot.rows != 0) {
        return;
    }
    prefab_by_guid_.erase(slot.guid);
    slot.guid.clear();
    slot.cached = 0;
    free_prefab_entries_.push_back(entry);
}

void SnapshotPump::set_row_prefab(VisualInstance& inst, const std::string& guid) {
    if (inst.prefab != 0 && prefab_entries_[inst.prefab].guid == guid) {
        return;
    }
    // Acquire first, so a row moving between two names of one entry keeps it alive.
    const std::uint32_t next = acquire_prefab(guid);
    release_prefab(inst.prefab);
    inst.prefab = next;
}

void fill_visual_material(const DataModel& game, const Material* material, VisualMesh& out) {
    // Assigned in place, so unchanged strings keep their buffers. Empty for no Material or no Texture.
    const auto texture_path = [&](std::size_t index, std::string& path, bool& flip_y, bool& always_loaded) {
        const Texture* texture = material != nullptr ? ReferencedAs<Texture>(game, *material, index) : nullptr;
        if (texture != nullptr) {
            path = texture->path();
            flip_y = texture->flip_y();
            always_loaded = texture->streaming() == TextureStreaming::AlwaysLoaded;
        } else {
            path.clear();
            flip_y = false;
            always_loaded = false;
        }
    };
    texture_path(Material::kDiffuseTextureReference, out.diffuse_texture, out.diffuse_flip_y, out.diffuse_always_loaded);
    texture_path(Material::kNormalTextureReference, out.normal_texture, out.normal_flip_y, out.normal_always_loaded);
    texture_path(Material::kRoughnessTextureReference, out.roughness_texture, out.roughness_flip_y, out.roughness_always_loaded);
    texture_path(Material::kMetalnessTextureReference, out.metalness_texture, out.metalness_flip_y, out.metalness_always_loaded);
    texture_path(Material::kEmissiveTextureReference, out.emissive_texture, out.emissive_flip_y, out.emissive_always_loaded);
    out.color = material != nullptr ? material->color() : ColorRgb{};
    out.emissive = material != nullptr ? material->emissive() : Material::kDefaultEmissive;
    out.metalness = unit(material != nullptr ? material->metalness() : Material::kDefaultMetalness);
    out.roughness = unit(material != nullptr ? material->roughness() : Material::kDefaultRoughness);
    out.reflectivity = unit(material != nullptr ? material->reflectivity() : Material::kDefaultReflectivity);
    out.transparency = unit(material != nullptr ? material->transparency() : Material::kDefaultTransparency);
}

void SnapshotPump::resolve_prefabs(DataModel& game) {
    base_.prefabs.resize(std::max<std::size_t>(prefab_entries_.size(), 1));
    for (std::size_t index = 1; index < prefab_entries_.size(); ++index) {
        PrefabEntry& entry = prefab_entries_[index];
        std::vector<VisualMesh>& meshes = base_.prefabs[index].meshes;
        std::size_t used = 0;
        if (entry.rows != 0) {
            // A Prefab undone, loaded, or brought back by Stop holds its GUID again, maybe under a new id.
            if (entry.cached == 0 || !game.alive(entry.cached) || game.guid(entry.cached) != entry.guid) {
                const std::optional<InstanceId> found = game.find_guid(entry.guid);
                entry.cached = found ? *found : 0;
            }
            if (dynamic_cast<const Prefab*>(game.instance(entry.cached)) != nullptr) {
                for (InstanceId child = game.first_child(entry.cached); child != 0; child = game.next_sibling(child)) {
                    const auto* model = dynamic_cast<const Model*>(game.instance(child));
                    if (model == nullptr) {
                        continue;
                    }
                    const LuaSlot mesh_ref = model->reference(Model::kMeshReference);
                    const auto* mesh = mesh_ref.kind == LuaSlot::Kind::Instance
                                           ? dynamic_cast<const Mesh*>(game.instance(mesh_ref.id))
                                           : nullptr;
                    if (mesh == nullptr) {
                        continue;
                    }
                    Mesh::SessionGeometry session = mesh->session_geometry();
                    if (session.data == nullptr && mesh->path().empty()) {
                        continue;
                    }
                    if (used == meshes.size()) {
                        meshes.emplace_back();
                    }
                    // Assigned in place, so an unchanged Prefab reuses last frame's strings.
                    VisualMesh& out = meshes[used++];
                    out.mesh = mesh->id();
                    out.revision = session.revision;
                    if (session.data != nullptr) {
                        out.path.clear();
                        out.session = std::move(session.data);
                    } else {
                        out.path = mesh->path();
                        out.session.reset();
                    }
                    fill_visual_material(game, ReferencedAs<Material>(game, *model, Model::kMaterialReference), out);
                }
            }
        }
        meshes.resize(used);
    }
}

template <class... T>
const DataModel* SnapshotPump::find_first_of(const DataModel& game, InstanceId root) {
    // Children are pushed last first, so the first child comes off the walk first.
    lighting_walk_.clear();
    lighting_walk_.push_back(root);
    while (!lighting_walk_.empty()) {
        const InstanceId id = lighting_walk_.back();
        lighting_walk_.pop_back();
        if (id != root) {
            const DataModel* object = game.instance(id);
            if (object != nullptr && (... || (dynamic_cast<const T*>(object) != nullptr))) {
                return object;
            }
        }
        const std::size_t first = lighting_walk_.size();
        for (InstanceId child = game.first_child(id); child != 0; child = game.next_sibling(child)) {
            lighting_walk_.push_back(child);
        }
        std::reverse(lighting_walk_.begin() + static_cast<std::ptrdiff_t>(first), lighting_walk_.end());
    }
    return nullptr;
}

template <class T>
const T* SnapshotPump::find_first(const DataModel& game, InstanceId root) {
    return static_cast<const T*>(find_first_of<T>(game, root));
}

void SnapshotPump::resolve_lighting(DataModel& game) {
    const auto* lighting = dynamic_cast<const Lighting*>(game.instance(game.scene_service("Lighting")));
    VisualSky& sky = base_.sky;
    // The first Skybox or DynamicSky in tree order is the sky; the other kind is not drawn.
    const DataModel* first_sky =
        lighting != nullptr ? find_first_of<Skybox, DynamicSky>(game, lighting->id()) : nullptr;
    const auto* skybox = dynamic_cast<const Skybox*>(first_sky);
    const auto* dynamic = dynamic_cast<const DynamicSky*>(first_sky);
    sky.present = skybox != nullptr;
    // Assigned in place, so an unchanged sky reuses last frame's strings.
    const auto texture_path = [&](const LuaSlot& slot, std::string& path, bool& flip_y, bool& always_loaded) {
        const auto* texture =
            slot.kind == LuaSlot::Kind::Instance ? dynamic_cast<const Texture*>(game.instance(slot.id)) : nullptr;
        if (texture != nullptr) {
            path = texture->path();
            flip_y = texture->flip_y();
            always_loaded = texture->streaming() == TextureStreaming::AlwaysLoaded;
        } else {
            path.clear();
            flip_y = false;
            always_loaded = false;
        }
    };
    if (skybox != nullptr) {
        bool image_always_loaded = false;   // a sky is uploaded whole; it never streams
        texture_path(skybox->image(), sky.image, sky.image_flip_y, image_always_loaded);
        sky.exposure = static_cast<float>(skybox->exposure());
        sky.light_scale = static_cast<float>(skybox->light_scale());
        sky.rotation = static_cast<float>(skybox->rotation());
        sky.tint = skybox->tint();
    } else {
        sky.image.clear();
        sky.image_flip_y = false;
        sky.exposure = static_cast<float>(Skybox::kDefaultExposure);
        sky.light_scale = static_cast<float>(Skybox::kDefaultLightScale);
        sky.rotation = static_cast<float>(Skybox::kDefaultRotation);
        sky.tint = Skybox::kDefaultTint;
    }

    VisualDynamicSky& procedural = base_.dynamic_sky;
    procedural.present = dynamic != nullptr;
    if (dynamic != nullptr) {
        procedural.time_of_day = static_cast<float>(dynamic->time_of_day());
        procedural.latitude = static_cast<float>(dynamic->latitude());
        procedural.brightness = static_cast<float>(dynamic->brightness());
        procedural.shadows = dynamic->shadows();
        procedural.cloud_cover = static_cast<float>(dynamic->cloud_cover());
        procedural.cloud_density = static_cast<float>(dynamic->cloud_density());
        procedural.wind = dynamic->wind_direction();
        texture_path(dynamic->sun_texture(), procedural.sun_texture, procedural.sun_flip_y, procedural.sun_always_loaded);
        texture_path(dynamic->moon_texture(), procedural.moon_texture, procedural.moon_flip_y, procedural.moon_always_loaded);
        procedural.sun_size = static_cast<float>(dynamic->sun_size());
        procedural.moon_size = static_cast<float>(dynamic->moon_size());
        procedural.reflection_quality = static_cast<int>(dynamic->reflection_quality());
    } else {
        // Field by field, so the strings keep their buffers.
        const VisualDynamicSky defaults;
        procedural.time_of_day = defaults.time_of_day;
        procedural.latitude = defaults.latitude;
        procedural.brightness = defaults.brightness;
        procedural.shadows = defaults.shadows;
        procedural.cloud_cover = defaults.cloud_cover;
        procedural.cloud_density = defaults.cloud_density;
        procedural.wind = defaults.wind;
        procedural.sun_texture.clear();
        procedural.moon_texture.clear();
        procedural.sun_flip_y = false;
        procedural.moon_flip_y = false;
        procedural.sun_always_loaded = false;
        procedural.moon_always_loaded = false;
        procedural.sun_size = defaults.sun_size;
        procedural.moon_size = defaults.moon_size;
        procedural.reflection_quality = defaults.reflection_quality;
    }

    const BloomEffect* effect = lighting != nullptr ? find_first<BloomEffect>(game, lighting->id()) : nullptr;
    VisualBloom& bloom = base_.bloom;
    bloom = VisualBloom{};
    if (effect != nullptr) {
        bloom.present = true;
        bloom.enabled = effect->enabled();
        bloom.intensity = static_cast<float>(effect->intensity());
        bloom.size = static_cast<float>(effect->size());
        bloom.threshold = static_cast<float>(effect->threshold());
    }
    const ScreenSpaceReflections* traced =
        lighting != nullptr ? find_first<ScreenSpaceReflections>(game, lighting->id()) : nullptr;
    VisualReflections& reflections = base_.reflections;
    reflections = VisualReflections{};
    if (traced != nullptr) {
        reflections.present = true;
        reflections.enabled = traced->enabled();
        reflections.intensity = static_cast<float>(traced->intensity());
        reflections.max_distance = static_cast<float>(traced->max_distance());
        reflections.max_roughness = static_cast<float>(traced->max_roughness());
    }
    const AmbientOcclusionEffect* shading =
        lighting != nullptr ? find_first<AmbientOcclusionEffect>(game, lighting->id()) : nullptr;
    VisualAmbientOcclusion& occlusion = base_.occlusion;
    occlusion = VisualAmbientOcclusion{};
    if (shading != nullptr) {
        occlusion.present = true;
        occlusion.enabled = shading->enabled();
        occlusion.intensity = static_cast<float>(shading->intensity());
        occlusion.radius = static_cast<float>(shading->radius());
        occlusion.quality = static_cast<int>(shading->quality());
    }

    if (lighting == nullptr) {
        base_.lighting = VisualLighting{};
        return;
    }
    base_.lighting.ambient = lighting->ambient();
    base_.lighting.exposure = static_cast<float>(lighting->exposure());
    base_.lighting.saturation = static_cast<float>(lighting->saturation());
    base_.lighting.gamma = static_cast<float>(lighting->gamma());
    base_.lighting.antialiasing = static_cast<int>(lighting->antialiasing());
    base_.lighting.terrain_quality = static_cast<int>(lighting->terrain_quality());
    base_.lighting.tone_mapping = static_cast<int>(lighting->tone_mapping());
    base_.lighting.shading_model = static_cast<int>(lighting->shading_model());
}


void SnapshotPump::blit(VisualSnapshot& dst) const {
    dst.camera = base_.camera;
    dst.lighting = base_.lighting;
    dst.sky = base_.sky;
    dst.dynamic_sky = base_.dynamic_sky;
    dst.bloom = base_.bloom;
    dst.reflections = base_.reflections;
    dst.occlusion = base_.occlusion;
    dst.resources_root = base_.resources_root;
    dst.draggers = base_.draggers;
    dst.billboards = base_.billboards;
    dst.wire_lines = base_.wire_lines;
    dst.terrains = base_.terrains;
    dst.brushes = base_.brushes;
    dst.instances.resize(base_.instances.size());
    std::copy(base_.instances.begin(), base_.instances.end(), dst.instances.begin());
    // Element by element, so strings that did not change keep their buffers.
    dst.prefabs.resize(base_.prefabs.size());
    std::copy(base_.prefabs.begin(), base_.prefabs.end(), dst.prefabs.begin());
}

void SnapshotPump::apply_overrides(VisualSnapshot& dst) {
    // dst is a copy of base_, so base_ids_ gives each instance's position.
    for (const SnapshotOverride& override : overrides_) {
        const int position = base_ids_.position(override.id);
        if (position < 0 || static_cast<std::size_t>(position) >= dst.instances.size()) {
            continue;
        }
        VisualInstance& inst = dst.instances[static_cast<std::size_t>(position)];
        if (inst.id != override.id || !inst.alive) {
            continue;
        }
        if (any(override.field, VisualField::Transform)) {
            inst.world = override.transform;
            inst.transform_origin = WriteOrigin::SnapshotOverride;
        }
    }
}

void SnapshotPump::prepare_copy(DataModel& game) {
    take_changes(game);
    finish_copy();
}

void SnapshotPump::resolve_draggers(DataModel& game) {
    base_.draggers.clear();
    game.draggers(dragger_ids_);
    for (InstanceId id : dragger_ids_) {
        const auto* dragger = dynamic_cast<const Dragger*>(game.instance(id));
        if (dragger == nullptr) {
            continue;
        }
        VisualDragger row;
        row.frame = dragger_frame(dragger->transform(), dragger->local_space());
        row.hovered = dragger->hovered();
        row.active = dragger->active_handle();
        base_.draggers.push_back(row);
    }
}

void SnapshotPump::resolve_billboards(DataModel& game) {
    base_.billboards.clear();
    game.billboards(billboard_ids_);
    // In id order, so the rows keep one order from frame to frame.
    std::sort(billboard_ids_.begin(), billboard_ids_.end());
    for (InstanceId id : billboard_ids_) {
        const auto* board = dynamic_cast<const BillboardGui*>(game.instance(id));
        if (board == nullptr || !board->drawn() || !board->flag(GuiProperty::Visible)) {
            continue;
        }
        VisualBillboard row;
        row.id = id;
        row.anchor_instance = board->anchor_instance();
        row.anchor = board->anchor_of(row.anchor_instance);
        row.always_on_top = board->always_on_top();
        base_.billboards.push_back(row);
    }
}

void SnapshotPump::resolve_wireframes(DataModel& game) {
    std::vector<float>& out = base_.wire_lines;
    out.clear();
    game.wireframes(wireframe_ids_);
    // In id order, so the lines keep one order from frame to frame.
    std::sort(wireframe_ids_.begin(), wireframe_ids_.end());
    for (InstanceId id : wireframe_ids_) {
        const auto* wire = dynamic_cast<const WireframeAdornment*>(game.instance(id));
        if (wire == nullptr || !wire->drawn() || wire->lines().empty()) {
            continue;
        }
        Matrix4 space = matrix4_identity();
        if (const InstanceId adornee = wire->adornee_id(); adornee != 0) {
            if (const auto* placed = dynamic_cast<const PVInstance*>(game.instance(adornee))) {
                space = placed->transform();
            }
        }
        const float alpha = 1.f - static_cast<float>(wire->transparency());
        const ColorRgb shared = wire->color();
        out.reserve(out.size() + wire->lines().size() * 14);
        for (const WireframeAdornment::Line& line : wire->lines()) {
            const ColorRgb c = line.own_color ? line.color : shared;
            for (const Vec3 local : {line.from, line.to}) {
                const Vec3 p = matrix4_point(space, local);
                out.insert(out.end(), {p.x, p.y, p.z, c.r, c.g, c.b, alpha});
            }
        }
    }
}

void SnapshotPump::resolve_terrains(DataModel& game) {
    (void)game;  // Called from RenderThread's snapshot copy, under the DataModel write
    // lock; views() is read under that same write lock that TerrainWorld::update also
    // takes, so nothing more to read here.
    base_.terrains.clear();
    if (terrain_world_ == nullptr) {
        return;
    }
    // Copies TerrainView by value -- Matrix4 and a couple of scalars plus two
    // shared_ptrs -- so this is a pointer-only copy, not a chunk/look copy.
    const std::vector<TerrainView>& views = terrain_world_->views();
    base_.terrains.assign(views.begin(), views.end());
}

void SnapshotPump::anchor_billboards(VisualSnapshot& dst) const {
    for (VisualBillboard& row : dst.billboards) {
        if (row.anchor_instance == 0) {
            continue;
        }
        const int position = base_ids_.position(row.anchor_instance);
        if (position < 0 || static_cast<std::size_t>(position) >= dst.instances.size()) {
            continue;
        }
        const VisualInstance& inst = dst.instances[static_cast<std::size_t>(position)];
        if (inst.id == row.anchor_instance && inst.alive) {
            row.anchor = Vec3{inst.world.m[12], inst.world.m[13], inst.world.m[14]};
        }
    }
}

void SnapshotPump::take_changes(DataModel& game) {
    InvalidationQueue& queue = game.invalidations();
    if (queue.take_overflow() || game.consume_resync()) {
        resync(game);
    } else {
        queue.drain([&](const Invalidation& change) { apply_live(game, change); });
    }
    resolve_prefabs(game);
    resolve_lighting(game);
    resolve_draggers(game);
    resolve_billboards(game);
    resolve_wireframes(game);
    resolve_terrains(game);
    brushes_.update(game, base_.brushes);
    base_.resources_root = game.resources_root();
    if (camera_pending_) {
        base_.camera = pending_camera_;
        camera_pending_ = false;
    }
}

void SnapshotPump::finish_copy() {
    VisualSnapshot& back = buffers_[1 - front_];
    blit(back);
    apply_overrides(back);
    anchor_billboards(back);
    back.frame = next_frame_++;
    back.camera = base_.camera;
    overrides_.clear();
}

void SnapshotPump::publish() {
    front_ ^= 1;
    published_frame_.store(buffers_[front_].frame);
}

const VisualSnapshot& SnapshotPump::front() const { return buffers_[front_]; }

const VisualInstance* SnapshotPump::find(InstanceId id) const {
    for (const VisualInstance& inst : buffers_[front_].instances) {
        if (inst.alive && inst.id == id) {
            return &inst;
        }
    }
    return nullptr;
}

}  // namespace engine_core
