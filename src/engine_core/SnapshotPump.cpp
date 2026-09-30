#include "SnapshotPump.hpp"

#include "AssetInstances.hpp"
#include "Camera.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"

#include <algorithm>

namespace engine_core {
namespace {

float field_of_view_of(const GameObject& object) {
    const auto* camera = dynamic_cast<const Camera*>(&object);
    return camera != nullptr ? static_cast<float>(camera->field_of_view()) : 0.f;
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
    // Only GameObjects under Workspace have rows. This drops the row of one
    // that left, and ignores a change to one that was never in.
    if (!game.in_workspace(change.id)) {
        erase_base(change.id);
        return;
    }
    const GameObject* object = game.game_object(change.id);
    if (object == nullptr) {
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
        set_row_prefab(*inst, object->prefab_guid());
    }
    if (whole || any(change.fields, VisualField::Camera)) {
        inst->field_of_view = field_of_view_of(*object);
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
        base_ids_.insert(object.id());
        base_.instances.push_back(inst);
    });
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
                }
            }
        }
        meshes.resize(used);
    }
}

void SnapshotPump::blit(VisualSnapshot& dst) const {
    dst.camera = base_.camera;
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

void SnapshotPump::take_changes(DataModel& game) {
    InvalidationQueue& queue = game.invalidations();
    if (queue.take_overflow() || game.consume_resync()) {
        resync(game);
    } else {
        queue.drain([&](const Invalidation& change) { apply_live(game, change); });
    }
    resolve_prefabs(game);
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
