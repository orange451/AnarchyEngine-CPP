// DataModel: the place captured at Test and restored at Stop, and the authored
// tree a project save writes.

#include "DataModel.hpp"

#include "DataModelState.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace engine_core {

void DataModel::capture_place() {
    if (!gameplay_thread()) {
        contract_fail("capture_place runs on SimulationThread");
    }
    if (state_->simulation_running) {
        contract_fail("capture_place while simulation is running");
    }
    DataModelLock lock(*this, DataModelLock::Write);
    capture_place_unlocked();
}

void DataModel::start_simulation() {
    if (!gameplay_thread()) {
        contract_fail("start_simulation runs on SimulationThread");
    }
    if (state_->simulation_running) {
        contract_fail("start_simulation while simulation is running");
    }
    DataModelLock lock(*this, DataModelLock::Write);
    // Commit an open edit recording before play. Play writes stay off the edit stack.
    if (state_->history) {
        state_->history->seal_edit_recording();
    }
    if (!state_->place_captured) {
        capture_place_unlocked();
    }
    state_->simulation_running = true;
    // Script analysis checks the authored tree only. It stops checking until Stop.
    if (ScriptAnalysis* analysis = script_analysis()) {
        analysis->note_play_started();
    }
    if (state_->on_start) {
        state_->on_start();
    }
}

void DataModel::stop_simulation() {
    if (!state_->simulation_running) {
        return;
    }
    if (!gameplay_thread()) {
        contract_fail("stop_simulation runs on SimulationThread");
    }
    DataModelLock lock(*this, DataModelLock::Write);
    // Session waypoints can name play-only instances. Drop them before restore
    // retires those ids. The edit stack stays; the snapshot matches it.
    if (state_->history) {
        state_->history->drop_session();
    }
    // Scripts abort while the play tree is still the live one. The steps below
    // are the existing stop: restore runs after the hook returns.
    if (state_->on_stop) {
        state_->on_stop();
    }
    state_->events.drop_pending();
    state_->events.disconnect_all();
    if (TaskScheduler* scheduler = state_->events.scheduler()) {
        scheduler->cancel_session_jobs();
    }
    restore_place_unlocked();
    ++state_->world_generation;
    state_->simulation_running = false;
    // Restore's own notes came while the simulation still ran, so analysis
    // ignored them. It brings itself up to date with the restored tree now.
    if (ScriptAnalysis* analysis = script_analysis()) {
        analysis->note_play_stopped();
    }
}

void DataModel::capture_place_unlocked() {
    PlaceSnapshot shot;
    shot.root_name = state_->root != nullptr ? state_->root->name_ : std::string();
    if (state_->root != nullptr) {
        shot.root_guid = state_->root->guid_;
        shot.root_extras = state_->root->extras_;
    }
    shot.root_children = child_ids(0);
    const InstanceId core_id = core();
    shot.root_children.erase(std::remove(shot.root_children.begin(), shot.root_children.end(), core_id),
                             shot.root_children.end());
    const std::uint32_t count = slot_count();
    shot.instances.reserve(count);
    state_->place_slots.assign(count, false);
    for (std::uint32_t index = 0; index < count; ++index) {
        Slot& part = state_->slots[index];
        if (!part.alive || part.instance == nullptr) {
            continue;
        }
        // Core is outside the place: Stop leaves it as play left it.
        if (core_holds(make_instance_id(part.generation, index))) {
            continue;
        }
        state_->place_slots[index] = true;
        if (part.pool >= state_->pools.size() || state_->pools[part.pool] == nullptr) {
            contract_fail("place capture lost an instance type");
        }
        PlaceRecord record;
        record.id = make_instance_id(part.generation, index);
        record.type_key = state_->pools[part.pool]->key;
        record.parent = part.parent;
        record.children = child_ids(record.id);
        record.name = part.instance->name_;
        record.simulated = has_tag(ecs_world(), part.entity, state_->ecs_ids.simulated);
        record.visual_only = has_tag(ecs_world(), part.entity, state_->ecs_ids.visual_only);
        record.archivable = part.instance->archivable_;
        part.instance->write_place(record.extra);
        record.guid = part.instance->guid_;
        record.extras = part.instance->extras_;
        const char* label = part.instance->class_name();
        record.class_name = label != nullptr ? label : "";
        record.properties = merged_properties(*part.instance);
        if (const auto* lua = dynamic_cast<const LuaSource*>(part.instance)) {
            record.has_source = true;
            record.source = lua->source();
        }
        shot.instances.push_back(std::move(record));
    }
    state_->place = std::move(shot);
    state_->place_captured = true;
}

