#include "ViewportScenes.hpp"

#include "Brush.hpp"
#include "Camera.hpp"
#include "GameObject.hpp"
#include "Gui.hpp"
#include "Light.hpp"
#include "Skybox.hpp"

#include <algorithm>
#include <cstring>

namespace engine_core {
namespace {

bool same_matrix(const Matrix4& a, const Matrix4& b) { return std::memcmp(a.m, b.m, sizeof a.m) == 0; }

bool same_mesh(const VisualMesh& a, const VisualMesh& b) {
    return a.path == b.path && a.session == b.session && a.revision == b.revision && a.skinned == b.skinned &&
           a.mesh == b.mesh && a.diffuse_texture == b.diffuse_texture && same_color(a.color, b.color) &&
           a.normal_texture == b.normal_texture && a.roughness_texture == b.roughness_texture &&
           a.metalness_texture == b.metalness_texture && a.emissive_texture == b.emissive_texture &&
           a.diffuse_flip_y == b.diffuse_flip_y && a.normal_flip_y == b.normal_flip_y &&
           a.roughness_flip_y == b.roughness_flip_y && a.metalness_flip_y == b.metalness_flip_y &&
           a.emissive_flip_y == b.emissive_flip_y && a.diffuse_always_loaded == b.diffuse_always_loaded &&
           a.normal_always_loaded == b.normal_always_loaded && a.roughness_always_loaded == b.roughness_always_loaded &&
           a.metalness_always_loaded == b.metalness_always_loaded &&
           a.emissive_always_loaded == b.emissive_always_loaded && same_color(a.emissive, b.emissive) &&
           a.metalness == b.metalness && a.roughness == b.roughness && a.reflectivity == b.reflectivity &&
           a.transparency == b.transparency;
}

bool same_brushes(const std::vector<VisualBrushDraw>& a, const std::vector<VisualBrushDraw>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].owner != b[i].owner || a[i].lod != b[i].lod || !same_matrix(a[i].world, b[i].world) ||
            a[i].transparency != b[i].transparency || a[i].casts_shadow != b[i].casts_shadow ||
            !same_color(a[i].tint, b[i].tint) || !same_mesh(a[i].look, b[i].look)) {
            return false;
        }
    }
    return true;
}

