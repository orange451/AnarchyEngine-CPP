#include "DataModel.hpp"

#include <cstring>

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

DataModel::DataModel() {
    slots_.reserve(kMaxInstances);
    free_list_.reserve(kMaxInstances);
    invalidation_.reserve(kMaxInvalidations);
    commands_.assign(kMaxCommands, Command{});
    bags_.resize(kMaxInstances);
    walk_.reserve(kMaxInstances);
    events_.watch_prerender(&prerender_window_);
}

DataModel::~DataModel() { events_.shutdown(); }

void DataModel::attach_scheduler(TaskScheduler* scheduler) { events_.attach_scheduler(scheduler); }

void DataModel::set_thread_ids(std::thread::id simulation, std::thread::id render) {
    simulation_thread_ = simulation;
    render_thread_ = render;
}

void DataModel::set_threads_running(bool running) { threads_running_ = running; }

bool DataModel::lock_write_blocking() {
    if (tlsHold > 0) {
        ++tlsHold;
        ++write_depth_;
        return true;
    }
    write_mu_.lock();
    owner_ = std::this_thread::get_id();
    write_depth_ = 1;
    tlsHold = 1;
    return true;
}

bool DataModel::lock_write_for(std::chrono::milliseconds budget) {
    if (tlsHold > 0) {
        ++tlsHold;
        ++write_depth_;
        return true;
    }
    if (!write_mu_.try_lock_for(budget)) {
        return false;
    }
    owner_ = std::this_thread::get_id();
    write_depth_ = 1;
    tlsHold = 1;
    return true;
}

void DataModel::unlock_write() {
    if (tlsHold <= 0) {
        contract_fail("DataModelLock released without a hold");
    }
    --tlsHold;
    --write_depth_;
    if (tlsHold > 0) {
        return;
    }
    owner_ = std::thread::id{};
    write_mu_.unlock();
}

DataModel::Slot* DataModel::slot(InstanceId id) {
    const std::uint32_t index = id & kIndexMask;
    const std::uint32_t generation = id >> 16u;
    if (index >= slots_.size()) {
        return nullptr;
    }
    Slot& part = slots_[index];
    if (!part.alive || part.generation != generation) {
        return nullptr;
    }
    return &part;
}

const DataModel::Slot* DataModel::slot(InstanceId id) const {
    return const_cast<DataModel*>(this)->slot(id);
}

