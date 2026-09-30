#include "DataModel.hpp"

#include "Containment.hpp"
#include "DataModelState.hpp"
#include "LuaApi.hpp"
#include "PropertyReflection.hpp"

#include <algorithm>

namespace engine_core {

bool valid_guid(std::string_view guid) {
    if (guid.empty() || guid.size() > 64 || guid.front() == '-') {
        return false;
    }
    for (const char c : guid) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string make_guid() {
    // One generator per thread. Seeded from the device, the clock, and the
    // thread, so two processes started together still diverge.
    thread_local std::mt19937_64 engine = [] {
        std::random_device device;
        std::seed_seq seed{static_cast<std::uint64_t>(device()), static_cast<std::uint64_t>(device()),
                           static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()),
                           static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()),
                           static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()))};
        return std::mt19937_64(seed);
    }();
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(engine()));
    return std::string(buffer, 16);
}

DataModel::DataModel(const char* root_name) : owned_(std::make_unique<State>()), state_(owned_.get()) {
    State& world = *state_;
    world.ecs_ids = register_ecs(world.ecs);
    world.step_query =
        world.ecs.query_builder<>().with<ecs::Instance>().in().with<ecs::Steps>().with<ecs::InGame>().cached().build();
    world.physics_query = world.ecs.query_builder<>()
                              .with<ecs::Instance>()
                              .in()
                              .with<Matrix4>()
                              .inout()
                              .with<ecs::Velocity>()
                              .in()
                              .with<ecs::Simulated>()
                              .without<ecs::VisualOnly>()
                              .cached()
                              .build();
    world.render_query = world.ecs.query_builder<>()
                             .with<ecs::Instance>()
                             .in()
                             .with<ecs::InWorkspace>()
                             .with<Matrix4>()
                             .inout_none()
                             .cached()
                             .build();
    world.slots.reserve(kMaxInstances);
    world.free_list.reserve(kMaxInstances);
    world.invalidation.reserve(kMaxInvalidations);
    world.commands.assign(kInitialCommands, Command{});
    world.bags.resize(kMaxInstances);
    world.walk.reserve(kMaxInstances);
    world.step_ids.reserve(kMaxInstances);
    world.scope_walk.reserve(kMaxInstances);
    world.events.watch_prerender(&world.prerender_window);
    world.root = this;
    name_ = root_name;
    guid_ = make_guid();
    world.history = std::make_unique<ChangeHistoryService>(*this);
}

DataModel::DataModel(ChildTag, State& state, InstanceId id) : state_(&state), id_(id) {}

DataModel::~DataModel() {
    if (owned_) {
        owned_->events.shutdown();
        owned_.reset();
    }
}

void DataModel::attach_scheduler(TaskScheduler* scheduler) { state_->events.attach_scheduler(scheduler); }

EventQueue& DataModel::events() { return state_->events; }

const EventQueue& DataModel::events() const { return state_->events; }

void DataModel::set_thread_ids(std::thread::id simulation, std::thread::id render) {
    state_->simulation_thread = simulation;
    state_->render_thread = render;
}

void DataModel::set_threads_running(bool running) { state_->threads_running = running; }

void DataModel::require_simulation_thread(const char* message) const {
    if (!gameplay_thread()) {
        contract_fail(message);
    }
}

bool DataModel::gameplay_thread() const {
    if (!state_->threads_running) {
        return true;
    }
    const std::thread::id self = std::this_thread::get_id();
    return self == state_->simulation_thread || self == state_->edit_owner;
}

void DataModel::perform_paused_edit(const std::function<void(DataModel&)>& fn) {
    struct Clear {
        State& state;
        ~Clear() { state.edit_owner = {}; }
    } clear{*state_};
    state_->edit_owner = std::this_thread::get_id();
    fn(*this);
}

void DataModel::set_prerender_window(bool open) { state_->prerender_window = open; }

bool DataModel::prerender_window() const { return state_->prerender_window; }

int DataModel::write_depth() const { return state_->write_depth; }

InvalidationQueue& DataModel::invalidations() { return state_->invalidation; }

bool DataModel::holds_write() const {
    return state_->owner.load(std::memory_order_relaxed) == std::this_thread::get_id();
}

bool DataModel::lock_write_blocking() {
    if (holds_write()) {
        ++state_->write_depth;
        return true;
    }
    state_->write_mu.lock();
    state_->owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
    state_->write_depth = 1;
    return true;
}

bool DataModel::lock_write_for(std::chrono::milliseconds budget) {
    if (holds_write()) {
        ++state_->write_depth;
        return true;
    }
    if (!state_->write_mu.try_lock_for(budget)) {
        return false;
    }
    state_->owner.store(std::this_thread::get_id(), std::memory_order_relaxed);
    state_->write_depth = 1;
    return true;
}

void DataModel::unlock_write() {
    if (!holds_write() || state_->write_depth <= 0) {
        contract_fail("DataModelLock released without a hold");
    }
    --state_->write_depth;
    if (state_->write_depth > 0) {
        return;
    }
    // The engine loops take their own deferred violations after a step. Any other
    // thread, such as one running a paused edit, never does, so its entry goes
    // with its last guard rather than waiting for a thread that reuses its id.
    const std::thread::id self = std::this_thread::get_id();
    if (self != state_->simulation_thread && self != state_->render_thread) {
        std::lock_guard<std::mutex> guard(state_->deferred_mu);
        for (auto it = state_->deferred.begin(); it != state_->deferred.end(); ++it) {
            if (it->first == self) {
                state_->deferred.erase(it);
                break;
            }
        }
    }
    state_->owner.store(std::thread::id{}, std::memory_order_relaxed);
    state_->write_mu.unlock();
}

DataModel::Slot* DataModel::slot(InstanceId id) {
    const std::uint32_t index = id_slot(id);
    const std::uint32_t generation = id_generation(id);
    if (index >= state_->slots.size()) {
        return nullptr;
    }
    Slot& part = state_->slots[index];
    if (!part.alive || part.generation != generation) {
        return nullptr;
    }
    return &part;
}

const DataModel::Slot* DataModel::slot(InstanceId id) const {
    return const_cast<DataModel*>(this)->slot(id);
}

std::uint32_t DataModel::slot_count() const { return static_cast<std::uint32_t>(state_->slots.size()); }

const GameObject* DataModel::game_object_at_slot(std::uint32_t index) const {
    if (index >= state_->slots.size()) {
        return nullptr;
    }
    const Slot& part = state_->slots[index];
    if (!part.alive) {
        return nullptr;
    }
    return as_game_object(part.instance);
}

WriteOrigin DataModel::current_origin() const {
    if (state_->threads_running && std::this_thread::get_id() == state_->render_thread) {
        return WriteOrigin::PreRenderDataModel;
    }
    return WriteOrigin::Simulation;
}

bool DataModel::reject_write(const char* message) {
    if (holds_write()) {
        const std::thread::id self = std::this_thread::get_id();
        std::lock_guard<std::mutex> guard(state_->deferred_mu);
        for (auto& entry : state_->deferred) {
            if (entry.first == self) {
                entry.second = message;
                return false;
            }
        }
        state_->deferred.emplace_back(self, message);
        return false;
    }
    contract_fail(message);
}

bool DataModel::take_deferred_violation() {
    const std::thread::id self = std::this_thread::get_id();
    std::lock_guard<std::mutex> guard(state_->deferred_mu);
    for (auto it = state_->deferred.begin(); it != state_->deferred.end(); ++it) {
        if (it->first == self) {
            state_->deferred.erase(it);
            return true;
        }
    }
    return false;
}