bool same_meshes(const std::vector<VisualMesh>& a, const std::vector<VisualMesh>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same_mesh(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

bool same_lod(const VisualLod& a, const VisualLod& b) {
    return a.distance == b.distance && a.kind == b.kind && same_meshes(a.meshes, b.meshes) &&
           same_mesh(a.card, b.card) && a.width == b.width && a.height == b.height;
}

bool same_prefab(const VisualPrefab& a, const VisualPrefab& b) {
    if (a.skeleton != b.skeleton || !same_meshes(a.meshes, b.meshes) || a.lods.size() != b.lods.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.lods.size(); ++i) {
        if (!same_lod(a.lods[i], b.lods[i])) {
            return false;
        }
    }
    return true;
}

bool same_light(const VisualLight& a, const VisualLight& b) {
    return a.kind == b.kind && a.enabled == b.enabled && std::equal(a.color, a.color + 3, b.color) &&
           a.intensity == b.intensity && a.radius == b.radius && a.outer_fov == b.outer_fov &&
           a.inner_fov_scale == b.inner_fov_scale && a.shadows == b.shadows &&
           a.shadow_distance == b.shadow_distance && std::equal(a.direction, a.direction + 3, b.direction);
}

bool same_row(const VisualInstance& a, const VisualInstance& b) {
    return a.id == b.id && same_matrix(a.world, b.world) && a.prefab == b.prefab && same_color(a.color, b.color) &&
           a.transparency == b.transparency && a.scale == b.scale && a.pose == b.pose && same_light(a.light, b.light);
}

bool same_sky(const VisualSky& a, const VisualSky& b) {
    return a.present == b.present && a.image == b.image && a.image_flip_y == b.image_flip_y &&
           a.exposure == b.exposure && a.light_scale == b.light_scale && a.rotation == b.rotation &&
           same_color(a.tint, b.tint);
}

// Everything but the revision: what a pane draws.
bool same_scene(const VisualViewport& a, const VisualViewport& b) {
    if (a.camera != b.camera || a.has_camera != b.has_camera || a.camera_inside != b.camera_inside ||
        !same_matrix(a.camera_world, b.camera_world) || a.field_of_view != b.field_of_view ||
        !same_color(a.ambient, b.ambient) || !same_sky(a.sky, b.sky) || a.rows.size() != b.rows.size() ||
        a.prefabs != b.prefabs || !same_brushes(a.brushes, b.brushes)) {
        return false;
    }
    for (std::size_t i = 0; i < a.rows.size(); ++i) {
        if (!same_row(a.rows[i], b.rows[i])) {
            return false;
        }
    }
    return true;
}

// The ViewportPane id is inside, at any depth, or 0.
InstanceId owning_pane(const DataModel& game, InstanceId id) {
    for (InstanceId above = game.parent(id); above != DataModel::kNoParent && above != 0; above = game.parent(above)) {
        const DataModel* object = game.instance(above);
        if (object != nullptr && object->viewport_pane()) {
            return above;
        }
    }
    return 0;
}

}  // namespace

ViewportScenes::ViewportScenes() {
    pane_ids_.reserve(64);
    dirty_.reserve(256);
    walk_.reserve(256);
    scratch_.rows.reserve(64);
    scratch_.prefabs.reserve(16);
    prefabs_.reserve(64);
}

void ViewportScenes::note(const DataModel& game, InstanceId id) {
    if (!game.alive(id) || !game.in_viewport(id)) {
        return;
    }
    if (const InstanceId pane = owning_pane(game, id); pane != 0) {
        dirty_.push_back(pane);
    }
}

std::uint32_t ViewportScenes::find_prefab(const DataModel& game, const std::string& guid) {
    if (const auto found = prefab_by_guid_.find(guid); found != prefab_by_guid_.end()) {
        return found->second;
    }
    std::uint32_t index = 0;
    if (!free_prefabs_.empty()) {
        index = free_prefabs_.back();
        free_prefabs_.pop_back();
    } else {
        index = static_cast<std::uint32_t>(prefabs_.size());
        prefabs_.emplace_back();
    }
    Prefab& entry = prefabs_[index];
    entry.guid = guid;
    entry.cached = 0;
    entry.panes = 0;
    VisualPrefab made;
    fill_visual_prefab(game, guid, entry.cached, made);
    entry.current = std::make_shared<const VisualPrefab>(std::move(made));
    ++entry.revision;
    prefab_by_guid_.emplace(guid, index);
    return index;
}

void ViewportScenes::release_prefabs(Pane& pane) {
    for (const std::uint32_t index : pane.prefabs) {
        if (index < prefabs_.size() && prefabs_[index].panes > 0) {
            --prefabs_[index].panes;
        }
    }
    pane.prefabs.clear();
    pane.prefab_revisions.clear();
}

void ViewportScenes::refresh_prefabs(const DataModel& game) {
    for (Prefab& entry : prefabs_) {
        if (entry.panes == 0 || entry.guid.empty()) {
            continue;
        }
        // Assigned in place, so an unchanged Prefab reuses last update's strings.
        fill_visual_prefab(game, entry.guid, entry.cached, prefab_scratch_);
        if (entry.current == nullptr || !same_prefab(prefab_scratch_, *entry.current)) {
            entry.current = std::make_shared<const VisualPrefab>(prefab_scratch_);
            ++entry.revision;
        }
    }
}

bool ViewportScenes::outside_changed(const DataModel& game, const ViewportPane& pane, const Pane& entry) {
    const VisualViewport& last = *entry.published;
    InstanceId camera = pane.current_camera_id();
    if (camera == 0) {
        camera = entry.first_camera;
    }
    if (camera != last.camera) {
        return true;
    }
    if (camera != 0) {
        const auto* object = dynamic_cast<const Camera*>(game.instance(camera));
        if (object == nullptr || !same_matrix(object->transform(), last.camera_world) ||
            static_cast<float>(object->field_of_view()) != last.field_of_view) {
            return true;
        }
    }
    if (entry.sky != 0) {
        const auto* sky = dynamic_cast<const Skybox*>(game.instance(entry.sky));
        if (sky == nullptr) {
            return true;
        }
        fill_visual_sky(game, *sky, sky_scratch_);
        sky_scratch_.present = true;
        if (!same_sky(sky_scratch_, last.sky)) {
            return true;
        }
    }
    for (std::size_t k = 0; k < entry.prefabs.size(); ++k) {
        if (prefabs_[entry.prefabs[k]].revision != entry.prefab_revisions[k]) {
            return true;
        }
    }
    for (const std::uint32_t index : entry.skinned_rows) {
        const VisualInstance& row = last.rows[index];
        const auto* object = dynamic_cast<const GameObject*>(game.instance(row.id));
        if (object == nullptr || object->pose() != row.pose) {
            return true;
        }
    }
    // A Brush's shape, color, transparency, Transform, or Materials: read again and compared.
    if (!entry.brushes.empty()) {
        brush_scratch_.clear();
        for (const InstanceId id : entry.brushes) {
            const auto* brush = dynamic_cast<const Brush*>(game.instance(id));
            if (brush == nullptr) {
                return true;
            }
            brush_visuals_.append_single(game, *brush, brush_scratch_);
        }
        if (!same_brushes(brush_scratch_, last.brushes)) {
            return true;
        }
    }
    return false;
}

void ViewportScenes::build(const DataModel& game, const ViewportPane& pane, Pane& entry) {
    VisualViewport& out = scratch_;
    out.pane = pane.id();
    out.ambient = pane.ambient();
    out.rows.clear();
    out.prefabs.clear();
    out.prefabs.push_back(nullptr);
    out.sky = VisualSky{};
    out.brushes.clear();
    out.camera = 0;
    out.has_camera = false;
    out.camera_inside = false;
    out.camera_world = matrix4_identity();
    out.field_of_view = 70.f;
    release_prefabs(entry);
    entry.skinned_rows.clear();
    entry.brushes.clear();
    entry.first_camera = 0;
    entry.sky = 0;

    // Depth first in child order: children are pushed last first. A nested
    // ViewportPane's contents are its own, so its children are not walked.
    walk_.clear();
    const auto push_children = [&](InstanceId id) {
        const std::size_t first = walk_.size();
        for (InstanceId child = game.first_child(id); child != 0; child = game.next_sibling(child)) {
            walk_.push_back(child);
        }
        std::reverse(walk_.begin() + static_cast<std::ptrdiff_t>(first), walk_.end());
    };
    push_children(pane.id());
    while (!walk_.empty()) {
        const InstanceId id = walk_.back();
        walk_.pop_back();
        const DataModel* object = game.instance(id);
        if (object == nullptr) {
            continue;
        }
        if (!object->viewport_pane()) {
            push_children(id);
        }
        if (dynamic_cast<const Camera*>(object) != nullptr) {
            if (entry.first_camera == 0) {
                entry.first_camera = id;
            }
        } else if (const auto* sky = dynamic_cast<const Skybox*>(object)) {
            if (entry.sky == 0) {
                entry.sky = id;
                fill_visual_sky(game, *sky, out.sky);
                out.sky.present = true;
            }
        } else if (const auto* drawn = dynamic_cast<const GameObject*>(object)) {
            VisualInstance row;
            row.id = id;
            row.world = drawn->transform();
            row.color = drawn->color();
            row.transparency = static_cast<float>(std::clamp(drawn->transparency(), 0.0, 1.0));
            row.scale = static_cast<float>(drawn->scale());
            const std::string& guid = drawn->prefab_guid();
            if (!guid.empty()) {
                const std::uint32_t index = find_prefab(game, guid);
                const auto known = std::find(entry.prefabs.begin(), entry.prefabs.end(), index);
                if (known == entry.prefabs.end()) {
                    ++prefabs_[index].panes;
                    entry.prefabs.push_back(index);
                    entry.prefab_revisions.push_back(prefabs_[index].revision);
                    out.prefabs.push_back(prefabs_[index].current);
                    row.prefab = static_cast<std::uint32_t>(out.prefabs.size() - 1);
                } else {
                    row.prefab = static_cast<std::uint32_t>(known - entry.prefabs.begin()) + 1;
                }
                if (prefabs_[index].current != nullptr && prefabs_[index].current->skeleton != nullptr) {
                    row.pose = drawn->pose();
                    entry.skinned_rows.push_back(static_cast<std::uint32_t>(out.rows.size()));
                }
            }
            out.rows.push_back(std::move(row));
        } else if (const auto* brush = dynamic_cast<const Brush*>(object)) {
            entry.brushes.push_back(id);
            brush_visuals_.append_single(game, *brush, out.brushes);
        } else if (const auto* light = dynamic_cast<const Light*>(object)) {
            VisualInstance row;
            row.id = id;
            row.world = light->transform();
            row.light = light_of(*light);
            out.rows.push_back(std::move(row));
        } else if (const auto* sun = dynamic_cast<const DirectionalLight*>(object)) {
            out.rows.push_back(sun_row(*sun));
        }
    }

    InstanceId camera = pane.current_camera_id();
    if (camera == 0) {
        camera = entry.first_camera;
    }
    if (const auto* object = camera != 0 ? dynamic_cast<const Camera*>(game.instance(camera)) : nullptr) {
        out.camera = camera;
        out.has_camera = true;
        out.camera_inside = owning_pane(game, camera) == pane.id();
        out.camera_world = object->transform();
        out.field_of_view = static_cast<float>(object->field_of_view());
    }
}

void ViewportScenes::update(DataModel& game, std::vector<std::shared_ptr<const VisualViewport>>& out) {
    out.clear();
    game.viewport_panes(pane_ids_);
    std::sort(pane_ids_.begin(), pane_ids_.end());
    const std::uint64_t tree = game.tree_revision();
    const bool tree_moved = tree != tree_seen_;
    tree_seen_ = tree;
    refresh_prefabs(game);
    brush_visuals_.begin_singles();
    std::sort(dirty_.begin(), dirty_.end());
    dirty_.erase(std::unique(dirty_.begin(), dirty_.end()), dirty_.end());
    for (const InstanceId id : pane_ids_) {
        const auto* pane = dynamic_cast<const ViewportPane*>(game.instance(id));
        if (pane == nullptr) {
            continue;
        }
        Pane& entry = panes_[id];
        const bool rebuild = all_ || tree_moved || entry.published == nullptr ||
                             pane->revision() != entry.pane_revision ||
                             std::binary_search(dirty_.begin(), dirty_.end(), id) ||
                             outside_changed(game, *pane, entry);
        if (rebuild) {
            build(game, *pane, entry);
            if (entry.published == nullptr || !same_scene(scratch_, *entry.published)) {
                scratch_.revision = next_revision_++;
                entry.published = std::make_shared<const VisualViewport>(scratch_);
            }
            entry.pane_revision = pane->revision();
        }
        entry.seen = true;
        out.push_back(entry.published);
    }
    for (auto it = panes_.begin(); it != panes_.end();) {
        if (!it->second.seen) {
            release_prefabs(it->second);
            it = panes_.erase(it);
        } else {
            it->second.seen = false;
            ++it;
        }
    }
    // A Prefab no pane draws any more is let go.
    for (std::uint32_t index = 0; index < prefabs_.size(); ++index) {
        Prefab& entry = prefabs_[index];
        if (entry.panes == 0 && !entry.guid.empty()) {
            prefab_by_guid_.erase(entry.guid);
            entry.guid.clear();
            entry.cached = 0;
            entry.current.reset();
            free_prefabs_.push_back(index);
        }
    }
    // Every pane with Brushes asked for theirs above; the rest are forgotten.
    brush_visuals_.end_singles();
    dirty_.clear();
    all_ = false;
}

}  // namespace engine_core