std::uint16_t DataModel::pool_index_for(const void* type_key) const {
    for (std::uint16_t index = 0; index < state_->pools.size(); ++index) {
        if (state_->pools[index] != nullptr && state_->pools[index]->key == type_key) {
            return index;
        }
    }
    return kNoPool;
}

void DataModel::retire_slot(std::uint32_t index, bool bump_generation) {
    Slot& part = state_->slots[index];
    if (!part.alive) {
        return;
    }
    const InstanceId id = make_instance_id(part.generation, index);
    detach_links(id, part);
    release_signals(id);
    if (part.instance != nullptr) {
        part.instance->name_.clear();
        part.instance->guid_.clear();
        part.instance->extras_.clear();
    }
    release_to_pool(part);
    part.parent = kNoParent;
    part.first_child = 0;
    part.last_child = 0;
    part.next_sibling = 0;
    part.prev_sibling = 0;
    if (bump_generation && part.generation != kMaxGeneration) {
        ++part.generation;
    }
}

void DataModel::adopt_slot(std::uint16_t pool_index, InstanceId id) {
    if (pool_index >= state_->pools.size() || state_->pools[pool_index] == nullptr) {
        contract_fail("place restore lost an instance type");
    }
    const std::uint32_t index = id_slot(id);
    const std::uint32_t generation = id_generation(id);
    Slot& part = state_->slots[index];
    part.generation = generation;
    part.alive = true;
    part.parent = kNoParent;
    part.first_child = 0;
    part.last_child = 0;
    part.next_sibling = 0;
    part.prev_sibling = 0;

    InstancePool& pool = *state_->pools[pool_index];
    const std::uint32_t storage = take_storage(pool);
    issue_entity(part, id);
    DataModel* object = pooled_object(pool, storage, id);
    if (object->steps()) {
        ecs_add_id(ecs_world(), part.entity, state_->ecs_ids.steps);
    }
    if (object->physics_body()) {
        ecs_add_id(ecs_world(), part.entity, state_->ecs_ids.physics_body);
    }
    if (object->sound_source()) {
        ecs_add_id(ecs_world(), part.entity, state_->ecs_ids.sound_source);
    }
    if (object->billboard_gui()) {
        ecs_add_id(ecs_world(), part.entity, state_->ecs_ids.billboard);
    }
    part.pool = pool_index;
    part.storage = storage;
    part.instance = object;
    part.body = as_game_object(object);
}

void DataModel::restore_record(const PlaceRecord& record) {
    const std::uint32_t index = id_slot(record.id);
    Slot& part = state_->slots[index];
    const bool same = part.alive && part.instance != nullptr && make_instance_id(part.generation, index) == record.id &&
                      part.pool < state_->pools.size() && state_->pools[part.pool] != nullptr &&
                      state_->pools[part.pool]->key == record.type_key;
    if (!same) {
        if (part.alive) {
            retire_slot(index, false);
        }
        const std::uint16_t pool_index = pool_index_for(record.type_key);
        if (pool_index == kNoPool) {
            contract_fail("place restore lost an instance type");
        }
        adopt_slot(pool_index, record.id);
    }
    Slot& live = state_->slots[index];
    if (live.instance == nullptr) {
        contract_fail("place restore lost an instance");
    }
    // Direct, not through set_simulated: a restore records no history.
    set_tag(ecs_world(), live.entity, state_->ecs_ids.simulated, record.simulated);
    set_tag(ecs_world(), live.entity, state_->ecs_ids.visual_only, record.visual_only);
    live.instance->name_ = record.name;
    live.instance->guid_ = record.guid;
    live.instance->archivable_ = record.archivable;
    live.instance->extras_ = record.extras;
    const std::byte* bytes = record.extra.empty() ? nullptr : record.extra.data();
    live.instance->read_place(bytes, record.extra.size());
}

