#include "DataModel.hpp"

#include "DataModelLock.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"
#include "TaskScheduler.hpp"
#include "TestTriangle.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine_core {
namespace {

constexpr std::uint32_t kIndexMask = 0xffffu;

bool same_transform(const Transform& a, const Transform& b) {
    return std::memcmp(a.m, b.m, sizeof(a.m)) == 0;
}

bool same_color(ColorRgb a, ColorRgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

int field_index(Field field) {
    const int index = static_cast<int>(field);
    if (index < 0 || index >= static_cast<int>(Field::Count)) {
        contract_fail("unknown field");
    }
    return index;
}

// How many DataModelLock guards this thread currently owns.
// The mutex itself is taken only for the outermost guard, so RenderThread's
// try_lock waits on a normal timed mutex instead of a recursive one.
thread_local int tlsHold = 0;
thread_local const char* tlsDeferred = nullptr;

struct InstancePool {
    const void* key = nullptr;
    std::size_t stride = 0;
    std::size_t align = 0;
    std::size_t count = 0;
    std::byte* memory = nullptr;
    std::vector<DataModel*> objects;
    std::vector<std::uint32_t> free;
    DataModel* (*construct)(void*, DataModel::ChildTag, DataModel::State&, InstanceId) = nullptr;
    void (*destroy)(DataModel*) = nullptr;

    ~InstancePool() {
        for (DataModel* object : objects) {
            if (object != nullptr && destroy != nullptr) {
                destroy(object);
            }
        }
        if (memory != nullptr) {
            ::operator delete(memory, stride * DataModel::kMaxInstances, std::align_val_t(align));
        }
    }
};

GameObject* as_game_object(DataModel* instance) { return dynamic_cast<GameObject*>(instance); }

const PropertyBag& empty_bag() {
    static const PropertyBag bag;
    return bag;
}

}  // namespace

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

struct DataModel::State {
    // Guards slots, free lists, invalidation, and resync.
    // SimulationThread may hold Write across a whole step and may re-enter
    // (thread-local depth; the mutex is taken once).
    // RenderThread may hold Write only inside Prepare (RenderStepped, PreRender, copy),
    // budget 2ms. PostRender does not hold it.
    // Workers never take it.
    std::timed_mutex write_mu;
    std::thread::id owner{};
    int write_depth = 0;
    std::thread::id simulation_thread{};
    std::thread::id render_thread{};
    // Set while Engine runs a paused edit on the caller. Empty otherwise.
    std::thread::id edit_owner{};
    bool threads_running = false;
    bool prerender_window = false;
    bool resync = false;

    // Guards commands only. Workers push, SimulationThread drains.
    // Hold is one record, never the gameplay step.
    std::mutex command_mu;
    std::vector<Command> commands;
    std::size_t command_head = 0;
    std::size_t command_tail = 0;
    std::size_t command_size = 0;

    std::vector<Slot> slots;
    std::vector<std::uint32_t> free_list;
    // Declared before the pools so their destructors still see history.
    // Members are destroyed in reverse order.
    std::unique_ptr<ChangeHistoryService> history;
    SelectionService selection;
    std::vector<std::unique_ptr<InstancePool>> pools;
    // First child of the root DataModel. 0 means the root has no children.
    InstanceId root_first_child = 0;
    InvalidationQueue invalidation;

    EventQueue events;
    std::vector<std::unique_ptr<InstanceSignals>> bags;
    // Id 0 is the root and has no slot. bags[0] belongs to the first created
    // instance, whose id is (generation << 16) | 0 and generation starts at 1.
    std::unique_ptr<InstanceSignals> root_signals;
    std::vector<InstanceId> walk;
    // Ids gathered for Heartbeat. Separate from walk, which ancestry mutates.
    std::vector<InstanceId> step_ids;

