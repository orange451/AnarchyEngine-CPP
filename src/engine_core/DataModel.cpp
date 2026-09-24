#include "DataModel.hpp"

#include "GameObject.hpp"

#include <cstring>
#include <mutex>
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
    std::vector<DataModel> plains;
    std::vector<std::uint32_t> free_plains;
    std::vector<GameObject> objects;
    std::vector<std::uint32_t> free_objects;
    InvalidationQueue invalidation;

    EventQueue events;
    std::vector<std::unique_ptr<InstanceSignals>> bags;
    std::vector<InstanceId> walk;
};

DataModel::DataModel() : owned_(std::make_unique<State>()), state_(owned_.get()) {
    State& world = *state_;
    world.slots.reserve(kMaxInstances);
    world.free_list.reserve(kMaxInstances);
    world.plains.reserve(kMaxInstances);
    world.free_plains.reserve(kMaxInstances);
    world.objects.reserve(kMaxInstances);
    world.free_objects.reserve(kMaxInstances);
    world.invalidation.reserve(kMaxInvalidations);
    world.commands.assign(kMaxCommands, Command{});
    world.bags.resize(kMaxInstances);
    world.walk.reserve(kMaxInstances);
    world.events.watch_prerender(&world.prerender_window);
}

DataModel::DataModel(ChildTag, State& state, InstanceId id) : state_(&state), id_(id) {}

DataModel::~DataModel() {
    if (owned_) {
        owned_->events.shutdown();
        owned_.reset();
    }
}

