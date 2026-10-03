#include "SnapshotPump.hpp"

#include "AssetInstances.hpp"
#include "Dragger.hpp"
#include "PVInstance.hpp"
#include "Camera.hpp"
#include "GameObject.hpp"
#include "Light.hpp"
#include "Lighting.hpp"
#include "LuaApi.hpp"
#include "Skybox.hpp"

#include <algorithm>

namespace engine_core {
namespace {

static_assert(VisualLighting{}.exposure == static_cast<float>(Lighting::kDefaultExposure) &&
                  VisualLighting{}.saturation == static_cast<float>(Lighting::kDefaultSaturation) &&
                  VisualLighting{}.gamma == static_cast<float>(Lighting::kDefaultGamma),
              "a place with no Lighting draws with Lighting's defaults");

float field_of_view_of(const GameObject& object) {
    const auto* camera = dynamic_cast<const Camera*>(&object);
    return camera != nullptr ? static_cast<float>(camera->field_of_view()) : 0.f;
}

float unit(double value) { return static_cast<float>(std::clamp(value, 0.0, 1.0)); }

VisualLight light_of(const GameObject& object) {
    VisualLight out;
    const auto* light = dynamic_cast<const Light*>(&object);
    if (light == nullptr) {
        return out;
    }
    out.kind = VisualLight::Kind::Point;
    out.enabled = light->enabled();
    const ColorRgb color = light->color();
    out.color[0] = color.r;
    out.color[1] = color.g;
    out.color[2] = color.b;
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
    out.color[0] = color.r;
    out.color[1] = color.g;
    out.color[2] = color.b;
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

// Whether id has a row: a GameObject or DirectionalLight in Workspace or Core,
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
    // Only GameObjects and DirectionalLights under Workspace, and lights under
    // Lighting, have rows. This drops the row of one that left, and ignores a
    // change to one that was never in.
    if (!has_row(game, change.id)) {
        erase_base(change.id);
        return;
    }
    const GameObject* object = game.game_object(change.id);
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
    if (whole || any(change.fields, VisualField::Prefab)) {
        // A light under Lighting only shines: Lighting is not part of the scene.
        static const std::string kNoPrefab;
        const bool scene = game.in_workspace(change.id) || game.in_core(change.id);
        set_row_prefab(*inst, scene ? object->prefab_guid() : kNoPrefab);
    }
    if (whole || any(change.fields, VisualField::Camera)) {
        inst->field_of_view = field_of_view_of(*object);
    }
    if (whole || any(change.fields, VisualField::Appearance)) {
        inst->color = object->color();
        inst->transparency = unit(object->transparency());
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
    game.for_each_rendered([&](const GameObject& object) {
        VisualInstance inst;
        inst.id = object.id();
        inst.world = object.transform();
        inst.alive = true;
        inst.transform_origin = WriteOrigin::Simulation;
        inst.prefab = acquire_prefab(object.prefab_guid());
        inst.field_of_view = field_of_view_of(object);
        inst.color = object.color();
        inst.transparency = unit(object.transparency());
        inst.light = light_of(object);
        base_ids_.insert(object.id());
        base_.instances.push_back(inst);
    });
    // The render query sees only GameObjects in Workspace. DirectionalLights
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
        // A light GameObject in Workspace or Core already has its row from the query.
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
                // Under Lighting: it shines, and draws no Prefab.
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
                    const Material* material = ReferencedAs<Material>(game, *model, Model::kMaterialReference);
                    // Assigned in place, as above. Empty for no Material or no Texture.
                    const auto texture_path = [&](std::size_t index, std::string& path) {
                        const Texture* texture =
                            material != nullptr ? ReferencedAs<Texture>(game, *material, index) : nullptr;
                        if (texture != nullptr) {
                            path = texture->path();
                        } else {
                            path.clear();
                        }
                    };
                    texture_path(Material::kDiffuseTextureReference, out.diffuse_texture);
                    texture_path(Material::kNormalTextureReference, out.normal_texture);
                    texture_path(Material::kRoughnessTextureReference, out.roughness_texture);
                    texture_path(Material::kMetalnessTextureReference, out.metalness_texture);
                    out.color = material != nullptr ? material->color() : ColorRgb{};
                    out.emissive = material != nullptr ? material->emissive() : Material::kDefaultEmissive;
                    out.metalness = unit(material != nullptr ? material->metalness() : Material::kDefaultMetalness);
                    out.roughness = unit(material != nullptr ? material->roughness() : Material::kDefaultRoughness);
                    out.reflectivity =
                        unit(material != nullptr ? material->reflectivity() : Material::kDefaultReflectivity);
                    out.transparency =
                        unit(material != nullptr ? material->transparency() : Material::kDefaultTransparency);
                }
            }
        }
        meshes.resize(used);
    }
}