bool DataModel::has_deferred_violation() const {
    const std::thread::id self = std::this_thread::get_id();
    std::lock_guard<std::mutex> guard(state_->deferred_mu);
    for (const auto& entry : state_->deferred) {
        if (entry.first == self) {
            return true;
        }
    }
    return false;
}

bool DataModel::authorize(const Slot& part, bool force_sim_write) {
    if (!state_->threads_running) {
        return true;
    }
    const std::thread::id self = std::this_thread::get_id();
    if (self == state_->simulation_thread || self == state_->edit_owner) {
        return true;
    }
    if (self == state_->render_thread) {
        if (!state_->prerender_window) {
            return reject_write("DataModel write from RenderThread outside RenderStepped and PreRender");
        }
        if (force_sim_write || has_tag(ecs_world(), part.entity, state_->ecs_ids.visual_only)) {
            return true;
        }
        return reject_write("render-step DataModel write requires visual_only or ForceSimWrite");
    }
    return true;
}

void DataModel::note(InstanceId id, VisualField fields, WriteOrigin origin) {
    state_->invalidation.push(Invalidation{id, fields, origin});
    if (state_->invalidation.overflow()) {
        state_->resync = true;
    }
}

void DataModel::enqueue(Command command) {
    std::lock_guard<std::mutex> guard(state_->command_mu);
    if (state_->command_size == state_->commands.size()) {
        // Workers can post faster than one step drains. The ring doubles under
        // the same lock drain_commands takes.
        grow_ring(state_->commands, state_->command_head, state_->command_tail, state_->command_size,
                  kInitialCommands);
    }
    state_->commands[state_->command_tail] = command;
    state_->command_tail = (state_->command_tail + 1) % state_->commands.size();
    ++state_->command_size;
}

void DataModel::drain_commands() {
    while (true) {
        Command command;
        {
            std::lock_guard<std::mutex> guard(state_->command_mu);
            if (state_->command_size == 0) {
                return;
            }
            command = state_->commands[state_->command_head];
            state_->command_head = (state_->command_head + 1) % state_->commands.size();
            --state_->command_size;
        }
        if (command.type == Command::Type::Destroy) {
            // Queued from another thread, which could not ask destroy_error: a
            // scene service stays, as a script's Destroy of one would.
            if (alive(command.id) && destroy_error(command.id)) {
                continue;
            }
            destroy(command.id);
        } else {
            apply_transform(command.id, command.transform, false);
        }
    }
}

bool DataModel::consume_resync() {
    const bool was = state_->resync;
    state_->resync = false;
    return was;
}

void DataModel::issue_entity(Slot& part, InstanceId id) {
    ecs_world_t* world = ecs_world();
    part.entity = ecs_new(world);
    write_component(world, part.entity, state_->ecs_ids.instance, ecs::Instance{id});
}

ecs_world_t* DataModel::ecs_world() const { return state_->ecs.c_ptr(); }

const EcsIds& DataModel::component_ids() const { return state_->ecs_ids; }

std::uint64_t DataModel::entity_of(InstanceId id) const {
    const Slot* part = slot(id);
    return part == nullptr ? 0 : part->entity;
}

std::size_t DataModel::entity_count() const {
    return static_cast<std::size_t>(ecs_count_id(ecs_world(), state_->ecs_ids.instance));
}

std::size_t DataModel::room_left() const {
    const State& world = *state_;
    return kMaxInstances - world.slots.size() + world.free_list.size();
}

InstanceCapacityError::InstanceCapacityError()
    : std::runtime_error("The place is full: it already holds " + std::to_string(DataModel::kMaxInstances) +
                         " instances, the most it can. Delete some to make room.") {}

InstanceId DataModel::allocate() {
    State& world = *state_;
    std::uint32_t index = 0;
    if (!world.free_list.empty()) {
        index = world.free_list.back();
        world.free_list.pop_back();
    } else {
        if (world.slots.size() >= kMaxInstances) {
            contract_fail("instance capacity exhausted");
        }
        index = static_cast<std::uint32_t>(world.slots.size());
        world.slots.emplace_back();
    }
    Slot& part = world.slots[index];
    part.alive = true;
    part.pool = 0;
    part.storage = 0;
    part.instance = nullptr;
    part.body = nullptr;
    part.parent = kNoParent;
    part.first_child = 0;
    part.last_child = 0;
    part.next_sibling = 0;
    part.prev_sibling = 0;
    return make_instance_id(part.generation, index);
}

std::uint32_t DataModel::take_storage(InstancePool& pool) {
    if (!pool.free.empty()) {
        const std::uint32_t storage = pool.free.back();
        pool.free.pop_back();
        return storage;
    }
    if (pool.count >= kMaxInstances) {
        contract_fail("instance capacity exhausted");
    }
    const auto storage = static_cast<std::uint32_t>(pool.count);
    ++pool.count;
    return storage;
}

DataModel* DataModel::pooled_object(InstancePool& pool, std::uint32_t storage, InstanceId id) {
    DataModel* object = pool.objects[storage];
    if (object == nullptr) {
        void* memory = pool.memory + static_cast<std::size_t>(storage) * pool.stride;
        object = pool.construct(memory, ChildTag{}, *state_, id);
        pool.objects[storage] = object;
    } else {
        object->rebind(id);
        object->on_reuse();
    }
    return object;
}

void DataModel::release_to_pool(Slot& part) {
    if (part.instance != nullptr) {
        part.instance->on_release();
    }
    if (part.entity != 0) {
        ecs_delete(ecs_world(), part.entity);
        part.entity = 0;
    }
    if (part.pool < state_->pools.size() && state_->pools[part.pool] != nullptr) {
        state_->pools[part.pool]->free.push_back(part.storage);
    }
    part.instance = nullptr;
    part.body = nullptr;
    part.alive = false;
}

DataModel& DataModel::spawn(const SpawnOps& ops) {
    if (!gameplay_thread()) {
        contract_fail("create runs on SimulationThread");
    }
    if (ops.construct == nullptr || ops.destroy == nullptr || ops.bytes == 0 || ops.align == 0) {
        contract_fail("create is missing a constructor");
    }
    // Checked before anything changes. A type's storage never runs out first:
    // each object it holds is also one of the place's instances.
    if (room_left() == 0) {
        throw InstanceCapacityError();
    }
    State& world = *state_;
    InstancePool* pool = nullptr;
    std::uint16_t pool_index = 0;
    for (std::uint16_t index = 0; index < world.pools.size(); ++index) {
        if (world.pools[index]->key == ops.key) {
            pool = world.pools[index].get();
            pool_index = index;
            break;
        }
    }
    if (pool == nullptr) {
        if (world.pools.size() >= kNoPool) {
            contract_fail("instance type capacity exhausted");
        }
        const std::size_t align = ops.align;
        const std::size_t stride = (ops.bytes + align - 1) & ~(align - 1);
        auto created = std::make_unique<InstancePool>();
        created->key = ops.key;
        created->stride = stride;
        created->align = align;
        created->construct = ops.construct;
        created->destroy = ops.destroy;
        created->memory = static_cast<std::byte*>(::operator new(stride * kMaxInstances, std::align_val_t(align)));
        created->objects.assign(kMaxInstances, nullptr);
        created->free.reserve(kMaxInstances);
        pool_index = static_cast<std::uint16_t>(world.pools.size());
        world.pools.push_back(std::move(created));
        pool = world.pools.back().get();
    }

    const std::uint32_t storage = take_storage(*pool);
    const InstanceId id = allocate();
    const std::uint32_t index = id_slot(id);
    issue_entity(world.slots[index], id);
    DataModel* object = pooled_object(*pool, storage, id);
    if (object->steps()) {
        ecs_add_id(ecs_world(), world.slots[index].entity, world.ecs_ids.steps);
    }
    const char* label = object->class_name();
    object->name_ = label != nullptr ? label : std::string();
    // Assigned once. A project load replaces it with the GUID from disk.
    object->guid_ = make_guid();
    object->extras_.clear();
    Slot& part = world.slots[index];
    part.pool = pool_index;
    part.storage = storage;
    part.instance = object;
    part.body = as_game_object(object);
    if (dynamic_cast<LuaSource*>(object) != nullptr) {
        if (ScriptAnalysis* analysis = script_analysis()) {
            analysis->invalidate(object->id());
        }
    }
    record_created(object->id());
    mark_authored_dirty(object->id());
    return *object;
}

