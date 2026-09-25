#include "DataModel.hpp"

#include "DataModelLock.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "TaskScheduler.hpp"

#include <cstring>
#include <mutex>
#include <new>
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

}  // namespace

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
    std::function<void()> on_stop;
    std::function<void()> on_start;
    ScriptHost* script_host = nullptr;
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
}

DataModel::DataModel(ChildTag, State& state, InstanceId id) : state_(&state), id_(id) {}

DataModel::~DataModel() {
    if (owned_) {
        owned_->events.shutdown();
        owned_.reset();
    }
}

DataModel::DataModel(DataModel&& other) noexcept
    : owned_(std::move(other.owned_)), state_(other.state_), id_(other.id_), name_(std::move(other.name_)) {
    if (owned_) {
        state_ = owned_.get();
        state_->root = this;
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
    if (owned_) {
        state_ = owned_.get();
        state_->root = this;
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
    if (self == state_->simulation_thread) {
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
    Slot& part = world.slots[index];
    part.pool = pool_index;
    part.storage = storage;
    part.instance = object;
    return *object;
}

DataModel& DataModel::create() { return create<DataModel>(); }

GameObject& DataModel::create_game_object() {
    GameObject& object = create<GameObject>();
    note(object.id(), VisualField::Transform | VisualField::Color | VisualField::Size, WriteOrigin::Simulation);
    return object;
}

void DataModel::destroy(InstanceId id) {
    if (state_->threads_running && std::this_thread::get_id() != state_->simulation_thread) {
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
}

void DataModel::apply_transform(InstanceId id, const Transform& transform, bool force) {
    if (state_->threads_running) {
        const std::thread::id self = std::this_thread::get_id();
        if (self != state_->simulation_thread && self != state_->render_thread) {
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
    target.transform_ = transform;
    const WriteOrigin origin = current_origin();
    note(id, VisualField::Transform, origin);
    emit_change(id, Field::Transform, origin);
}

void DataModel::apply_color(InstanceId id, ColorRgb color, bool force) {
    if (state_->threads_running) {
        const std::thread::id self = std::this_thread::get_id();
        if (self != state_->simulation_thread && self != state_->render_thread) {
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
    object->color_ = color;
    const WriteOrigin origin = current_origin();
    note(id, VisualField::Color, origin);
    emit_change(id, Field::Color, origin);
}

void DataModel::set_simulated(InstanceId id, bool simulated) {
    if (state_->threads_running && std::this_thread::get_id() != state_->simulation_thread) {
        contract_fail("set_simulated runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail("set_simulated on a dead instance");
    }
    if (part->simulated == simulated) {
        return;
    }
    part->simulated = simulated;
    emit_change(id, Field::Simulated, current_origin());
}

void DataModel::set_visual_only(InstanceId id, bool visual_only) {
    if (state_->threads_running && std::this_thread::get_id() != state_->simulation_thread) {
        contract_fail("set_visual_only runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail("set_visual_only on a dead instance");
    }
    if (part->visual_only == visual_only) {
        return;
    }
    part->visual_only = visual_only;
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
    const InstanceId old = part->parent;
    unlink_parent(id, *part);
    if (new_parent != kNoParent) {
        link_child(new_parent, id);
    }
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
    object->name_ = std::move(name);
    emit_change(id, Field::Name, current_origin());
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
    }
    rebuild_free_list();

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
    const LuaField fields[] = {
        lua_property("Name", "string", true, read_lua_name, write_lua_name),
        lua_property("ClassName", "string", false, read_lua_class, nullptr),
        lua_property("Parent", "Instance", true, read_lua_parent, write_lua_parent),
        lua_property("Changed", "Signal", false, read_lua_changed, nullptr),
    };
    register_lua_class("DataModel", nullptr, fields, 4);
}

}  // namespace

}  // namespace engine_core