void DataModel::clear_hierarchy() {
    state_->tree_revision.fetch_add(1, std::memory_order_relaxed);
    state_->root_first_child = 0;
    state_->root_last_child = 0;
    ecs_world_t* world = ecs_world();
    for (Slot& part : state_->slots) {
        part.parent = kNoParent;
        part.first_child = 0;
        part.last_child = 0;
        part.next_sibling = 0;
        part.prev_sibling = 0;
        // Out of scope with its links. link_children rebuilds the tags, in any
        // order: a node that gains scope walks whatever is already linked below it.
        set_tag(world, part.entity, state_->ecs_ids.in_game, false);
        set_tag(world, part.entity, state_->ecs_ids.in_workspace, false);
        set_tag(world, part.entity, state_->ecs_ids.in_lighting, false);
        set_tag(world, part.entity, state_->ecs_ids.in_core, false);
    }
}

void DataModel::link_children(InstanceId parent, const std::vector<InstanceId>& children) {
    for (InstanceId child : children) {
        link_child(parent, child);
        refresh_scope(child);
    }
}

void DataModel::rebuild_free_list() {
    state_->free_list.clear();
    // Edit history outlives a play session, and undo may bring its instances back.
    std::unordered_set<std::uint32_t> held;
    if (state_->history != nullptr) {
        held = state_->history->revivable_slots();
    }
    state_->history_held.clear();
    for (std::uint32_t index = 0; index < state_->slots.size(); ++index) {
        if (state_->slots[index].alive) {
            continue;
        }
        if (held.count(index) == 0) {
            state_->free_list.push_back(index);
        } else {
            state_->history_held.push_back(index);
        }
    }
}

void DataModel::restore_place_unlocked() {
    if (!state_->place_captured) {
        contract_fail("stop_simulation without a place snapshot");
    }
    const PlaceSnapshot& place = state_->place;
    std::unordered_set<InstanceId> ids;
    ids.reserve(place.instances.size());
    for (const PlaceRecord& record : place.instances) {
        if (!ids.insert(record.id).second) {
            contract_fail("place snapshot has a duplicate instance");
        }
        const std::uint32_t index = id_slot(record.id);
        if (index >= state_->slots.size()) {
            contract_fail("place snapshot has an unknown instance");
        }
        if (pool_index_for(record.type_key) == kNoPool) {
            contract_fail("place snapshot instance type is missing");
        }
    }
    for (InstanceId child : place.root_children) {
        if (ids.count(child) == 0) {
            contract_fail("place snapshot lost a child");
        }
    }
    for (const PlaceRecord& record : place.instances) {
        for (InstanceId child : record.children) {
            if (ids.count(child) == 0) {
                contract_fail("place snapshot lost a child");
            }
        }
    }

    // Core and what it holds stay as they are, links included.
    const InstanceId core_id = core();
    std::vector<std::pair<InstanceId, std::vector<InstanceId>>> core_links;
    if (core_id != 0) {
        std::vector<InstanceId> walk{core_id};
        for (std::size_t at = 0; at < walk.size(); ++at) {
            std::vector<InstanceId> children = child_ids(walk[at]);
            walk.insert(walk.end(), children.begin(), children.end());
            core_links.emplace_back(walk[at], std::move(children));
        }
    }

    for (std::uint32_t index = 0; index < state_->slots.size(); ++index) {
        Slot& part = state_->slots[index];
        if (!part.alive) {
            continue;
        }
        const InstanceId id = make_instance_id(part.generation, index);
        if (ids.count(id) == 0 && !core_holds(id)) {
            retire_slot(index, true);
        }
    }
    for (const PlaceRecord& record : place.instances) {
        restore_record(record);
    }
    clear_hierarchy();
    link_children(0, place.root_children);
    for (const PlaceRecord& record : place.instances) {
        link_children(record.id, record.children);
    }
    if (core_id != 0) {
        link_children(0, {core_id});
        for (const auto& [parent, children] : core_links) {
            link_children(parent, children);
        }
    }
    if (state_->root != nullptr) {
        state_->root->name_ = place.root_name;
        state_->root->guid_ = place.root_guid;
        state_->root->extras_ = place.root_extras;
    }
    rebuild_free_list();
    // Edits after the last capture are gone from the live tree now.
    state_->dirty.clear();
    state_->dirty_all = true;
    state_->revision.fetch_add(1, std::memory_order_relaxed);
    state_->tree_revision.fetch_add(1, std::memory_order_relaxed);
    // The restore wrote instances without their setters.
    notify_all_watchers();

    {
        std::lock_guard<std::mutex> guard(state_->command_mu);
        state_->command_head = 0;
        state_->command_tail = 0;
        state_->command_size = 0;
    }
    // One resync so the next Prepare copies the reverted world, not session invalidations.
    state_->invalidation.clear();
    (void)state_->invalidation.take_overflow();
    state_->resync = true;
}