void SnapshotPump::resolve_lighting(DataModel& game) {
    const auto* lighting = dynamic_cast<const Lighting*>(game.instance(game.scene_service("Lighting")));
    VisualSky& sky = base_.sky;
    const Skybox* skybox = lighting != nullptr ? find_skybox(game, lighting->id()) : nullptr;
    sky.present = skybox != nullptr;
    // Assigned in place, so an unchanged sky reuses last frame's strings.
    const auto texture_path = [&](const LuaSlot& slot, std::string& path) {
        const auto* texture =
            slot.kind == LuaSlot::Kind::Instance ? dynamic_cast<const Texture*>(game.instance(slot.id)) : nullptr;
        if (texture != nullptr) {
            path = texture->path();
        } else {
            path.clear();
        }
    };
    if (skybox != nullptr) {
        texture_path(skybox->image(), sky.image);
        sky.exposure = static_cast<float>(skybox->exposure());
        sky.light_scale = static_cast<float>(skybox->light_scale());
        sky.rotation = static_cast<float>(skybox->rotation());
        sky.tint = skybox->tint();
    } else {
        sky.image.clear();
        sky.exposure = static_cast<float>(Skybox::kDefaultExposure);
        sky.light_scale = static_cast<float>(Skybox::kDefaultLightScale);
        sky.rotation = static_cast<float>(Skybox::kDefaultRotation);
        sky.tint = Skybox::kDefaultTint;
    }

    if (lighting == nullptr) {
        base_.lighting = VisualLighting{};
        return;
    }
    base_.lighting.ambient = lighting->ambient();
    base_.lighting.exposure = static_cast<float>(lighting->exposure());
    base_.lighting.saturation = static_cast<float>(lighting->saturation());
    base_.lighting.gamma = static_cast<float>(lighting->gamma());
}

const Skybox* SnapshotPump::find_skybox(const DataModel& game, InstanceId root) {
    // Children are pushed last first, so the first child comes off the walk first.
    sky_walk_.clear();
    sky_walk_.push_back(root);
    while (!sky_walk_.empty()) {
        const InstanceId id = sky_walk_.back();
        sky_walk_.pop_back();
        if (id != root) {
            if (const auto* skybox = dynamic_cast<const Skybox*>(game.instance(id))) {
                return skybox;
            }
        }
        const std::size_t first = sky_walk_.size();
        for (InstanceId child = game.first_child(id); child != 0; child = game.next_sibling(child)) {
            sky_walk_.push_back(child);
        }
        std::reverse(sky_walk_.begin() + static_cast<std::ptrdiff_t>(first), sky_walk_.end());
    }
    return nullptr;
}

void SnapshotPump::blit(VisualSnapshot& dst) const {
    dst.camera = base_.camera;
    dst.lighting = base_.lighting;
    dst.sky = base_.sky;
    dst.resources_root = base_.resources_root;
    dst.draggers = base_.draggers;
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
        const auto* target = dragger != nullptr ? dynamic_cast<const PVInstance*>(game.instance(dragger->target())) : nullptr;
        if (target == nullptr) {
            continue;
        }
        VisualDragger row;
        row.frame = dragger_frame(target->transform(), dragger->local_space());
        row.hovered = dragger->hovered();
        row.active = dragger->active_handle();
        base_.draggers.push_back(row);
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