DataModel::DataModel(DataModel&& other) noexcept
    : owned_(std::move(other.owned_)), state_(other.state_), id_(other.id_) {
    if (owned_) {
        state_ = owned_.get();
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
    if (owned_) {
        state_ = owned_.get();
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
    if (state_->threads_running && std::this_thread::get_id() != state_->simulation_thread) {
        contract_fail(message);
    }
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
    if (!part.alive || part.kind != InstanceKind::GameObject || part.storage >= state_->objects.size()) {
        return nullptr;
    }
    return &state_->objects[part.storage];
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

InstanceId DataModel::allocate(InstanceKind kind) {
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
    part.kind = kind;
    part.storage = 0;
    part.parent = 0;
    part.first_child = 0;
    part.next_sibling = 0;
    part.prev_sibling = 0;
    return (part.generation << 16u) | index;
}

DataModel& DataModel::create() {
    if (state_->threads_running && std::this_thread::get_id() != state_->simulation_thread) {
        contract_fail("create runs on SimulationThread");
    }
    const InstanceId id = allocate(InstanceKind::Plain);
    const std::uint32_t index = id & kIndexMask;
    State& world = *state_;
    std::uint32_t storage = 0;
    DataModel* object = nullptr;
    if (!world.free_plains.empty()) {
        storage = world.free_plains.back();
        world.free_plains.pop_back();
        object = &world.plains[storage];
        object->rebind(id);
    } else {
        if (world.plains.size() == world.plains.capacity()) {
            contract_fail("instance capacity exhausted");
        }
        world.plains.emplace_back(ChildTag{}, *state_, id);
        storage = static_cast<std::uint32_t>(world.plains.size() - 1);
        object = &world.plains.back();
    }
    world.slots[index].storage = storage;
    return *object;
}

GameObject& DataModel::create_game_object() {
    if (state_->threads_running && std::this_thread::get_id() != state_->simulation_thread) {
        contract_fail("create_game_object runs on SimulationThread");
    }
    const InstanceId id = allocate(InstanceKind::GameObject);
    const std::uint32_t index = id & kIndexMask;
    State& world = *state_;
    std::uint32_t storage = 0;
    GameObject* object = nullptr;
    if (!world.free_objects.empty()) {
        storage = world.free_objects.back();
        world.free_objects.pop_back();
        object = &world.objects[storage];
        object->rebind(id);
        object->reset_spatial();
    } else {
        if (world.objects.size() == world.objects.capacity()) {
            contract_fail("instance capacity exhausted");
        }
        world.objects.emplace_back(ChildTag{}, *state_, id);
        storage = static_cast<std::uint32_t>(world.objects.size() - 1);
        object = &world.objects.back();
    }
    world.slots[index].storage = storage;
    note(id, VisualField::Transform | VisualField::Color | VisualField::Size, WriteOrigin::Simulation);
    return *object;
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
    if (part->kind == InstanceKind::GameObject && part->storage < state_->objects.size()) {
        state_->objects[part->storage].clear_spatial();
    }
    part->alive = false;
    if (part->generation != 0xffffu) {
        ++part->generation;
        state_->free_list.push_back(index);
        if (part->kind == InstanceKind::GameObject) {
            state_->free_objects.push_back(part->storage);
        } else {
            state_->free_plains.push_back(part->storage);
        }
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
    if (part->kind != InstanceKind::GameObject || part->storage >= state_->objects.size()) {
        reject_write("transform write on an instance that is not a GameObject");
        return;
    }
    if (!authorize(*part, force)) {
        return;
    }
    GameObject& object = state_->objects[part->storage];
    if (same_transform(object.transform_, transform)) {
        return;
    }
    object.transform_ = transform;
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
    if (part->kind != InstanceKind::GameObject || part->storage >= state_->objects.size()) {
        reject_write("color write on an instance that is not a GameObject");
        return;
    }
    if (!authorize(*part, force)) {
        return;
    }
    GameObject& object = state_->objects[part->storage];
    if (same_color(object.color_, color)) {
        return;
    }
    object.color_ = color;
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

GameObject* DataModel::game_object(InstanceId id) {
    Slot* part = slot(id);
    if (part == nullptr || part->kind != InstanceKind::GameObject || part->storage >= state_->objects.size()) {
        return nullptr;
    }
    return &state_->objects[part->storage];
}

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
        if (part.kind != InstanceKind::GameObject || part.storage >= world.objects.size()) {
            continue;
        }
        GameObject& object = world.objects[part.storage];
        if (object.velocity_[0] == 0.f && object.velocity_[1] == 0.f && object.velocity_[2] == 0.f) {
            continue;
        }
        object.transform_.m[12] += object.velocity_[0] * step;
        object.transform_.m[13] += object.velocity_[1] * step;
        object.transform_.m[14] += object.velocity_[2] * step;
        const InstanceId id = (part.generation << 16u) | index;
        note(id, VisualField::Transform, WriteOrigin::Simulation);
    }
}

DataModel::InstanceSignals* DataModel::bag_for(InstanceId id) {
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
    if (part.parent == 0) {
        part.prev_sibling = 0;
        part.next_sibling = 0;
        return;
    }
    Slot* parent = slot(part.parent);
    if (part.prev_sibling != 0) {
        Slot* prev = slot(part.prev_sibling);
        if (prev != nullptr) {
            prev->next_sibling = part.next_sibling;
        }
    } else if (parent != nullptr && parent->first_child == id) {
        parent->first_child = part.next_sibling;
    }
    if (part.next_sibling != 0) {
        Slot* next = slot(part.next_sibling);
        if (next != nullptr) {
            next->prev_sibling = part.prev_sibling;
        }
    }
    part.parent = 0;
    part.prev_sibling = 0;
    part.next_sibling = 0;
}

void DataModel::link_child(InstanceId parent_id, InstanceId child) {
    Slot* parent = slot(parent_id);
    Slot* part = slot(child);
    if (parent == nullptr || part == nullptr) {
        contract_fail("set_parent lost an instance");
    }
    part->parent = parent_id;
    part->prev_sibling = 0;
    part->next_sibling = parent->first_child;
    if (parent->first_child != 0) {
        Slot* first = slot(parent->first_child);
        if (first != nullptr) {
            first->prev_sibling = child;
        }
    }
    parent->first_child = child;
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
        child_slot->parent = 0;
        child_slot->prev_sibling = 0;
        child_slot->next_sibling = 0;
        child = next;
    }
    part.first_child = 0;
}

bool DataModel::is_under(InstanceId ancestor, InstanceId node) const {
    InstanceId cursor = node;
    while (cursor != 0) {
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
    if (state_->threads_running && std::this_thread::get_id() != state_->simulation_thread) {
        contract_fail("set_parent runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail("set_parent on a dead instance");
    }
    if (new_parent != 0) {
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
    if (new_parent != 0) {
        link_child(new_parent, id);
    }
    const WriteOrigin origin = current_origin();
    emit_change(id, Field::Parent, origin);
    if (old != 0) {
        emit_child(old, SignalKind::ChildRemoved, id, origin);
    }
    if (new_parent != 0) {
        emit_child(new_parent, SignalKind::ChildAdded, id, origin);
    }
    emit_ancestry(id, origin);
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
        return 0;
    }
    return part->parent;
}

}  // namespace engine_core