const std::vector<std::byte>* DataModel::captured_place_bytes() const {
    if (!state_->place_captured) {
        return nullptr;
    }
    for (const PlaceRecord& record : state_->place.instances) {
        if (record.id == id_) {
            return &record.extra;
        }
    }
    return nullptr;
}

std::vector<AuthoredNode> DataModel::authored_tree(const std::function<bool(InstanceId)>& want) const {
    std::vector<AuthoredNode> out;
    const DataModel* root = state_->root;
    if (root == nullptr) {
        return out;
    }
    auto wanted = [&want](InstanceId id) { return !want || want(id); };
    AuthoredNode top;
    top.id = 0;
    top.class_name = root->class_name();
    if (!state_->simulation_running) {
        top.guid = root->guid_;
        top.name = root->name_;
        if (wanted(0)) {
            top.has_properties = true;
            top.properties = merged_properties(*root);
        }
        out.push_back(std::move(top));
        // Breadth first with an explicit queue. A deep chain does not recurse.
        for (std::size_t at = 0; at < out.size(); ++at) {
            const InstanceId parent_id = out[at].id;
            for (InstanceId child = first_child(parent_id); child != 0; child = next_sibling(child)) {
                const DataModel* object = instance(child);
                // Core is the studio's, not the place's, and a non-archivable instance
                // asks not to be saved: neither is written, with all it holds.
                if (object == nullptr || child == core() || !object->archivable_) {
                    continue;
                }
                AuthoredNode node;
                node.id = child;
                node.guid = object->guid_;
                const char* label = object->class_name();
                node.class_name = label != nullptr ? label : "";
                node.name = object->name_;
                const auto* lua = dynamic_cast<const LuaSource*>(object);
                node.has_source = lua != nullptr;
                if (wanted(child)) {
                    node.has_properties = true;
                    node.properties = merged_properties(*object);
                    if (lua != nullptr) {
                        node.source = lua->source();
                    }
                }
                out[at].children.push_back(out.size());
                out.push_back(std::move(node));
            }
        }
        return out;
    }

    // Play: the snapshot is the authored tree. Play-only instances are not in it.
    const PlaceSnapshot& place = state_->place;
    top.guid = place.root_guid;
    top.name = place.root_name;
    top.has_properties = true;
    top.properties = place.root_extras;
    out.push_back(std::move(top));
    std::unordered_map<InstanceId, const PlaceRecord*> records;
    records.reserve(place.instances.size());
    for (const PlaceRecord& record : place.instances) {
        records.emplace(record.id, &record);
    }
    std::vector<const std::vector<InstanceId>*> kids{&place.root_children};
    for (std::size_t at = 0; at < out.size(); ++at) {
        const std::vector<InstanceId>& children = *kids[at];
        for (InstanceId child : children) {
            const auto found = records.find(child);
            if (found == records.end()) {
                continue;
            }
            const PlaceRecord& record = *found->second;
            // As at Test: what was not archivable then is not saved, with all it holds.
            if (!record.archivable) {
                continue;
            }
            AuthoredNode node;
            node.id = record.id;
            node.guid = record.guid;
            node.class_name = record.class_name;
            node.name = record.name;
            node.has_properties = true;
            node.properties = record.properties;
            node.has_source = record.has_source;
            node.source = record.source;
            out[at].children.push_back(out.size());
            out.push_back(std::move(node));
            kids.push_back(&record.children);
        }
    }
    return out;
}

}  // namespace engine_core
