#pragma once

#include "Contract.hpp"
#include "InvalidationQueue.hpp"
#include "types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace engine_core {

class DataModelLock;

// Live source of truth. SimulationThread is the only thread that may run
// gameplay against it, and the only thread that may hold the write lock
// for longer than Prepare's budget.
//
// print(instance.transform) after a PreRender DataModel write shows the new
// value immediately, because this object is live memory. The GPU does not
// read it. Pixels come from VisualSnapshot, filled later in the same Prepare
// for path B, or not at all for a write that lands after the copy.
//
// Path A: SimulationThread phases write here. They dirty the queue and become
//         sim truth. The snapshot sees them at the next Prepare.
// Path B: RenderThread may write here only inside PreRender, before the copy,
//         and only for visual_only parts (or ForceSimWrite).
// Path C: does not enter this class. See SnapshotPump::override.
// Path D: a render-thread write outside PreRender fails the contract.
// Path E: any other OS thread enqueues a command. Simulation applies it.
class DataModel {
public:
    static constexpr std::size_t kMaxInstances = 16384;
    static constexpr std::size_t kMaxInvalidations = 65536;
    static constexpr std::size_t kMaxCommands = 4096;

    DataModel();

    void set_thread_ids(std::thread::id simulation, std::thread::id render);
    void set_threads_running(bool running);

    InstanceId create_part();
    void destroy(InstanceId id);

    void set_transform(InstanceId id, const Transform& transform);
    void set_transform(InstanceId id, const Transform& transform, ForceSimWrite);
    void set_color(InstanceId id, ColorRgb color);
    void set_color(InstanceId id, ColorRgb color, ForceSimWrite);
    void set_size(InstanceId id, float x, float y, float z);
    void set_simulated(InstanceId id, bool simulated);
    void set_visual_only(InstanceId id, bool visual_only);
    void set_linear_velocity(InstanceId id, float x, float y, float z);

    // Live values. A dead id fails closed: alive() is false and transform()
    // is a zero matrix, not a recycled slot.
    Transform transform(InstanceId id) const;
    ColorRgb color(InstanceId id) const;
    bool copy_size(InstanceId id, float out[3]) const;
    bool alive(InstanceId id) const;
    bool simulated(InstanceId id) const;
    bool visual_only(InstanceId id) const;

    InvalidationQueue& invalidations() { return invalidation_; }
    bool consume_resync();

    // A rejected write while this thread holds the DataModel lock is stored
    // instead of thrown. Throwing with the mutex locked deadlocks under TSan.
    // Returns true once, then clears the stored rejection.
    bool take_deferred_violation();
    bool has_deferred_violation() const;

    // SimulationThread, under the write lock, at the start of the step.
    // Applies worker commands so they become sim truth before phases run.
    void drain_commands();

    // Moves every simulated, non-visual body by its velocity and dirties Transform.
    void integrate_simulated(double dt);

    // Startup and resync. Fn is void(InstanceId, const Transform&, const ColorRgb&, const float size[3]).
    template <typename Fn>
    void for_each_live(Fn&& fn) const {
        for (std::uint32_t index = 0; index < slots_.size(); ++index) {
            const Slot& part = slots_[index];
            if (!part.alive) {
                continue;
            }
            const InstanceId id = (part.generation << 16u) | index;
            fn(id, part.transform, part.color, part.size);
        }
    }

    void set_prerender_window(bool open) { prerender_window_ = open; }
    bool prerender_window() const { return prerender_window_; }
    int write_depth() const { return write_depth_; }

private:
    friend class DataModelLock;

    struct Slot {
        std::uint32_t generation = 1;
        bool alive = false;
        bool simulated = false;
        bool visual_only = false;
        Transform transform = transform_identity();
        ColorRgb color{};
        float size[3] = {1.f, 1.f, 1.f};
        float velocity[3] = {};
    };

    struct Command {
        enum class Type { Transform, Color, Destroy } type = Type::Transform;
        InstanceId id = 0;
        Transform transform = transform_identity();
        ColorRgb color{};
    };

    // Guards slots_, free_list_, invalidation_, and resync_.
    // SimulationThread may hold Write across a whole step and may re-enter
    // (thread-local depth; the mutex is taken once).
    // RenderThread may hold Write only inside Prepare (PreRender + copy), budget 2ms.
    // Workers never take it.
    std::timed_mutex write_mu_;
    std::thread::id owner_{};
    int write_depth_ = 0;
    std::thread::id simulation_thread_{};
    std::thread::id render_thread_{};
    bool threads_running_ = false;
    bool prerender_window_ = false;
    bool resync_ = false;

    // Guards commands_ only. Workers push, SimulationThread drains.
    // Hold is one record, never the gameplay step.
    std::mutex command_mu_;
    std::vector<Command> commands_;
    std::size_t command_head_ = 0;
    std::size_t command_tail_ = 0;
    std::size_t command_size_ = 0;

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_list_;
    InvalidationQueue invalidation_;

    bool lock_write_blocking();
    bool lock_write_for(std::chrono::milliseconds budget);
    void unlock_write();

    Slot* slot(InstanceId id);
    const Slot* slot(InstanceId id) const;
    bool authorize(const Slot& part, bool force_sim_write);
    bool reject_write(const char* message);
    void note(InstanceId id, VisualField fields, WriteOrigin origin);
    void apply_transform(InstanceId id, const Transform& transform, bool force);
    void apply_color(InstanceId id, ColorRgb color, bool force);
    void enqueue(Command command);
    WriteOrigin current_origin() const;
};

}  // namespace engine_core