    // The root object. Children share this State and reach the root through here.
    DataModel* root = nullptr;
    std::uint32_t world_generation = 0;
    bool simulation_running = false;
    bool place_captured = false;
    PlaceSnapshot place;
    // Edit-mode authored changes a project save has not written yet.
    std::unordered_set<InstanceId> dirty;
    bool dirty_all = false;
    std::atomic<std::uint64_t> revision{0};
    std::function<void()> on_stop;
    std::function<void()> on_start;
    ScriptHost* script_host = nullptr;
    ScriptAnalysis* script_analysis = nullptr;
};

DataModel::DataModel() : owned_(std::make_unique<State>()), state_(owned_.get()) {
    State& world = *state_;
    world.slots.reserve(kMaxInstances);
    world.free_list.reserve(kMaxInstances);
    world.invalidation.reserve(kMaxInvalidations);
    world.commands.assign(kMaxCommands, Command{});
    world.bags.resize(kMaxInstances);
    world.walk.reserve(kMaxInstances);
    world.step_ids.reserve(kMaxInstances);
    world.events.watch_prerender(&world.prerender_window);
    world.root = this;
    name_ = class_name();
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

DataModel::DataModel(DataModel&& other) noexcept
    : owned_(std::move(other.owned_)),
      state_(other.state_),
      id_(other.id_),
      name_(std::move(other.name_)),
      guid_(std::move(other.guid_)),
      extras_(std::move(other.extras_)) {
    if (owned_) {
        state_ = owned_.get();
        state_->root = this;
        if (state_->history) {
            state_->history->rebind(*this);
        }
    }
    other.state_ = nullptr;
    other.id_ = 0;
}

DataModel& DataModel::operator=(DataModel&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (owned_) {
        owned_->events.shutdown();
        owned_.reset();
    }
    owned_ = std::move(other.owned_);
    state_ = other.state_;
    id_ = other.id_;
    name_ = std::move(other.name_);
    guid_ = std::move(other.guid_);
    extras_ = std::move(other.extras_);
    if (owned_) {
        state_ = owned_.get();
        state_->root = this;
        if (state_->history) {
            state_->history->rebind(*this);
        }
    }
    other.state_ = nullptr;
    other.id_ = 0;
    return *this;
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

bool DataModel::lock_write_blocking() {
    if (tlsHold > 0) {
        ++tlsHold;
        ++state_->write_depth;
        return true;
    }
    state_->write_mu.lock();
    state_->owner = std::this_thread::get_id();
    state_->write_depth = 1;
    tlsHold = 1;
    return true;
}

bool DataModel::lock_write_for(std::chrono::milliseconds budget) {
    if (tlsHold > 0) {
        ++tlsHold;
        ++state_->write_depth;
        return true;
    }
    if (!state_->write_mu.try_lock_for(budget)) {
        return false;
    }
    state_->owner = std::this_thread::get_id();
    state_->write_depth = 1;
    tlsHold = 1;
    return true;
}

void DataModel::unlock_write() {
    if (tlsHold <= 0) {
        contract_fail("DataModelLock released without a hold");
    }
    --tlsHold;
    --state_->write_depth;
    if (tlsHold > 0) {
        return;
    }
    state_->owner = std::thread::id{};
    state_->write_mu.unlock();
}

DataModel::Slot* DataModel::slot(InstanceId id) {
    const std::uint32_t index = id & kIndexMask;
    const std::uint32_t generation = id >> 16u;
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
    if (tlsHold > 0) {
        tlsDeferred = message;
        return false;
    }
    contract_fail(message);
}

bool DataModel::take_deferred_violation() {
    if (tlsDeferred == nullptr) {
        return false;
    }
    tlsDeferred = nullptr;
    return true;
}

bool DataModel::has_deferred_violation() const { return tlsDeferred != nullptr; }

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
        if (part.visual_only || force_sim_write) {
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
    if (state_->commands.empty() || state_->command_size == state_->commands.size()) {
        contract_fail("simulation command queue is full");
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
            destroy(command.id);
        } else if (command.type == Command::Type::Color) {
            apply_color(command.id, command.color, false);
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
    part.simulated = false;
    part.visual_only = false;
    part.pool = 0;
    part.storage = 0;
    part.instance = nullptr;
    part.parent = kNoParent;
    part.first_child = 0;
    part.next_sibling = 0;
    part.prev_sibling = 0;
    return (part.generation << 16u) | index;
}

DataModel& DataModel::spawn(const SpawnOps& ops) {
    if (!gameplay_thread()) {
        contract_fail("create runs on SimulationThread");
    }
    if (ops.construct == nullptr || ops.destroy == nullptr || ops.bytes == 0 || ops.align == 0) {
        contract_fail("create is missing a constructor");
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
        if (world.pools.size() >= 0xffffu) {
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

    std::uint32_t storage = 0;
    if (!pool->free.empty()) {
        storage = pool->free.back();
        pool->free.pop_back();
    } else {
        if (pool->count >= kMaxInstances) {
            contract_fail("instance capacity exhausted");
        }
        storage = static_cast<std::uint32_t>(pool->count);
        ++pool->count;
    }

    const InstanceId id = allocate();
    const std::uint32_t index = id & kIndexMask;
    DataModel* object = pool->objects[storage];
    if (object == nullptr) {
        void* memory = pool->memory + static_cast<std::size_t>(storage) * pool->stride;
        object = pool->construct(memory, ChildTag{}, *state_, id);
        pool->objects[storage] = object;
    } else {
        object->rebind(id);
        object->on_reuse();
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
    note(object.id(), VisualField::Transform | VisualField::Color | VisualField::Size, WriteOrigin::Simulation);
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
    const std::uint32_t index = id & kIndexMask;
    Slot* part = slot(id);
    if (part == nullptr) {
        return;
    }
    std::optional<AuthoredRecord> captured;
    if (state_->history && state_->history->wants_mutation()) {
        captured = capture_record(id, true);
    }
    // The parent's folder may become a leaf and its child order changes.
    mark_authored_dirty(part->parent);
    note_tree_changed();
    detach_links(id, *part);
    release_signals(id);
    if (part->instance != nullptr) {
        part->instance->on_release();
    }
    if (part->pool < state_->pools.size()) {
        state_->pools[part->pool]->free.push_back(part->storage);
    }
    part->instance = nullptr;
    part->alive = false;
    if (part->generation != 0xffffu) {
        ++part->generation;
        state_->free_list.push_back(index);
    }
    note(id, VisualField::Removed, current_origin());
    if (captured) {
        record_destroyed(std::move(*captured));
    }
}

void DataModel::apply_transform(InstanceId id, const Transform& transform, bool force) {
    if (state_->threads_running) {
        const std::thread::id self = std::this_thread::get_id();
        // A paused edit writes now, like SimulationThread. Any other thread enqueues.
        if (!gameplay_thread() && self != state_->render_thread) {
            Command command;
            command.type = Command::Type::Transform;
            command.id = id;
            command.transform = transform;
            enqueue(command);
            return;
        }
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        reject_write("transform write on a dead instance");
        return;
    }
    GameObject* object = as_game_object(part->instance);
    if (object == nullptr) {
        reject_write("transform write on an instance that is not a GameObject");
        return;
    }
    if (!authorize(*part, force)) {
        return;
    }
    GameObject& target = *object;
    if (same_transform(target.transform_, transform)) {
        return;
    }
    const Transform previous = target.transform_;
    target.transform_ = transform;
    record_transform(id, previous, target.transform_);
    const WriteOrigin origin = current_origin();
    note(id, VisualField::Transform, origin);
    emit_change(id, Field::Transform, origin);
}

void DataModel::apply_color(InstanceId id, ColorRgb color, bool force) {
    if (state_->threads_running) {
        const std::thread::id self = std::this_thread::get_id();
        if (!gameplay_thread() && self != state_->render_thread) {
            Command command;
            command.type = Command::Type::Color;
            command.id = id;
            command.color = color;
            enqueue(command);
            return;
        }
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        reject_write("color write on a dead instance");
        return;
    }
    GameObject* object = as_game_object(part->instance);
    if (object == nullptr) {
        reject_write("color write on an instance that is not a GameObject");
        return;
    }
    if (!authorize(*part, force)) {
        return;
    }
    if (same_color(object->color_, color)) {
        return;
    }
    const ColorRgb previous = object->color_;
    object->color_ = color;
    record_color(id, previous, object->color_);
    const WriteOrigin origin = current_origin();
    note(id, VisualField::Color, origin);
    emit_change(id, Field::Color, origin);
}

void DataModel::set_simulated(InstanceId id, bool simulated) {
    if (!gameplay_thread()) {
        contract_fail("set_simulated runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail("set_simulated on a dead instance");
    }
    if (part->simulated == simulated) {
        return;
    }
    const bool previous = part->simulated;
    part->simulated = simulated;
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
    if (part->visual_only == visual_only) {
        return;
    }
    const bool previous = part->visual_only;
    part->visual_only = visual_only;
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

bool DataModel::simulated(InstanceId id) const {
    const Slot* part = slot(id);
    return part != nullptr && part->simulated;
}

bool DataModel::visual_only(InstanceId id) const {
    const Slot* part = slot(id);
    return part != nullptr && part->visual_only;
}

void DataModel::integrate_simulated(double dt) {
    const float step = static_cast<float>(dt);
    State& world = *state_;
    for (std::uint32_t index = 0; index < world.slots.size(); ++index) {
        Slot& part = world.slots[index];
        if (!part.alive || !part.simulated || part.visual_only) {
            continue;
        }
        GameObject* object = as_game_object(part.instance);
        if (object == nullptr) {
            continue;
        }
        GameObject& body = *object;
        if (body.velocity_[0] == 0.f && body.velocity_[1] == 0.f && body.velocity_[2] == 0.f) {
            continue;
        }
        body.transform_.m[12] += body.velocity_[0] * step;
        body.transform_.m[13] += body.velocity_[1] * step;
        body.transform_.m[14] += body.velocity_[2] * step;
        const InstanceId id = (part.generation << 16u) | index;
        note(id, VisualField::Transform, WriteOrigin::Simulation);
    }
}

void DataModel::step_descendants(double dt) {
    std::vector<InstanceId>& ids = state_->step_ids;
    ids.clear();
    for (InstanceId child = state_->root_first_child; child != 0;) {
        if (ids.size() == ids.capacity()) {
            break;
        }
        ids.push_back(child);
        const Slot* part = slot(child);
        if (part == nullptr) {
            break;
        }
        child = part->next_sibling;
    }
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const Slot* part = slot(ids[index]);
        if (part == nullptr) {
            continue;
        }
        for (InstanceId child = part->first_child; child != 0;) {
            if (ids.size() == ids.capacity()) {
                break;
            }
            ids.push_back(child);
            const Slot* child_slot = slot(child);
            if (child_slot == nullptr) {
                break;
            }
            child = child_slot->next_sibling;
        }
    }
    const std::size_t count = ids.size();
    for (std::size_t index = 0; index < count; ++index) {
        DataModel* object = instance(ids[index]);
        if (object != nullptr) {
            object->step(dt);
        }
    }
}

DataModel::InstanceSignals* DataModel::bag_for(InstanceId id) {
    if (id == 0) {
        return state_->root_signals.get();
    }
    if (slot(id) == nullptr) {
        return nullptr;
    }
    const std::uint32_t index = id & kIndexMask;
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
    const std::uint32_t index = id & kIndexMask;
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

void DataModel::emit_change(InstanceId id, Field field, WriteOrigin origin) {
    InstanceSignals* bag = bag_for(id);
    if (bag == nullptr) {
        return;
    }
    if (bag->changed.bound() && bag->changed.listeners_ > 0) {
        state_->events.emit(bag->changed.id(), id, field, origin);
    }
    const int index = static_cast<int>(field);
    if (index >= 0 && index < static_cast<int>(Field::Count)) {
        Signal& prop = bag->property[index];
        if (prop.bound() && prop.listeners_ > 0) {
            state_->events.emit(prop.id(), id, field, origin);
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
    // Parent 0 is the root DataModel, which has no slot of its own.
    InstanceId* head = nullptr;
    if (part.parent == 0) {
        head = &state_->root_first_child;
    } else if (Slot* parent = slot(part.parent)) {
        head = &parent->first_child;
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
    InstanceId* head = nullptr;
    if (parent_id == 0) {
        head = &state_->root_first_child;
    } else {
        Slot* parent = slot(parent_id);
        if (parent == nullptr) {
            contract_fail("set_parent lost an instance");
        }
        head = &parent->first_child;
    }
    part->parent = parent_id;
    part->prev_sibling = 0;
    part->next_sibling = *head;
    if (*head != 0) {
        Slot* first = slot(*head);
        if (first != nullptr) {
            first->prev_sibling = child;
        }
    }
    *head = child;
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
        child = next;
    }
    part.first_child = 0;
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
        contract_fail("set_parent on a dead instance");
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
    const int old_index = part->parent == kNoParent ? -1 : sibling_index_of(id);
    const InstanceId old = part->parent;
    unlink_parent(id, *part);
    if (new_parent != kNoParent) {
        link_child(new_parent, id);
    }
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
    if (new_parent != kNoParent && state_->script_host != nullptr) {
        state_->script_host->on_child_named(new_parent, id, name(id));
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
    const std::uint32_t index = id & kIndexMask;
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

void DataModel::context_actions(std::vector<ContextAction>& out) const {
    out.push_back(ContextAction{"Cut", false});
    out.push_back(ContextAction{"Paste", false});
    out.push_back(ContextAction{"Rename", false});
    if (id_ != 0) {
        out.push_back(ContextAction{"Delete", false});
    }
}

void DataModel::destroy_tree(InstanceId id) {
    if (id == 0 || !alive(id)) {
        return;
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
    const std::string previous = object->name_;
    object->name_ = std::move(name);
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
    // Close an edit gesture before play. Play writes stay off the edit stack.
    if (state_->history) {
        state_->history->seal_edit_recording();
    }
    if (!state_->place_captured) {
        capture_place_unlocked();
    }
    state_->simulation_running = true;
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
}

void DataModel::capture_place_unlocked() {
    PlaceSnapshot shot;
    shot.root_name = state_->root != nullptr ? state_->root->name_ : std::string();
    if (state_->root != nullptr) {
        shot.root_guid = state_->root->guid_;
        shot.root_extras = state_->root->extras_;
    }
    shot.root_children = child_ids(0);
    const std::uint32_t count = slot_count();
    shot.instances.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        Slot& part = state_->slots[index];
        if (!part.alive || part.instance == nullptr) {
            continue;
        }
        if (part.pool >= state_->pools.size() || state_->pools[part.pool] == nullptr) {
            contract_fail("place capture lost an instance type");
        }
        PlaceRecord record;
        record.id = (part.generation << 16u) | index;
        record.type_key = state_->pools[part.pool]->key;
        record.parent = part.parent;
        record.children = child_ids(record.id);
        record.name = part.instance->name_;
        record.simulated = part.simulated;
        record.visual_only = part.visual_only;
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
    return 0xffffu;
}

void DataModel::retire_slot(std::uint32_t index, bool bump_generation) {
    Slot& part = state_->slots[index];
    if (!part.alive) {
        return;
    }
    const InstanceId id = (part.generation << 16u) | index;
    detach_links(id, part);
    release_signals(id);
    if (part.instance != nullptr) {
        part.instance->name_.clear();
        part.instance->guid_.clear();
        part.instance->extras_.clear();
        part.instance->on_release();
    }
    if (part.pool < state_->pools.size() && state_->pools[part.pool] != nullptr) {
        state_->pools[part.pool]->free.push_back(part.storage);
    }
    part.instance = nullptr;
    part.alive = false;
    part.simulated = false;
    part.visual_only = false;
    part.parent = kNoParent;
    part.first_child = 0;
    part.next_sibling = 0;
    part.prev_sibling = 0;
    if (bump_generation && part.generation != 0xffffu) {
        ++part.generation;
    }
}

void DataModel::adopt_slot(std::uint16_t pool_index, InstanceId id) {
    if (pool_index >= state_->pools.size() || state_->pools[pool_index] == nullptr) {
        contract_fail("place restore lost an instance type");
    }
    const std::uint32_t index = id & kIndexMask;
    const std::uint32_t generation = id >> 16u;
    Slot& part = state_->slots[index];
    part.generation = generation;
    part.alive = true;
    part.simulated = false;
    part.visual_only = false;
    part.parent = kNoParent;
    part.first_child = 0;
    part.next_sibling = 0;
    part.prev_sibling = 0;

    InstancePool& pool = *state_->pools[pool_index];
    std::uint32_t storage = 0;
    if (!pool.free.empty()) {
        storage = pool.free.back();
        pool.free.pop_back();
    } else {
        if (pool.count >= kMaxInstances) {
            contract_fail("instance capacity exhausted");
        }
        storage = static_cast<std::uint32_t>(pool.count);
        ++pool.count;
    }
    DataModel* object = pool.objects[storage];
    if (object == nullptr) {
        void* memory = pool.memory + static_cast<std::size_t>(storage) * pool.stride;
        object = pool.construct(memory, ChildTag{}, *state_, id);
        pool.objects[storage] = object;
    } else {
        object->rebind(id);
        object->on_reuse();
    }
    part.pool = pool_index;
    part.storage = storage;
    part.instance = object;
}

void DataModel::restore_record(const PlaceRecord& record) {
    const std::uint32_t index = record.id & kIndexMask;
    Slot& part = state_->slots[index];
    const bool same = part.alive && part.instance != nullptr && ((part.generation << 16u) | index) == record.id &&
                      part.pool < state_->pools.size() && state_->pools[part.pool] != nullptr &&
                      state_->pools[part.pool]->key == record.type_key;
    if (!same) {
        if (part.alive) {
            retire_slot(index, false);
        }
        const std::uint16_t pool_index = pool_index_for(record.type_key);
        if (pool_index == 0xffffu) {
            contract_fail("place restore lost an instance type");
        }
        adopt_slot(pool_index, record.id);
    }
    Slot& live = state_->slots[index];
    live.simulated = record.simulated;
    live.visual_only = record.visual_only;
    if (live.instance == nullptr) {
        contract_fail("place restore lost an instance");
    }
    live.instance->name_ = record.name;
    live.instance->guid_ = record.guid;
    live.instance->extras_ = record.extras;
    const std::byte* bytes = record.extra.empty() ? nullptr : record.extra.data();
    live.instance->read_place(bytes, record.extra.size());
}

void DataModel::clear_hierarchy() {
    state_->root_first_child = 0;
    for (Slot& part : state_->slots) {
        part.parent = kNoParent;
        part.first_child = 0;
        part.next_sibling = 0;
        part.prev_sibling = 0;
    }
}

void DataModel::link_children_front(InstanceId parent, const std::vector<InstanceId>& children) {
    for (std::size_t n = children.size(); n > 0; --n) {
        link_child(parent, children[n - 1]);
    }
}

void DataModel::rebuild_free_list() {
    state_->free_list.clear();
    for (std::uint32_t index = 0; index < state_->slots.size(); ++index) {
        if (!state_->slots[index].alive) {
            state_->free_list.push_back(index);
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
        const std::uint32_t index = record.id & kIndexMask;
        if (index >= state_->slots.size()) {
            contract_fail("place snapshot has an unknown instance");
        }
        if (pool_index_for(record.type_key) == 0xffffu) {
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

    for (std::uint32_t index = 0; index < state_->slots.size(); ++index) {
        Slot& part = state_->slots[index];
        if (!part.alive) {
            continue;
        }
        const InstanceId id = (part.generation << 16u) | index;
        if (ids.count(id) == 0) {
            retire_slot(index, true);
        }
    }
    for (const PlaceRecord& record : place.instances) {
        restore_record(record);
    }
    clear_hierarchy();
    link_children_front(0, place.root_children);
    for (const PlaceRecord& record : place.instances) {
        link_children_front(record.id, record.children);
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

namespace {

PropertyValue value_transform(const Transform& value) {
    PropertyValue out;
    out.prop = HistoryProp::Transform;
    out.transform = value;
    return out;
}

PropertyValue value_color(ColorRgb value) {
    PropertyValue out;
    out.prop = HistoryProp::Color;
    out.color = value;
    return out;
}

PropertyValue value_size(float x, float y, float z) {
    PropertyValue out;
    out.prop = HistoryProp::Size;
    out.size[0] = x;
    out.size[1] = y;
    out.size[2] = z;
    return out;
}

PropertyValue value_flag(HistoryProp prop, bool value) {
    PropertyValue out;
    out.prop = prop;
    out.flag = value;
    return out;
}

PropertyValue value_text(HistoryProp prop, std::string value) {
    PropertyValue out;
    out.prop = prop;
    out.text = std::move(value);
    return out;
}

PropertyValue value_position(const Vec3& value) {
    PropertyValue out;
    out.prop = HistoryProp::Position;
    out.vector = value;
    return out;
}

HistoryProp history_prop(Field field) {
    switch (field) {
    case Field::Transform:
        return HistoryProp::Transform;
    case Field::Color:
        return HistoryProp::Color;
    case Field::Size:
        return HistoryProp::Size;
    case Field::Simulated:
        return HistoryProp::Simulated;
    case Field::VisualOnly:
        return HistoryProp::VisualOnly;
    case Field::Name:
        return HistoryProp::Name;
    case Field::Source:
        return HistoryProp::Source;
    case Field::Enabled:
        return HistoryProp::Enabled;
    case Field::LinearVelocity:
    case Field::Parent:
    case Field::Count:
        break;
    }
    return HistoryProp::Name;
}

void note_property(ChangeHistoryService* history, InstanceId id, PropertyValue before, PropertyValue after) {
    if (history == nullptr) {
        return;
    }
    Mutation mutation;
    mutation.kind = MutationKind::SetProperty;
    mutation.id = id;
    mutation.before = std::move(before);
    mutation.after = std::move(after);
    history->note(std::move(mutation));
}

}  // namespace

void DataModel::record_transform(InstanceId id, const Transform& before, const Transform& after) {
    mark_authored_dirty(id);
    note_property(state_->history.get(), id, value_transform(before), value_transform(after));
}

void DataModel::record_color(InstanceId id, ColorRgb before, ColorRgb after) {
    mark_authored_dirty(id);
    note_property(state_->history.get(), id, value_color(before), value_color(after));
}

void DataModel::record_size(InstanceId id, float bx, float by, float bz, float ax, float ay, float az) {
    mark_authored_dirty(id);
    note_property(state_->history.get(), id, value_size(bx, by, bz), value_size(ax, ay, az));
}

void DataModel::record_bool(InstanceId id, Field field, bool before, bool after) {
    if (field != Field::Simulated && field != Field::VisualOnly && field != Field::Enabled) {
        return;
    }
    mark_authored_dirty(id);
    const HistoryProp prop = history_prop(field);
    note_property(state_->history.get(), id, value_flag(prop, before), value_flag(prop, after));
}

void DataModel::record_string(InstanceId id, Field field, const std::string& before, const std::string& after) {
    if (field != Field::Name && field != Field::Source) {
        return;
    }
    mark_authored_dirty(id);
    const HistoryProp prop = history_prop(field);
    note_property(state_->history.get(), id, value_text(prop, before), value_text(prop, after));
}

void DataModel::record_position(InstanceId id, const Vec3& before, const Vec3& after) {
    mark_authored_dirty(id);
    note_property(state_->history.get(), id, value_position(before), value_position(after));
}

void DataModel::record_parent(InstanceId id, InstanceId old_parent, InstanceId new_parent, int old_index) {
    // The child's path moves. Each parent's child order, and whether it is a
    // folder or a leaf, may change.
    mark_authored_dirty(id);
    mark_authored_dirty(old_parent);
    mark_authored_dirty(new_parent);
    if (state_->history == nullptr) {
        return;
    }
    Mutation mutation;
    mutation.kind = MutationKind::SetParent;
    mutation.id = id;
    mutation.old_parent = old_parent;
    mutation.new_parent = new_parent;
    mutation.old_sibling_index = old_index;
    state_->history->note(std::move(mutation));
}

void DataModel::record_created(InstanceId id) {
    if (state_->history == nullptr || !state_->history->wants_mutation()) {
        return;
    }
    Mutation mutation;
    mutation.kind = MutationKind::CreateInstance;
    mutation.id = id;
    mutation.record = capture_record(id, false);
    state_->history->note(std::move(mutation));
}

void DataModel::record_destroyed(AuthoredRecord record) {
    if (state_->history == nullptr) {
        return;
    }
    Mutation mutation;
    mutation.kind = MutationKind::DestroyInstance;
    mutation.id = record.id;
    mutation.record = std::move(record);
    state_->history->note(std::move(mutation));
}

const void* DataModel::type_key_of(InstanceId id) const {
    const Slot* part = slot(id);
    if (part == nullptr || part->pool >= state_->pools.size() || state_->pools[part->pool] == nullptr) {
        return nullptr;
    }
    return state_->pools[part->pool]->key;
}

int DataModel::sibling_index_of(InstanceId id) const {
    const Slot* part = slot(id);
    if (part == nullptr || part->parent == kNoParent) {
        return -1;
    }
    int index = 0;
    for (InstanceId child = first_child(part->parent); child != 0; child = next_sibling(child)) {
        if (child == id) {
            return index;
        }
        ++index;
    }
    return -1;
}

void DataModel::take_free_index(std::uint32_t index) {
    std::vector<std::uint32_t>& free = state_->free_list;
    free.erase(std::remove(free.begin(), free.end(), index), free.end());
}

AuthoredRecord DataModel::capture_record(InstanceId id, bool subtree) const {
    AuthoredRecord record;
    const Slot* part = slot(id);
    if (part == nullptr || part->instance == nullptr) {
        return record;
    }
    const DataModel* object = part->instance;
    record.id = id;
    record.type_key = type_key_of(id);
    const char* class_name = object->class_name();
    record.class_name = class_name != nullptr ? class_name : "";
    record.name = object->name_;
    record.guid = object->guid_;
    record.extras = object->extras_;
    record.parent = part->parent;
    record.sibling_index = sibling_index_of(id);
    record.simulated = part->simulated;
    record.visual_only = part->visual_only;
    if (const GameObject* body = dynamic_cast<const GameObject*>(object)) {
        record.spatial = true;
        record.transform = body->transform();
        record.color = body->color();
        body->copy_size(record.size);
    }
    if (const LuaSource* source = dynamic_cast<const LuaSource*>(object)) {
        record.has_source = true;
        record.source = source->source();
        record.enabled = source->enabled();
    }
    if (!record.spatial && !record.has_source) {
        object->write_place(record.extra);
    }
    if (subtree) {
        for (InstanceId child = part->first_child; child != 0; child = next_sibling(child)) {
            record.children.push_back(capture_record(child, true));
        }
    }
    return record;
}

void DataModel::apply_record_fields(const AuthoredRecord& record) {
    if (DataModel* object = instance(record.id)) {
        object->guid_ = record.guid.empty() ? make_guid() : record.guid;
        object->extras_ = record.extras;
    }
    mark_authored_dirty(record.id);
    set_name(record.id, record.name);
    set_simulated(record.id, record.simulated);
    set_visual_only(record.id, record.visual_only);
    if (record.spatial) {
        if (GameObject* body = game_object(record.id)) {
            body->set_transform(record.transform);
            body->set_color(record.color);
            body->set_size(record.size[0], record.size[1], record.size[2]);
        }
    }
    if (record.has_source) {
        if (auto* source = dynamic_cast<LuaSource*>(instance(record.id))) {
            source->set_source(record.source);
            source->set_enabled(record.enabled);
        }
    } else if (!record.spatial) {
        if (DataModel* object = instance(record.id)) {
            const std::byte* bytes = record.extra.empty() ? nullptr : record.extra.data();
            object->read_place(bytes, record.extra.size());
        }
    }
}

void DataModel::revive_record(const AuthoredRecord& record) {
    if (record.id == 0 || record.type_key == nullptr || alive(record.id)) {
        return;
    }
    const std::uint32_t index = record.id & kIndexMask;
    if (index >= state_->slots.size()) {
        contract_fail("history revive lost an instance");
    }
    if (state_->slots[index].alive) {
        contract_fail("history revive collided with a live instance");
    }
    const std::uint16_t pool = pool_index_for(record.type_key);
    if (pool == 0xffffu) {
        contract_fail("history revive lost an instance type");
    }
    take_free_index(index);
    adopt_slot(pool, record.id);
    apply_record_fields(record);
    if (record.spatial) {
        note(record.id, VisualField::Transform | VisualField::Color | VisualField::Size, WriteOrigin::Simulation);
    }
    if (dynamic_cast<LuaSource*>(instance(record.id)) != nullptr) {
        if (ScriptAnalysis* analysis = script_analysis()) {
            analysis->invalidate(record.id);
        }
    }
}

void DataModel::revive_tree(const AuthoredRecord& record) {
    if (!alive(record.id)) {
        revive_record(record);
    }
    for (const AuthoredRecord& child : record.children) {
        revive_tree(child);
    }
}

void DataModel::place_at_sibling(InstanceId id, int index) {
    Slot* part = slot(id);
    if (part == nullptr || index < 0 || part->parent == kNoParent) {
        return;
    }
    const InstanceId parent = part->parent;
    std::vector<InstanceId> kids = child_ids(parent);
    const auto found = std::find(kids.begin(), kids.end(), id);
    if (found == kids.end()) {
        return;
    }
    const int current = static_cast<int>(found - kids.begin());
    if (current == index) {
        return;
    }
    kids.erase(found);
    if (index > static_cast<int>(kids.size())) {
        index = static_cast<int>(kids.size());
    }
    kids.insert(kids.begin() + index, id);
    mark_authored_dirty(parent);
    note_tree_changed();
    for (InstanceId child : kids) {
        if (Slot* child_slot = slot(child)) {
            unlink_parent(child, *child_slot);
        }
    }
    link_children_front(parent, kids);
}

void DataModel::reparent_record(const AuthoredRecord& record) {
    if (!alive(record.id)) {
        return;
    }
    if (parent(record.id) != record.parent) {
        set_parent(record.id, record.parent);
    }
    if (record.parent != kNoParent && record.sibling_index >= 0) {
        place_at_sibling(record.id, record.sibling_index);
    }
    std::vector<const AuthoredRecord*> ordered;
    ordered.reserve(record.children.size());
    for (const AuthoredRecord& child : record.children) {
        ordered.push_back(&child);
    }
    std::sort(ordered.begin(), ordered.end(), [](const AuthoredRecord* a, const AuthoredRecord* b) {
        return a->sibling_index < b->sibling_index;
    });
    for (const AuthoredRecord* child : ordered) {
        reparent_record(*child);
    }
}

void DataModel::apply_property(InstanceId id, const PropertyValue& value) {
    if (id != 0 && !alive(id)) {
        return;
    }
    switch (value.prop) {
    case HistoryProp::Transform:
        if (GameObject* body = game_object(id)) {
            body->set_transform(value.transform);
        }
        break;
    case HistoryProp::Color:
        if (GameObject* body = game_object(id)) {
            body->set_color(value.color);
        }
        break;
    case HistoryProp::Size:
        if (GameObject* body = game_object(id)) {
            body->set_size(value.size[0], value.size[1], value.size[2]);
        }
        break;
    case HistoryProp::Simulated:
        set_simulated(id, value.flag);
        break;
    case HistoryProp::VisualOnly:
        set_visual_only(id, value.flag);
        break;
    case HistoryProp::Name:
        set_name(id, value.text);
        break;
    case HistoryProp::Source:
        if (auto* source = dynamic_cast<LuaSource*>(instance(id))) {
            source->set_source(value.text);
        }
        break;
    case HistoryProp::Enabled:
        if (auto* source = dynamic_cast<LuaSource*>(instance(id))) {
            source->set_enabled(value.flag);
        }
        break;
    case HistoryProp::Position:
        if (auto* triangle = dynamic_cast<TestTriangle*>(instance(id))) {
            triangle->set_position(value.vector.x, value.vector.y, value.vector.z);
        }
        break;
    }
}

void DataModel::apply_parent(InstanceId id, InstanceId parent_id, int sibling_index) {
    if (!alive(id)) {
        return;
    }
    if (parent(id) != parent_id) {
        set_parent(id, parent_id);
    }
    if (sibling_index >= 0 && parent_id != kNoParent) {
        place_at_sibling(id, sibling_index);
    }
}

void DataModel::apply_history(const Mutation& mutation, bool inverse) {
    switch (mutation.kind) {
    case MutationKind::SetProperty:
        apply_property(mutation.id, inverse ? mutation.before : mutation.after);
        break;
    case MutationKind::SetParent:
        if (inverse) {
            apply_parent(mutation.id, mutation.old_parent, mutation.old_sibling_index);
        } else {
            apply_parent(mutation.id, mutation.new_parent, -1);
        }
        break;
    case MutationKind::CreateInstance:
        if (inverse) {
            if (mutation.record.id != 0 && alive(mutation.record.id)) {
                destroy(mutation.record.id);
            }
        } else {
            revive_record(mutation.record);
        }
        break;
    case MutationKind::DestroyInstance:
        if (inverse) {
            revive_tree(mutation.record);
            reparent_record(mutation.record);
        } else if (mutation.record.id != 0 && alive(mutation.record.id)) {
            destroy(mutation.record.id);
        }
        break;
    }
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
            return (part.generation << 16u) | index;
        }
    }
    return std::nullopt;
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

void DataModel::save_properties(PropertyBag& out) const {
    const Slot* part = slot(id_);
    if (part == nullptr || part->instance != this) {
        return;
    }
    if (part->simulated) {
        bag_set(out, "Simulated", JsonValue::boolean(true));
    }
    if (part->visual_only) {
        bag_set(out, "VisualOnly", JsonValue::boolean(true));
    }
}

bool DataModel::load_property(const std::string& key, const JsonValue& value, std::string& error) {
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
                if (object == nullptr) {
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

namespace {

bool read_lua_name(DataModel& world, DataModel& object, LuaSlot& out) {
    out.kind = LuaSlot::Kind::String;
    out.text = world.name(object.id());
    return true;
}

bool write_lua_name(DataModel& world, DataModel& object, LuaSlot& in) {
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
    world.set_parent(object.id(), in.kind == LuaSlot::Kind::Nil ? DataModel::kNoParent : in.id);
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
        lua_property("Parent", "Instance?", true, read_lua_parent, write_lua_parent),
        changed,
    };
    register_lua_class("DataModel", nullptr, fields, 4);
}

}  // namespace

}  // namespace engine_core