DataModel& DataModel::create() { return create<DataModel>(); }

GameObject& DataModel::create_game_object() {
    GameObject& object = create<GameObject>();
    note(object.id(), VisualField::Transform, WriteOrigin::Simulation);
    return object;
}

void DataModel::destroy(InstanceId id) {
    // A paused edit owns the world like SimulationThread does, so it destroys now.
    if (!gameplay_thread()) {
        if (std::this_thread::get_id() == state_->render_thread) {
            contract_fail("destroy from RenderThread");
        }
        Command command;
        command.type = Command::Type::Destroy;
        command.id = id;
        enqueue(command);
        return;
    }
    const std::uint32_t index = id_slot(id);
    Slot* part = slot(id);
    if (part == nullptr) {
        return;
    }
    if (part->instance != nullptr && part->instance->is_service()) {
        contract_fail(destroy_error(id)->c_str());
    }
    std::optional<AuthoredRecord> captured;
    if (state_->history && state_->history->wants_mutation()) {
        captured = capture_record(id, true);
    }
    // The parent's folder may become a leaf and its child order changes.
    mark_authored_dirty(part->parent);
    note_tree_changed();
    // Its children leave the tree without set_parent, so scripts under them stop here.
    const std::vector<InstanceId> orphans = state_->script_host != nullptr ? get_children(id) : std::vector<InstanceId>{};
    detach_links(id, *part);
    release_signals(id);
    release_to_pool(*part);
    if (part->generation != kMaxGeneration) {
        ++part->generation;
        state_->free_list.push_back(index);
    }
    note(id, VisualField::Removed, current_origin());
    notify_watchers(id);
    if (captured) {
        record_destroyed(std::move(*captured));
    }
    for (InstanceId orphan : orphans) {
        state_->script_host->on_moved(orphan);
    }
}

bool DataModel::queues_visual_write() const {
    // A paused edit writes now, like SimulationThread. Any other thread enqueues.
    return state_->threads_running && !gameplay_thread() && std::this_thread::get_id() != state_->render_thread;
}

GameObject* DataModel::visual_target(InstanceId id, bool force, const char* dead, const char* not_object) {
    Slot* part = slot(id);
    if (part == nullptr) {
        reject_write(dead);
        return nullptr;
    }
    GameObject* object = as_game_object(part->instance);
    if (object == nullptr) {
        reject_write(not_object);
        return nullptr;
    }
    return authorize(*part, force) ? object : nullptr;
}

void DataModel::apply_transform(InstanceId id, const Matrix4& transform, bool force) {
    if (queues_visual_write()) {
        Command command;
        command.type = Command::Type::Transform;
        command.id = id;
        command.transform = transform;
        enqueue(command);
        return;
    }
    GameObject* target = visual_target(id, force, "transform write on a dead instance",
                                       "transform write on an instance that is not a GameObject");
    if (target == nullptr) {
        return;
    }
    const Matrix4 previous = target->transform();
    if (same_matrix4(previous, transform)) {
        return;
    }
    target->store_transform(transform);
    record_transform(id, previous, transform);
    const WriteOrigin origin = current_origin();
    note(id, VisualField::Transform, origin);
    emit_change(id, Field::Transform, origin);
}


