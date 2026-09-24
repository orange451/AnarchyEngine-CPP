#pragma once

#include "Contract.hpp"
#include "Events.hpp"
#include "InvalidationQueue.hpp"
#include "types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace engine_core {

class DataModelLock;
class GameObject;

// Live source of truth. SimulationThread is the only thread that may run
// gameplay against it, and the only thread that may hold the write lock
// for longer than Prepare's budget.
//
// The root DataModel owns that world. Every other instance shares it.
// GameObject inherits DataModel and adds transform, color, size, and
// velocity. A plain instance does not have those fields.
//
// print(object.transform()) after a PreRender GameObject write shows the new
// value immediately, because this object is live memory. The GPU does not
// read it. Pixels come from VisualSnapshot, filled later in the same Prepare
// for path B, or not at all for a write that lands after the copy.
//
// Path A: SimulationThread phases write here. They dirty the queue and become
//         sim truth. The snapshot sees them at the next Prepare.
// Path B: RenderThread may write a GameObject only inside PreRender, before
//         the copy, and only for visual_only parts (or ForceSimWrite).
// Path C: does not enter this class. See SnapshotPump::override.
// Path D: a render-thread write outside PreRender fails the contract.
// Path E: any other OS thread enqueues a command. Simulation applies it.
class DataModel {
public:
    static constexpr std::size_t kMaxInstances = 16384;
    static constexpr std::size_t kMaxInvalidations = 65536;
    static constexpr std::size_t kMaxCommands = 4096;

    DataModel();
    virtual ~DataModel();

    DataModel(const DataModel&) = delete;
    DataModel& operator=(const DataModel&) = delete;
    DataModel(DataModel&&) noexcept;
    DataModel& operator=(DataModel&&) noexcept;

    // Zero on the root world. A created instance returns its slot id.
    InstanceId id() const { return id_; }

    void set_thread_ids(std::thread::id simulation, std::thread::id render);
    void set_threads_running(bool running);

    // Plain instance in this world. No transform, color, size, or velocity.
    DataModel& create();
    GameObject& create_game_object();
    void destroy(InstanceId id);

    void set_simulated(InstanceId id, bool simulated);
    void set_visual_only(InstanceId id, bool visual_only);
    // Hierarchy. Equal parent is a no-op. Emits Changed, property_changed,
    // ChildRemoved/ChildAdded, and AncestryChanged on this id and descendants.
    void set_parent(InstanceId id, InstanceId parent);

    // Per-instance signals. The reference dies with the instance.
    Signal& changed(InstanceId id);
    Signal& property_changed(InstanceId id, Field field);
    Signal& child_added(InstanceId id);
    Signal& child_removed(InstanceId id);
    Signal& ancestry_changed(InstanceId id);

    EventQueue& events();
    const EventQueue& events() const;
    void attach_scheduler(TaskScheduler* scheduler);

    // Null when the id is dead or the instance is not a GameObject.
    // A dead id does not alias a recycled slot.
    GameObject* game_object(InstanceId id);
    const GameObject* game_object(InstanceId id) const;

    bool alive(InstanceId id) const;
    bool simulated(InstanceId id) const;
    bool visual_only(InstanceId id) const;
    InstanceId parent(InstanceId id) const;

    InvalidationQueue& invalidations();
    bool consume_resync();

    // A rejected write while this thread holds the DataModel lock is stored
    // instead of thrown. Throwing with the mutex locked deadlocks under TSan.
    // Returns true once, then clears the stored rejection.
    bool take_deferred_violation();
    bool has_deferred_violation() const;

    // SimulationThread, under the write lock, at the start of the step.
    // Applies worker commands so they become sim truth before phases run.
    void drain_commands();

    // Moves every simulated, non-visual GameObject by its velocity and dirties Transform.
    void integrate_simulated(double dt);

    // Startup and resync. Visits live GameObjects only.
    template <typename Fn>
    void for_each_game_object(Fn&& fn) const {
        const std::uint32_t count = slot_count();
        for (std::uint32_t index = 0; index < count; ++index) {
            if (const GameObject* object = game_object_at_slot(index)) {
                fn(*object);
            }
        }
    }

    void set_prerender_window(bool open);
    bool prerender_window() const;
    int write_depth() const;

    // Pool construction. Outsiders cannot build a ChildTag or a State.
    class ChildTag {
        friend class DataModel;
        friend class GameObject;
        explicit ChildTag() = default;
    };

    struct State;
    DataModel(ChildTag, State& state, InstanceId id);

private:
    friend class DataModelLock;
    friend class GameObject;

    enum class InstanceKind : std::uint8_t { Plain, GameObject };

    struct Slot {
        std::uint32_t generation = 1;
        bool alive = false;
        bool simulated = false;
        bool visual_only = false;
        InstanceKind kind = InstanceKind::Plain;
        std::uint32_t storage = 0;
        InstanceId parent = 0;
        InstanceId first_child = 0;
        InstanceId next_sibling = 0;
        InstanceId prev_sibling = 0;
    };

    // Stable signal objects for one live generation of a slot.
    struct InstanceSignals {
        InstanceId owner = 0;
        Signal changed;
        Signal property[static_cast<int>(Field::Count)];
        Signal child_added;
        Signal child_removed;
        Signal ancestry;
    };

    struct Command {
        enum class Type { Transform, Color, Destroy } type = Type::Transform;
        InstanceId id = 0;
        Transform transform = transform_identity();
        ColorRgb color{};
    };

    // Root owns the world. Every child instance points at that same State.
    std::unique_ptr<State> owned_;
    State* state_ = nullptr;
    InstanceId id_ = 0;

    bool lock_write_blocking();
    bool lock_write_for(std::chrono::milliseconds budget);
    void unlock_write();

    Slot* slot(InstanceId id);
    const Slot* slot(InstanceId id) const;
    std::uint32_t slot_count() const;
    const GameObject* game_object_at_slot(std::uint32_t index) const;
    InstanceId allocate(InstanceKind kind);
    void rebind(InstanceId id) { id_ = id; }

    bool authorize(const Slot& part, bool force_sim_write);
    bool reject_write(const char* message);
    void note(InstanceId id, VisualField fields, WriteOrigin origin);
    void apply_transform(InstanceId id, const Transform& transform, bool force);
    void apply_color(InstanceId id, ColorRgb color, bool force);
    void enqueue(Command command);
    WriteOrigin current_origin() const;

    InstanceSignals* bag_for(InstanceId id);
    InstanceSignals& ensure_bag(InstanceId id);
    Signal& ensure_signal(InstanceId id, SignalKind kind, Field field);
    void emit_change(InstanceId id, Field field, WriteOrigin origin);
    void emit_child(InstanceId parent, SignalKind kind, InstanceId child, WriteOrigin origin);
    void emit_ancestry(InstanceId id, WriteOrigin origin);
    void detach_links(InstanceId id, Slot& part);
    void unlink_parent(InstanceId id, Slot& part);
    void link_child(InstanceId parent, InstanceId child);
    bool is_under(InstanceId ancestor, InstanceId node) const;
    void release_signals(InstanceId id);
};

}  // namespace engine_core