WriteOrigin DataModel::current_origin() const {
    if (threads_running_ && std::this_thread::get_id() == render_thread_) {
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
    if (!threads_running_) {
        return true;
    }
    const std::thread::id self = std::this_thread::get_id();
    if (self == simulation_thread_) {
        return true;
    }
    if (self == render_thread_) {
        if (!prerender_window_) {
            return reject_write("DataModel write from RenderThread outside PreRender");
        }
        if (part.visual_only || force_sim_write) {
            return true;
        }
        return reject_write("PreRender DataModel write requires visual_only or ForceSimWrite");
    }
    return true;
}

void DataModel::note(InstanceId id, VisualField fields, WriteOrigin origin) {
    invalidation_.push(Invalidation{id, fields, origin});
    if (invalidation_.overflow()) {
        resync_ = true;
    }
}

void DataModel::enqueue(Command command) {
    std::lock_guard<std::mutex> guard(command_mu_);
    if (commands_.empty() || command_size_ == commands_.size()) {
        contract_fail("simulation command queue is full");
    }
    commands_[command_tail_] = command;
    command_tail_ = (command_tail_ + 1) % commands_.size();
    ++command_size_;
}

void DataModel::drain_commands() {
    while (true) {
        Command command;
        {
            std::lock_guard<std::mutex> guard(command_mu_);
            if (command_size_ == 0) {
                return;
            }
            command = commands_[command_head_];
            command_head_ = (command_head_ + 1) % commands_.size();
            --command_size_;
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
    const bool was = resync_;
    resync_ = false;
    return was;
}

InstanceId DataModel::create_part() {
    if (threads_running_ && std::this_thread::get_id() != simulation_thread_) {
        contract_fail("create_part runs on SimulationThread");
    }
    std::uint32_t index = 0;
    if (!free_list_.empty()) {
        index = free_list_.back();
        free_list_.pop_back();
    } else {
        if (slots_.size() >= kMaxInstances) {
            contract_fail("instance capacity exhausted");
        }
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    }
    Slot& part = slots_[index];
    part.alive = true;
    part.simulated = false;
    part.visual_only = false;
    part.transform = transform_identity();
    part.color = ColorRgb{};
    part.size[0] = part.size[1] = part.size[2] = 1.f;
    part.velocity[0] = part.velocity[1] = part.velocity[2] = 0.f;
    part.parent = 0;
    part.first_child = 0;
    part.next_sibling = 0;
    part.prev_sibling = 0;
    const InstanceId id = (part.generation << 16u) | index;
    note(id, VisualField::Transform | VisualField::Color | VisualField::Size, WriteOrigin::Simulation);
    return id;
}

void DataModel::destroy(InstanceId id) {
    if (threads_running_ && std::this_thread::get_id() != simulation_thread_) {
        if (std::this_thread::get_id() == render_thread_) {
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
    part->alive = false;
    if (part->generation != 0xffffu) {
        ++part->generation;
        free_list_.push_back(index);
    }
    note(id, VisualField::Removed, current_origin());
}

void DataModel::apply_transform(InstanceId id, const Transform& transform, bool force) {
    if (threads_running_) {
        const std::thread::id self = std::this_thread::get_id();
        if (self != simulation_thread_ && self != render_thread_) {
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
    if (!authorize(*part, force)) {
        return;
    }
    if (same_transform(part->transform, transform)) {
        return;
    }
    part->transform = transform;
    const WriteOrigin origin = current_origin();
    note(id, VisualField::Transform, origin);
    emit_change(id, Field::Transform, origin);
}

void DataModel::apply_color(InstanceId id, ColorRgb color, bool force) {
    if (threads_running_) {
        const std::thread::id self = std::this_thread::get_id();
        if (self != simulation_thread_ && self != render_thread_) {
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
    if (!authorize(*part, force)) {
        return;
    }
    if (same_color(part->color, color)) {
        return;
    }
    part->color = color;
    const WriteOrigin origin = current_origin();
    note(id, VisualField::Color, origin);
    emit_change(id, Field::Color, origin);
}

void DataModel::set_transform(InstanceId id, const Transform& transform) {
    apply_transform(id, transform, false);
}

void DataModel::set_transform(InstanceId id, const Transform& transform, ForceSimWrite) {
    apply_transform(id, transform, true);
}

void DataModel::set_color(InstanceId id, ColorRgb color) { apply_color(id, color, false); }

void DataModel::set_color(InstanceId id, ColorRgb color, ForceSimWrite) { apply_color(id, color, true); }

void DataModel::set_size(InstanceId id, float x, float y, float z) {
    if (threads_running_ && std::this_thread::get_id() != simulation_thread_) {
        contract_fail("set_size runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail("size write on a dead instance");
    }
    if (part->size[0] == x && part->size[1] == y && part->size[2] == z) {
        return;
    }
    part->size[0] = x;
    part->size[1] = y;
    part->size[2] = z;
    const WriteOrigin origin = current_origin();
    note(id, VisualField::Size, origin);
    emit_change(id, Field::Size, origin);
}

void DataModel::set_simulated(InstanceId id, bool simulated) {
    if (threads_running_ && std::this_thread::get_id() != simulation_thread_) {
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
    if (threads_running_ && std::this_thread::get_id() != simulation_thread_) {
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

void DataModel::set_linear_velocity(InstanceId id, float x, float y, float z) {
    if (threads_running_ && std::this_thread::get_id() != simulation_thread_) {
        contract_fail("set_linear_velocity runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail("velocity write on a dead instance");
    }
    if (part->velocity[0] == x && part->velocity[1] == y && part->velocity[2] == z) {
        return;
    }
    part->velocity[0] = x;
    part->velocity[1] = y;
    part->velocity[2] = z;
    emit_change(id, Field::LinearVelocity, current_origin());
}

Transform DataModel::transform(InstanceId id) const {
    const Slot* part = slot(id);
    if (part == nullptr) {
        return Transform{};
    }
    return part->transform;
}

ColorRgb DataModel::color(InstanceId id) const {
    const Slot* part = slot(id);
    if (part == nullptr) {
        return ColorRgb{};
    }
    return part->color;
}

bool DataModel::copy_size(InstanceId id, float out[3]) const {
    const Slot* part = slot(id);
    if (part == nullptr) {
        return false;
    }
    out[0] = part->size[0];
    out[1] = part->size[1];
    out[2] = part->size[2];
    return true;
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
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        Slot& part = slots_[index];
        if (!part.alive || !part.simulated || part.visual_only) {
            continue;
        }
        if (part.velocity[0] == 0.f && part.velocity[1] == 0.f && part.velocity[2] == 0.f) {
            continue;
        }
        part.transform.m[12] += part.velocity[0] * step;
        part.transform.m[13] += part.velocity[1] * step;
        part.transform.m[14] += part.velocity[2] * step;
        const InstanceId id = (part.generation << 16u) | index;
        note(id, VisualField::Transform, WriteOrigin::Simulation);
    }
}

DataModel::InstanceSignals* DataModel::bag_for(InstanceId id) {
    if (slot(id) == nullptr) {
        return nullptr;
    }
    const std::uint32_t index = id & kIndexMask;
    if (index >= bags_.size() || !bags_[index] || bags_[index]->owner != id) {
        return nullptr;
    }
    return bags_[index].get();
}

DataModel::InstanceSignals& DataModel::ensure_bag(InstanceId id) {
    if (slot(id) == nullptr) {
        contract_fail("signal on a dead instance");
    }
    const std::uint32_t index = id & kIndexMask;
    if (bags_.size() <= index) {
        bags_.resize(index + 1);
    }
    if (!bags_[index] || bags_[index]->owner != id) {
        if (bags_[index]) {
            events_.destroy_instance(bags_[index]->owner);
        }
        bags_[index] = std::make_unique<InstanceSignals>();
        bags_[index]->owner = id;
    }
    return *bags_[index];
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
        events_.register_signal(signal);
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
        events_.emit(bag->changed.id(), id, field, origin);
    }
    const int index = static_cast<int>(field);
    if (index >= 0 && index < static_cast<int>(Field::Count)) {
        Signal& prop = bag->property[index];
        if (prop.bound() && prop.listeners_ > 0) {
            events_.emit(prop.id(), id, field, origin);
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
    events_.emit(signal.id(), child, Field::Parent, origin);
}

void DataModel::emit_ancestry(InstanceId id, WriteOrigin origin) {
    walk_.clear();
    walk_.push_back(id);
    for (std::size_t i = 0; i < walk_.size(); ++i) {
        const InstanceId cur = walk_[i];
        InstanceSignals* bag = bag_for(cur);
        if (bag != nullptr && bag->ancestry.bound() && bag->ancestry.listeners_ > 0) {
            events_.emit(bag->ancestry.id(), cur, Field::Parent, origin);
        }
        Slot* part = slot(cur);
        if (part == nullptr) {
            continue;
        }
        for (InstanceId child = part->first_child; child != 0;) {
            if (walk_.size() == walk_.capacity()) {
                break;
            }
            walk_.push_back(child);
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
    if (threads_running_ && std::this_thread::get_id() != simulation_thread_) {
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
    events_.destroy_instance(id);
    const std::uint32_t index = id & kIndexMask;
    if (index < bags_.size()) {
        bags_[index].reset();
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
