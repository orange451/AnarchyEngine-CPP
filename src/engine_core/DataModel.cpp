#include "DataModel.hpp"

namespace engine_core {
namespace {

constexpr std::uint32_t kIndexMask = 0xffffu;

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
}

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
    part->transform = transform;
    note(id, VisualField::Transform, current_origin());
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
    part->color = color;
    note(id, VisualField::Color, current_origin());
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
    part->size[0] = x;
    part->size[1] = y;
    part->size[2] = z;
    note(id, VisualField::Size, WriteOrigin::Simulation);
}

void DataModel::set_simulated(InstanceId id, bool simulated) {
    if (threads_running_ && std::this_thread::get_id() != simulation_thread_) {
        contract_fail("set_simulated runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail("set_simulated on a dead instance");
    }
    part->simulated = simulated;
}

void DataModel::set_visual_only(InstanceId id, bool visual_only) {
    if (threads_running_ && std::this_thread::get_id() != simulation_thread_) {
        contract_fail("set_visual_only runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail("set_visual_only on a dead instance");
    }
    part->visual_only = visual_only;
}

void DataModel::set_linear_velocity(InstanceId id, float x, float y, float z) {
    if (threads_running_ && std::this_thread::get_id() != simulation_thread_) {
        contract_fail("set_linear_velocity runs on SimulationThread");
    }
    Slot* part = slot(id);
    if (part == nullptr) {
        contract_fail("velocity write on a dead instance");
    }
    part->velocity[0] = x;
    part->velocity[1] = y;
    part->velocity[2] = z;
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

}  // namespace engine_core