void DataModel::set_simulated(InstanceId id, bool simulated) {
    if (!gameplay_thread()) {
        contract_fail("set_simulated runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail("set_simulated on a dead instance");
    }
    const bool previous = has_tag(ecs_world(), part->entity, state_->ecs_ids.simulated);
    if (previous == simulated) {
        return;
    }
    set_tag(ecs_world(), part->entity, state_->ecs_ids.simulated, simulated);
    record_bool(id, Field::Simulated, previous, simulated);
    emit_change(id, Field::Simulated, current_origin());
}

void DataModel::set_visual_only(InstanceId id, bool visual_only) {
    if (!gameplay_thread()) {
        contract_fail("set_visual_only runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail("set_visual_only on a dead instance");
    }
    const bool previous = has_tag(ecs_world(), part->entity, state_->ecs_ids.visual_only);
    if (previous == visual_only) {
        return;
    }
    set_tag(ecs_world(), part->entity, state_->ecs_ids.visual_only, visual_only);
    record_bool(id, Field::VisualOnly, previous, visual_only);
    emit_change(id, Field::VisualOnly, current_origin());
}

DataModel* DataModel::instance(InstanceId id) {
    Slot* part = slot(id);
    if (part == nullptr) {
        return nullptr;
    }
    return part->instance;
}

const DataModel* DataModel::instance(InstanceId id) const { return const_cast<DataModel*>(this)->instance(id); }

GameObject* DataModel::game_object(InstanceId id) { return as_game_object(instance(id)); }

const GameObject* DataModel::game_object(InstanceId id) const {
    return const_cast<DataModel*>(this)->game_object(id);
}

bool DataModel::alive(InstanceId id) const { return slot(id) != nullptr; }

bool DataModel::in_game(InstanceId id) const {
    return has_tag(ecs_world(), entity_of(id), state_->ecs_ids.in_game);
}

bool DataModel::in_workspace(InstanceId id) const {
    return has_tag(ecs_world(), entity_of(id), state_->ecs_ids.in_workspace);
}

void DataModel::refresh_scope(InstanceId id) {
    const Slot* part = slot(id);
    if (part == nullptr) {
        return;
    }
    bool game = false;
    bool workspace = false;
    if (part->parent == 0) {
        game = true;
    } else if (part->parent != kNoParent) {
        game = in_game(part->parent);
        workspace = in_workspace(part->parent) || part->parent == scene_service("Workspace");
    }
    if (in_game(id) == game && in_workspace(id) == workspace) {
        return;
    }
    apply_scope(id, game, workspace);
}

void DataModel::apply_scope(InstanceId id, bool in_game_now, bool in_workspace_now) {
    // Top-down over the subtree. A node whose tags come out unchanged prunes
    // its children: their tags were derived from its tags.
    ecs_world_t* world = ecs_world();
    const EcsIds& ids = state_->ecs_ids;
    const InstanceId workspace_id = scene_service("Workspace");
    std::vector<InstanceId>& queue = state_->scope_walk;
    queue.clear();
    queue.push_back(id);
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const InstanceId cur = queue[i];
        const Slot* part = slot(cur);
        if (part == nullptr) {
            continue;
        }
        bool game = in_game_now;
        bool workspace = in_workspace_now;
        if (i > 0) {
            game = in_game(part->parent);
            workspace = in_workspace(part->parent) || part->parent == workspace_id;
        }
        const bool had_game = has_tag(world, part->entity, ids.in_game);
        const bool had_workspace = has_tag(world, part->entity, ids.in_workspace);
        if (had_game == game && had_workspace == workspace) {
            continue;
        }
        if (had_game != game) {
            set_tag(world, part->entity, ids.in_game, game);
        }
        if (had_workspace != workspace) {
            set_tag(world, part->entity, ids.in_workspace, workspace);
            if (part->body != nullptr) {
                note(cur, VisualField::Ancestry, current_origin());
            }
        }
        for (InstanceId child = part->first_child; child != 0;) {
            if (queue.size() == queue.capacity()) {
                contract_fail("scope walk capacity exhausted");
            }
            queue.push_back(child);
            const Slot* child_slot = slot(child);
            if (child_slot == nullptr) {
                break;
            }
            child = child_slot->next_sibling;
        }
    }
}

bool DataModel::simulated(InstanceId id) const {
    return has_tag(ecs_world(), entity_of(id), state_->ecs_ids.simulated);
}

bool DataModel::visual_only(InstanceId id) const {
    return has_tag(ecs_world(), entity_of(id), state_->ecs_ids.visual_only);
}

void DataModel::integrate_simulated(double dt) {
    const float step = static_cast<float>(dt);
    // Writes component values only, so it runs inside the query. note() only
    // queues, and watcher callbacks must not call back into the DataModel.
    ecs_iter_t it = ecs_query_iter(ecs_world(), state_->physics_query.c_ptr());
    while (ecs_query_next(&it)) {
        const auto* owners = static_cast<const ecs::Instance*>(ecs_field_w_size(&it, sizeof(ecs::Instance), 0));
        auto* transforms = static_cast<Matrix4*>(ecs_field_w_size(&it, sizeof(Matrix4), 1));
        const auto* velocities = static_cast<const ecs::Velocity*>(ecs_field_w_size(&it, sizeof(ecs::Velocity), 2));
        for (std::int32_t i = 0; i < it.count; ++i) {
            const ecs::Velocity& velocity = velocities[i];
            if (velocity.x == 0.f && velocity.y == 0.f && velocity.z == 0.f) {
                continue;
            }
            transforms[i].m[12] += velocity.x * step;
            transforms[i].m[13] += velocity.y * step;
            transforms[i].m[14] += velocity.z * step;
            note(owners[i].id, VisualField::Transform, WriteOrigin::Simulation);
            notify_watchers(owners[i].id);
        }
    }
}

void DataModel::step_instances(double dt) {
    // Gather first: step() may create, destroy, or reparent, which flecs would
    // defer inside a running query. step_ids is reserved, so this does not allocate.
    std::vector<InstanceId>& ids = state_->step_ids;
    ids.clear();
    ecs_iter_t it = ecs_query_iter(ecs_world(), state_->step_query.c_ptr());
    while (ecs_query_next(&it)) {
        const auto* owners = static_cast<const ecs::Instance*>(ecs_field_w_size(&it, sizeof(ecs::Instance), 0));
        for (std::int32_t i = 0; i < it.count; ++i) {
            ids.push_back(owners[i].id);
        }
    }
    for (const InstanceId id : ids) {
        if (DataModel* object = instance(id)) {
            object->step(dt);
        }
    }
}

void DataModel::for_each_rendered(const std::function<void(const GameObject&)>& fn) const {
    ecs_iter_t it = ecs_query_iter(ecs_world(), state_->render_query.c_ptr());
    while (ecs_query_next(&it)) {
        const auto* owners = static_cast<const ecs::Instance*>(ecs_field_w_size(&it, sizeof(ecs::Instance), 0));
        for (std::int32_t i = 0; i < it.count; ++i) {
            if (const GameObject* object = game_object(owners[i].id)) {
                fn(*object);
            }
        }
    }
}

std::size_t DataModel::stepper_count() const {
    std::size_t count = 0;
    ecs_iter_t it = ecs_query_iter(ecs_world(), state_->step_query.c_ptr());
    while (ecs_query_next(&it)) {
        count += static_cast<std::size_t>(it.count);
    }
    return count;
}

DataModel::InstanceSignals* DataModel::bag_for(InstanceId id) {
    if (id == 0) {
        return state_->root_signals.get();
    }
    if (slot(id) == nullptr) {
        return nullptr;
    }
    const std::uint32_t index = id_slot(id);
    if (index >= state_->bags.size() || !state_->bags[index] || state_->bags[index]->owner != id) {
        return nullptr;
    }
    return state_->bags[index].get();
}

DataModel::InstanceSignals& DataModel::ensure_bag(InstanceId id) {
    if (id == 0) {
        if (state_->root == nullptr) {
            contract_fail("signal on a dead instance");
        }
        // Never destroy_instance(0). RunService phase signals are also owned
        // by 0, and their queued events use instance 0.
        if (!state_->root_signals) {
            state_->root_signals = std::make_unique<InstanceSignals>();
            state_->root_signals->owner = 0;
        }
        return *state_->root_signals;
    }
    if (slot(id) == nullptr) {
        contract_fail("signal on a dead instance");
    }
    const std::uint32_t index = id_slot(id);
    if (state_->bags.size() <= index) {
        state_->bags.resize(index + 1);
    }
    if (!state_->bags[index] || state_->bags[index]->owner != id) {
        if (state_->bags[index]) {
            state_->events.destroy_instance(state_->bags[index]->owner);
        }
        state_->bags[index] = std::make_unique<InstanceSignals>();
        state_->bags[index]->owner = id;
    }
    return *state_->bags[index];
}

Signal& DataModel::ensure_signal(InstanceId id, SignalKind kind, Field field) {
    InstanceSignals& bag = ensure_bag(id);
    Signal* signal = nullptr;
    switch (kind) {
    case SignalKind::Changed:
        signal = &bag.changed;
        break;
    case SignalKind::PropertyChanged:
        signal = &bag.property[field_index(field)];
        break;
    case SignalKind::ChildAdded:
        signal = &bag.child_added;
        break;
    case SignalKind::ChildRemoved:
        signal = &bag.child_removed;
        break;
    case SignalKind::AncestryChanged:
        signal = &bag.ancestry;
        break;
    }
    if (!signal->bound()) {
        signal->owner_ = id;
        signal->kind_ = kind;
        signal->field_ = field;
        state_->events.register_signal(signal);
    }
    return *signal;
}

Signal& DataModel::changed(InstanceId id) { return ensure_signal(id, SignalKind::Changed, Field::Transform); }

Signal& DataModel::property_changed(InstanceId id, Field field) {
    field_index(field);
    return ensure_signal(id, SignalKind::PropertyChanged, field);
}

Signal& DataModel::child_added(InstanceId id) { return ensure_signal(id, SignalKind::ChildAdded, Field::Parent); }

Signal& DataModel::child_removed(InstanceId id) {
    return ensure_signal(id, SignalKind::ChildRemoved, Field::Parent);
}

Signal& DataModel::ancestry_changed(InstanceId id) {
    return ensure_signal(id, SignalKind::AncestryChanged, Field::Parent);
}

std::uint64_t DataModel::watch_changes(std::function<void()> notify) {
    std::lock_guard<std::mutex> guard(state_->watch_mu);
    State::ChangeWatcher watcher;
    watcher.watch = state_->next_watch++;
    watcher.notify = std::move(notify);
    state_->watchers.push_back(std::move(watcher));
    return state_->watchers.back().watch;
}

void DataModel::set_watched(std::uint64_t watch, std::vector<InstanceId> ids) {
    std::sort(ids.begin(), ids.end());
    std::lock_guard<std::mutex> guard(state_->watch_mu);
    std::size_t watched = 0;
    for (State::ChangeWatcher& watcher : state_->watchers) {
        if (watcher.watch == watch) {
            watcher.ids = std::move(ids);
        }
        watched += watcher.ids.size();
    }
    state_->watched_count.store(watched, std::memory_order_relaxed);
}

void DataModel::unwatch_changes(std::uint64_t watch) {
    std::lock_guard<std::mutex> guard(state_->watch_mu);
    auto& watchers = state_->watchers;
    watchers.erase(std::remove_if(watchers.begin(), watchers.end(),
                                  [watch](const State::ChangeWatcher& watcher) { return watcher.watch == watch; }),
                   watchers.end());
    std::size_t watched = 0;
    for (const State::ChangeWatcher& watcher : watchers) {
        watched += watcher.ids.size();
    }
    state_->watched_count.store(watched, std::memory_order_relaxed);
}

void DataModel::notify_watchers(InstanceId id) {
    if (state_->watched_count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(state_->watch_mu);
    for (const State::ChangeWatcher& watcher : state_->watchers) {
        if (watcher.notify && std::binary_search(watcher.ids.begin(), watcher.ids.end(), id)) {
            watcher.notify();
        }
    }
}

void DataModel::notify_all_watchers() {
    if (state_->watched_count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(state_->watch_mu);
    for (const State::ChangeWatcher& watcher : state_->watchers) {
        if (watcher.notify && !watcher.ids.empty()) {
            watcher.notify();
        }
    }
}

void DataModel::emit_change(InstanceId id, Field field, WriteOrigin origin, std::uint64_t payload) {
    if (field == Field::Source) {
        state_->source_revision.fetch_add(1, std::memory_order_relaxed);
    }
    notify_watchers(id);
    InstanceSignals* bag = bag_for(id);
    if (bag == nullptr) {
        return;
    }
    if (bag->changed.bound() && bag->changed.listeners_ > 0) {
        state_->events.emit(bag->changed.id(), id, field, origin, payload);
    }
    const int index = static_cast<int>(field);
    if (index >= 0 && index < static_cast<int>(Field::Count)) {
        Signal& prop = bag->property[index];
        if (prop.bound() && prop.listeners_ > 0) {
            state_->events.emit(prop.id(), id, field, origin, payload);
        }
    }
}

void DataModel::emit_child(InstanceId parent, SignalKind kind, InstanceId child, WriteOrigin origin) {
    InstanceSignals* bag = bag_for(parent);
    if (bag == nullptr) {
        return;
    }
    Signal& signal = kind == SignalKind::ChildRemoved ? bag->child_removed : bag->child_added;
    if (!signal.bound() || signal.listeners_ <= 0) {
        return;
    }
    state_->events.emit(signal.id(), child, Field::Parent, origin);
}

void DataModel::emit_ancestry(InstanceId id, WriteOrigin origin) {
    state_->walk.clear();
    state_->walk.push_back(id);
    for (std::size_t i = 0; i < state_->walk.size(); ++i) {
        const InstanceId cur = state_->walk[i];
        InstanceSignals* bag = bag_for(cur);
        if (bag != nullptr && bag->ancestry.bound() && bag->ancestry.listeners_ > 0) {
            state_->events.emit(bag->ancestry.id(), cur, Field::Parent, origin);
        }
        Slot* part = slot(cur);
        if (part == nullptr) {
            continue;
        }
        for (InstanceId child = part->first_child; child != 0;) {
            if (state_->walk.size() == state_->walk.capacity()) {
                break;
            }
            state_->walk.push_back(child);
            Slot* child_slot = slot(child);
            if (child_slot == nullptr) {
                break;
            }
            child = child_slot->next_sibling;
        }
    }
}

void DataModel::unlink_parent(InstanceId id, Slot& part) {
    if (part.parent == kNoParent) {
        part.prev_sibling = 0;
        part.next_sibling = 0;
        return;
    }
    state_->tree_revision.fetch_add(1, std::memory_order_relaxed);
    // Parent 0 is the root DataModel, which has no slot of its own.
    InstanceId* head = nullptr;
    InstanceId* tail = nullptr;
    if (part.parent == 0) {
        head = &state_->root_first_child;
        tail = &state_->root_last_child;
    } else if (Slot* parent = slot(part.parent)) {
        head = &parent->first_child;
        tail = &parent->last_child;
    }
    if (part.prev_sibling != 0) {
        Slot* prev = slot(part.prev_sibling);
        if (prev != nullptr) {
            prev->next_sibling = part.next_sibling;
        }
    } else if (head != nullptr && *head == id) {
        *head = part.next_sibling;
    }
    if (part.next_sibling != 0) {
        Slot* next = slot(part.next_sibling);
        if (next != nullptr) {
            next->prev_sibling = part.prev_sibling;
        }
    } else if (tail != nullptr && *tail == id) {
        *tail = part.prev_sibling;
    }
    part.parent = kNoParent;
    part.prev_sibling = 0;
    part.next_sibling = 0;
}

void DataModel::link_child(InstanceId parent_id, InstanceId child) {
    Slot* part = slot(child);
    if (part == nullptr) {
        contract_fail("set_parent lost an instance");
    }
    state_->tree_revision.fetch_add(1, std::memory_order_relaxed);
    InstanceId* head = nullptr;
    InstanceId* tail = nullptr;
    if (parent_id == 0) {
        head = &state_->root_first_child;
        tail = &state_->root_last_child;
    } else {
        Slot* parent = slot(parent_id);
        if (parent == nullptr) {
            contract_fail("set_parent lost an instance");
        }
        head = &parent->first_child;
        tail = &parent->last_child;
    }
    // Last, so siblings stay in the order they arrived.
    part->parent = parent_id;
    part->next_sibling = 0;
    part->prev_sibling = 0;
    if (*head == 0) {
        *head = child;
        *tail = child;
        return;
    }
    Slot* last = slot(*tail);
    if (last == nullptr) {
        contract_fail("set_parent lost an instance");
    }
    last->next_sibling = child;
    part->prev_sibling = *tail;
    *tail = child;
}

void DataModel::detach_links(InstanceId id, Slot& part) {
    unlink_parent(id, part);
    InstanceId child = part.first_child;
    while (child != 0) {
        Slot* child_slot = slot(child);
        if (child_slot == nullptr) {
            break;
        }
        const InstanceId next = child_slot->next_sibling;
        child_slot->parent = kNoParent;
        child_slot->prev_sibling = 0;
        child_slot->next_sibling = 0;
        // Out of the tree now, with its own subtree.
        refresh_scope(child);
        child = next;
    }
    part.first_child = 0;
    part.last_child = 0;
}

bool DataModel::is_under(InstanceId ancestor, InstanceId node) const {
    InstanceId cursor = node;
    while (cursor != 0 && cursor != kNoParent) {
        if (cursor == ancestor) {
            return true;
        }
        const Slot* part = slot(cursor);
        if (part == nullptr) {
            return false;
        }
        cursor = part->parent;
    }
    return false;
}

void DataModel::set_parent(InstanceId id, InstanceId new_parent) {
    if (!gameplay_thread()) {
        contract_fail("set_parent runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail(id == 0 ? "set_parent on the root" : "set_parent on a dead instance");
    }
    if (new_parent != 0 && new_parent != kNoParent) {
        if (slot(new_parent) == nullptr) {
            contract_fail("set_parent to a dead instance");
        }
        if (new_parent == id || is_under(id, new_parent)) {
            contract_fail("set_parent would cycle");
        }
    }
    if (part->parent == new_parent) {
        return;
    }
    if (const std::optional<std::string> error = parent_error(id, new_parent)) {
        contract_fail(error->c_str());
    }
    const int old_index = part->parent == kNoParent ? -1 : sibling_index_of(id);
    const InstanceId old = part->parent;
    unlink_parent(id, *part);
    if (new_parent != kNoParent) {
        link_child(new_parent, id);
    }
    refresh_scope(id);
    record_parent(id, old, new_parent, old_index);
    note_tree_changed();
    const WriteOrigin origin = current_origin();
    emit_change(id, Field::Parent, origin);
    if (old != kNoParent) {
        emit_child(old, SignalKind::ChildRemoved, id, origin);
    }
    if (new_parent != kNoParent) {
        emit_child(new_parent, SignalKind::ChildAdded, id, origin);
    }
    emit_ancestry(id, origin);
    if (part->instance != nullptr) {
        part->instance->on_parent_changed(old, new_parent);
    }
    if (state_->script_host != nullptr) {
        state_->script_host->on_moved(id);
        if (new_parent != kNoParent) {
            state_->script_host->on_child_named(new_parent, id, name(id));
        }
    }
    if (DataModel* live = instance(id)) {
        if (dynamic_cast<LuaSource*>(live) != nullptr) {
            if (ScriptAnalysis* analysis = script_analysis()) {
                analysis->invalidate(id);
            }
        }
    }
}

void DataModel::release_signals(InstanceId id) {
    state_->events.destroy_instance(id);
    const std::uint32_t index = id_slot(id);
    if (index < state_->bags.size()) {
        state_->bags[index].reset();
    }
}

InstanceId DataModel::parent(InstanceId id) const {
    const Slot* part = slot(id);
    if (part == nullptr) {
        return kNoParent;
    }
    return part->parent;
}

InstanceId DataModel::first_child(InstanceId parent_id) const {
    if (parent_id == 0) {
        return state_->root_first_child;
    }
    const Slot* part = slot(parent_id);
    if (part == nullptr) {
        return 0;
    }
    return part->first_child;
}

InstanceId DataModel::next_sibling(InstanceId id) const {
    const Slot* part = slot(id);
    if (part == nullptr) {
        return 0;
    }
    return part->next_sibling;
}

const char* action_label(InstanceAction action) {
    switch (action) {
    case InstanceAction::Edit:
        return "Edit";
    case InstanceAction::Cut:
        return "Cut";
    case InstanceAction::Copy:
        return "Copy";
    case InstanceAction::Paste:
        return "Paste";
    case InstanceAction::Duplicate:
        return "Duplicate";
    case InstanceAction::Rename:
        return "Rename";
    case InstanceAction::Delete:
        return "Delete";
    }
    return "";
}

void DataModel::context_actions(std::vector<ContextAction>& out) const {
    out.push_back(ContextAction{InstanceAction::Cut, false});
    if (id_ != 0) {
        out.push_back(ContextAction{InstanceAction::Copy, false});
    }
    out.push_back(ContextAction{InstanceAction::Paste, false});
    if (id_ != 0) {
        out.push_back(ContextAction{InstanceAction::Duplicate, false});
    }
    out.push_back(ContextAction{InstanceAction::Rename, false});
    if (id_ != 0) {
        out.push_back(ContextAction{InstanceAction::Delete, false});
    }
}

InstanceId DataModel::scene_service(std::string_view class_name) const {
    for (InstanceId child = first_child(0); child != 0; child = next_sibling(child)) {
        const DataModel* object = instance(child);
        if (object != nullptr && object->is_scene_service() && class_name == object->class_name()) {
            return child;
        }
    }
    return 0;
}

InstanceId DataModel::service(std::string_view class_name) const {
    // kServices nests one level: a service's parent is game or a child of game.
    for (InstanceId child = first_child(0); child != 0; child = next_sibling(child)) {
        const DataModel* object = instance(child);
        if (object == nullptr || !object->is_service()) {
            continue;
        }
        if (class_name == object->class_name()) {
            return child;
        }
        if (object->is_scene_service()) {
            continue;
        }
        for (InstanceId inner = first_child(child); inner != 0; inner = next_sibling(inner)) {
            const DataModel* nested = instance(inner);
            if (nested != nullptr && nested->is_service() && class_name == nested->class_name()) {
                return inner;
            }
        }
    }
    return 0;
}

std::string DataModel::rule_class(InstanceId parent, InstanceId moved, InstanceId moved_to) const {
    InstanceId at = parent;
    for (std::size_t guard = 0; guard <= kMaxInstances + 1; ++guard) {
        if (at == kNoParent) {
            return {};
        }
        const DataModel* holder = at == 0 ? state_->root : instance(at);
        if (holder == nullptr) {
            return {};
        }
        if (!passes_rule_up(holder->class_name())) {
            return holder->class_name();
        }
        at = at == moved ? moved_to : this->parent(at);
    }
    return {};
}

std::optional<std::string> DataModel::placement_error_for(InstanceId id, InstanceId new_parent) const {
    std::vector<InstanceId> pending{id};
    while (!pending.empty()) {
        const InstanceId at = pending.back();
        pending.pop_back();
        const DataModel* object = instance(at);
        if (object == nullptr) {
            continue;
        }
        const std::string holder = rule_class(at == id ? new_parent : parent(at), id, new_parent);
        if (!holder.empty()) {
            if (std::optional<std::string> error = placement_error(holder, object->class_name(), name(at))) {
                return error;
            }
        }
        for (InstanceId child = first_child(at); child != 0; child = next_sibling(child)) {
            pending.push_back(child);
        }
    }
    return std::nullopt;
}

std::optional<std::string> DataModel::placement_error_for_class(InstanceId parent, std::string_view class_name) const {
    const std::string holder = rule_class(parent, kNoParent, parent);
    if (holder.empty()) {
        return std::nullopt;
    }
    return placement_error(holder, class_name, class_name);
}

std::optional<std::string> DataModel::parent_error(InstanceId id, InstanceId new_parent) const {
    if (id == 0) {
        return std::string("game cannot be moved");
    }
    const DataModel* object = instance(id);
    if (object == nullptr || (new_parent != 0 && new_parent != kNoParent && !alive(new_parent))) {
        return std::string("That instance no longer exists");
    }
    const InstanceId current = parent(id);
    if (current == new_parent) {
        return std::nullopt;
    }
    if (object->is_service()) {
        // Game places each one under its table parent once, when it makes the
        // world, and a project read does the same for one its files lack.
        const ServiceSpec* spec = find_service(object->class_name());
        InstanceId home = kNoParent;
        if (spec != nullptr) {
            home = spec->parent_class == nullptr ? 0 : service(spec->parent_class);
            if (spec->parent_class != nullptr && home == 0) {
                home = kNoParent;
            }
        }
        const bool placing = current == kNoParent && home != kNoParent && new_parent == home &&
                             service(object->class_name()) == 0;
        if (!placing) {
            return name(id) + " cannot be moved";
        }
        return std::nullopt;
    }
    if (new_parent == kNoParent) {
        return std::nullopt;
    }
    if (new_parent == id || is_under(id, new_parent)) {
        return "Cannot parent " + name(id) + " to itself or a descendant";
    }
    return placement_error_for(id, new_parent);
}

std::optional<std::string> DataModel::rename_error(InstanceId id, std::string_view new_name) const {
    if (id == 0) {
        return std::nullopt;
    }
    const DataModel* object = instance(id);
    if (object == nullptr) {
        return std::string("That instance no longer exists");
    }
    if (object->is_service() && object->name_ != new_name) {
        return object->name_ + " cannot be renamed";
    }
    return std::nullopt;
}

std::optional<std::string> DataModel::destroy_error(InstanceId id) const {
    if (id == 0) {
        return std::string("game cannot be destroyed");
    }
    const DataModel* object = instance(id);
    if (object == nullptr) {
        return std::string("That instance no longer exists");
    }
    if (object->is_service()) {
        return object->name_ + " cannot be destroyed";
    }
    return std::nullopt;
}

void DataModel::destroy_tree(InstanceId id) {
    if (id == 0 || !alive(id)) {
        return;
    }
    if (const std::optional<std::string> error = destroy_error(id)) {
        contract_fail(error->c_str());
    }
    // Preorder, then destroyed back to front: every descendant before its ancestors.
    std::vector<InstanceId> order;
    std::vector<InstanceId> pending{id};
    while (!pending.empty()) {
        const InstanceId next = pending.back();
        pending.pop_back();
        order.push_back(next);
        for (InstanceId child = first_child(next); child != 0; child = next_sibling(child)) {
            pending.push_back(child);
        }
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (alive(*it)) {
            destroy(*it);
        }
    }
}

void DataModel::set_name(InstanceId id, std::string name) {
    if (!gameplay_thread()) {
        contract_fail("set_name runs on SimulationThread");
    }
    DataModel* object = id == 0 ? state_->root : instance(id);
    if (object == nullptr) {
        contract_fail("set_name on a dead instance");
    }
    if (object->name_ == name) {
        return;
    }
    if (const std::optional<std::string> error = rename_error(id, name)) {
        contract_fail(error->c_str());
    }
    const std::string previous = object->name_;
    object->name_ = std::move(name);
    state_->tree_revision.fetch_add(1, std::memory_order_relaxed);
    record_string(id, Field::Name, previous, object->name_);
    note_tree_changed();
    emit_change(id, Field::Name, current_origin());
    if (state_->script_host != nullptr && id != 0) {
        const InstanceId under = parent(id);
        if (under != kNoParent) {
            state_->script_host->on_child_named(under, id, object->name_);
        }
    }
    if (dynamic_cast<LuaSource*>(object) != nullptr) {
        if (ScriptAnalysis* analysis = script_analysis()) {
            analysis->invalidate(id);
        }
    }
}

std::string DataModel::name(InstanceId id) const {
    if (id == 0) {
        return state_->root != nullptr ? state_->root->name_ : std::string();
    }
    const DataModel* object = instance(id);
    if (object == nullptr) {
        return {};
    }
    return object->name_;
}

InstanceId DataModel::find_first_child(InstanceId parent_id, std::string_view name) const {
    if (parent_id != 0 && slot(parent_id) == nullptr) {
        return 0;
    }
    for (InstanceId child = first_child(parent_id); child != 0; child = next_sibling(child)) {
        const DataModel* object = instance(child);
        if (object != nullptr && object->name_ == name) {
            return child;
        }
    }
    return 0;
}

std::vector<InstanceId> DataModel::get_children(InstanceId parent_id) const { return child_ids(parent_id); }

std::vector<InstanceId> DataModel::child_ids(InstanceId parent_id) const {
    std::vector<InstanceId> children;
    if (parent_id != 0 && slot(parent_id) == nullptr) {
        return children;
    }
    for (InstanceId child = first_child(parent_id); child != 0; child = next_sibling(child)) {
        children.push_back(child);
    }
    return children;
}

void DataModel::set_stop_hook(std::function<void()> hook) { state_->on_stop = std::move(hook); }

void DataModel::set_start_hook(std::function<void()> hook) { state_->on_start = std::move(hook); }

void DataModel::set_script_host(ScriptHost* host) { state_->script_host = host; }

void DataModel::set_script_analysis(ScriptAnalysis* analysis) { state_->script_analysis = analysis; }

ScriptAnalysis* DataModel::script_analysis() const { return state_->script_analysis; }

void DataModel::for_each_instance(const std::function<void(DataModel&)>& fn) {
    const std::uint32_t count = slot_count();
    for (std::uint32_t index = 0; index < count; ++index) {
        Slot& part = state_->slots[index];
        if (!part.alive || part.instance == nullptr) {
            continue;
        }
        fn(*part.instance);
    }
}

void DataModel::emit_own(Field field) { emit_change(id_, field, current_origin()); }

ScriptHost* DataModel::script_host() const { return state_->script_host; }

bool DataModel::on_gameplay_thread() const { return gameplay_thread(); }

std::uint32_t DataModel::world_generation() const { return state_->world_generation; }

bool DataModel::simulation_running() const { return state_->simulation_running; }

ChangeHistoryService& DataModel::history() { return *state_->history; }

const ChangeHistoryService& DataModel::history() const { return *state_->history; }

SelectionService& DataModel::selection() { return state_->selection; }

const SelectionService& DataModel::selection() const { return state_->selection; }

UserInputService& DataModel::input() { return state_->input; }

const UserInputService& DataModel::input() const { return state_->input; }

std::filesystem::path DataModel::resources_root() const {
    std::lock_guard<std::mutex> guard(state_->resources_mu);
    return state_->resources_root;
}

void DataModel::set_resources_root(std::filesystem::path root) {
    std::lock_guard<std::mutex> guard(state_->resources_mu);
    state_->resources_root = std::move(root);
}

std::string DataModel::guid(InstanceId id) const {
    const DataModel* object = id == 0 ? state_->root : instance(id);
    return object != nullptr ? object->guid_ : std::string();
}

void DataModel::set_guid(InstanceId id, std::string guid) {
    if (!valid_guid(guid)) {
        throw std::invalid_argument("malformed GUID \"" + guid + "\"");
    }
    DataModel* object = id == 0 ? state_->root : instance(id);
    if (object == nullptr) {
        contract_fail("set_guid on a dead instance");
    }
    if (object->guid_ == guid) {
        return;
    }
    object->guid_ = std::move(guid);
    mark_authored_dirty(id);
}

std::optional<InstanceId> DataModel::find_guid(std::string_view guid) const {
    if (guid.empty()) {
        return std::nullopt;
    }
    if (state_->root != nullptr && state_->root->guid_ == guid) {
        return InstanceId{0};
    }
    const std::uint32_t count = slot_count();
    for (std::uint32_t index = 0; index < count; ++index) {
        const Slot& part = state_->slots[index];
        if (part.alive && part.instance != nullptr && part.instance->guid_ == guid) {
            return make_instance_id(part.generation, index);
        }
    }
    return std::nullopt;
}

std::unordered_map<std::string, InstanceId> DataModel::guid_index() const {
    std::unordered_map<std::string, InstanceId> out;
    if (state_->root != nullptr && !state_->root->guid_.empty()) {
        out.emplace(state_->root->guid_, InstanceId{0});
    }
    const std::uint32_t count = slot_count();
    for (std::uint32_t index = 0; index < count; ++index) {
        const Slot& part = state_->slots[index];
        if (part.alive && part.instance != nullptr && !part.instance->guid_.empty()) {
            out.emplace(part.instance->guid_, make_instance_id(part.generation, index));
        }
    }
    return out;
}

const PropertyBag& DataModel::extra_properties(InstanceId id) const {
    const DataModel* object = id == 0 ? state_->root : instance(id);
    return object != nullptr ? object->extras_ : empty_bag();
}

void DataModel::set_extra_property(InstanceId id, std::string key, JsonValue value) {
    DataModel* object = id == 0 ? state_->root : instance(id);
    if (object == nullptr) {
        contract_fail("set_extra_property on a dead instance");
    }
    if (const JsonValue* current = bag_find(object->extras_, key)) {
        if (*current == value) {
            return;
        }
    }
    bag_set(object->extras_, std::move(key), std::move(value));
    mark_authored_dirty(id);
}

void DataModel::erase_extra_property(InstanceId id, std::string_view key) {
    DataModel* object = id == 0 ? state_->root : instance(id);
    if (object != nullptr && bag_erase(object->extras_, key)) {
        mark_authored_dirty(id);
    }
}

namespace {

// A saved registry property's default, as its class registered it.
JsonValue saved_default(const LuaField& field) {
    JsonValue value;
    std::string message;
    if (field.default_json == nullptr || !parse_json(field.default_json, value, message)) {
        contract_fail("a saved property has no default");
    }
    return value;
}

// Its value now, as a project file holds it. A read never changes the object.
bool saved_value(DataModel& world, const DataModel& object, const LuaField& field, JsonValue& out) {
    DataModel& self = const_cast<DataModel&>(object);
    LuaSlot slot;
    return field.read(world, self, slot) && slot_to_json(slot, field.type_name, out);
}

// value put through the property's write, which validates it.
bool write_saved(DataModel& world, DataModel& object, const LuaField& field, const JsonValue& value,
                 std::string& error) {
    LuaSlot slot;
    if (!slot_from_json(value, field.type_name, field.name, slot, error)) {
        return false;
    }
    if (!field.write(world, object, slot)) {
        error = slot.error.empty() ? std::string(field.name) + " was refused" : slot.error;
        return false;
    }
    return true;
}

}  // namespace

void DataModel::write_place(std::vector<std::byte>& out) const {
    const std::vector<LuaField> fields = lua_saved_fields(class_name());
    if (fields.empty()) {
        return;
    }
    // Every saved property, defaults too, as the JSON a file would hold.
    JsonValue values = JsonValue::object();
    for (const LuaField& field : fields) {
        JsonValue value;
        if (saved_value(*state_->root, *this, field, value)) {
            values.set(field.name, std::move(value));
        }
    }
    const std::string text = write_json(values);
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    out.insert(out.end(), bytes, bytes + text.size());
}

void DataModel::read_place(const std::byte* data, std::size_t size) {
    const std::vector<LuaField> fields = lua_saved_fields(class_name());
    if (fields.empty()) {
        return;
    }
    JsonValue values = JsonValue::object();
    std::string message;
    if (data != nullptr && size != 0) {
        parse_json(std::string(reinterpret_cast<const char*>(data), size), values, message);
    }
    for (const LuaField& field : fields) {
        const JsonValue* value = values.is_object() ? values.find(field.name) : nullptr;
        std::string error;
        write_saved(*state_->root, *this, field, value != nullptr ? *value : saved_default(field), error);
    }
}

void DataModel::save_properties(PropertyBag& out) const {
    for (const LuaField& field : lua_saved_fields(class_name())) {
        JsonValue value;
        if (saved_value(*state_->root, *this, field, value) && !(value == saved_default(field))) {
            bag_set(out, field.name, std::move(value));
        }
    }
    const Slot* part = slot(id_);
    if (part == nullptr || part->instance != this) {
        return;
    }
    if (has_tag(ecs_world(), part->entity, state_->ecs_ids.simulated)) {
        bag_set(out, "Simulated", JsonValue::boolean(true));
    }
    if (has_tag(ecs_world(), part->entity, state_->ecs_ids.visual_only)) {
        bag_set(out, "VisualOnly", JsonValue::boolean(true));
    }
}

void DataModel::default_properties(PropertyBag& out) const {
    for (const LuaField& field : lua_saved_fields(class_name())) {
        bag_set(out, field.name, saved_default(field));
    }
    const Slot* part = slot(id_);
    if (part == nullptr || part->instance != this) {
        return;
    }
    bag_set(out, "Simulated", JsonValue::boolean(false));
    bag_set(out, "VisualOnly", JsonValue::boolean(false));
}

bool DataModel::load_property(const std::string& key, const JsonValue& value, std::string& error) {
    for (const LuaField& field : lua_saved_fields(class_name())) {
        if (key == field.name) {
            write_saved(*state_->root, *this, field, value, error);
            return true;
        }
    }
    if (key != "Simulated" && key != "VisualOnly") {
        return false;
    }
    if (!value.is_bool()) {
        error = key + " must be true or false";
        return true;
    }
    if (slot(id_) == nullptr) {
        error = key + " is not a property of the root";
        return true;
    }
    if (key == "Simulated") {
        set_simulated(id_, value.as_bool());
    } else {
        set_visual_only(id_, value.as_bool());
    }
    return true;
}

PropertyBag DataModel::merged_properties(const DataModel& object) const {
    PropertyBag out;
    object.save_properties(out);
    for (const JsonValue::Member& member : object.extras_) {
        // A key the class writes itself wins over a stale extra of the same name.
        if (bag_find(out, member.first) == nullptr) {
            bag_set(out, member.first, member.second);
        }
    }
    return out;
}

AuthoredDirty DataModel::authored_dirty() const {
    AuthoredDirty out;
    out.all = state_->dirty_all;
    out.ids.assign(state_->dirty.begin(), state_->dirty.end());
    std::sort(out.ids.begin(), out.ids.end());
    return out;
}

void DataModel::clear_authored_dirty() {
    state_->dirty.clear();
    state_->dirty_all = false;
}

void DataModel::note_tree_changed() {
    if (state_->simulation_running) {
        return;
    }
    if (ScriptAnalysis* analysis = script_analysis()) {
        analysis->note_world_changed();
    }
}

void DataModel::mark_authored_dirty(InstanceId id) {
    if (id == kNoParent || state_->simulation_running) {
        return;
    }
    state_->dirty.insert(id);
    state_->revision.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t DataModel::authored_revision() const { return state_->revision.load(std::memory_order_relaxed); }

std::uint64_t DataModel::tree_revision() const { return state_->tree_revision.load(std::memory_order_relaxed); }

std::uint64_t DataModel::source_revision() const { return state_->source_revision.load(std::memory_order_relaxed); }

namespace {

bool read_lua_name(DataModel& world, DataModel& object, LuaSlot& out) {
    out.kind = LuaSlot::Kind::String;
    out.text = world.name(object.id());
    return true;
}

bool write_lua_name(DataModel& world, DataModel& object, LuaSlot& in) {
    if (std::optional<std::string> error = world.rename_error(object.id(), in.text)) {
        in.error = std::move(*error);
        return false;
    }
    world.set_name(object.id(), in.text);
    return true;
}

bool read_lua_class(DataModel&, DataModel& object, LuaSlot& out) {
    out.kind = LuaSlot::Kind::String;
    const char* name = object.class_name();
    out.text = name != nullptr ? name : "";
    return true;
}

bool read_lua_parent(DataModel& world, DataModel& object, LuaSlot& out) {
    const InstanceId parent = world.parent(object.id());
    if (parent == DataModel::kNoParent) {
        out.kind = LuaSlot::Kind::Nil;
        return true;
    }
    out.kind = LuaSlot::Kind::Instance;
    out.id = parent;
    return true;
}

bool write_lua_parent(DataModel& world, DataModel& object, LuaSlot& in) {
    const InstanceId parent = in.kind == LuaSlot::Kind::Nil ? DataModel::kNoParent : in.id;
    if (std::optional<std::string> error = world.parent_error(object.id(), parent)) {
        in.error = std::move(*error);
        return false;
    }
    world.set_parent(object.id(), parent);
    return true;
}

bool read_lua_changed(DataModel&, DataModel&, LuaSlot& out) {
    out.kind = LuaSlot::Kind::Signal;
    return true;
}

ANARCHY_LUA_REGISTER(register_datamodel_lua) {
    // Changed passes the name of the property that changed.
    static const LuaParam kChangedArgs[] = {{"property", "string"}};
    LuaField changed = lua_property("Changed", "Signal", false, read_lua_changed, nullptr);
    changed.params = kChangedArgs;
    changed.param_count = 1;
    const LuaField fields[] = {
        lua_property("Name", "string", true, read_lua_name, write_lua_name),
        lua_property("ClassName", "string", false, read_lua_class, nullptr),
        // game is a parent too, and it is not an Instance.
        lua_property("Parent", "DataModel?", true, read_lua_parent, write_lua_parent),
        changed,
    };
    register_lua_class("DataModel", nullptr, fields, 4);
}

}  // namespace

}  // namespace engine_core
