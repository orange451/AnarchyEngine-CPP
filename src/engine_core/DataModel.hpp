#pragma once

#include "Contract.hpp"
#include "Events.hpp"
#include "InvalidationQueue.hpp"
#include "types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace engine_core {

class DataModelLock;
class Engine;
class GameObject;
class ScriptHost;

// Live source of truth. SimulationThread is the only thread that may run
// gameplay against it, and the only thread that may hold the write lock
// for longer than Prepare's budget.
//
// The root DataModel owns that world. Every other instance shares it.
// create<T>() makes any subclass. This class does not list those types.
// GameObject adds transform, color, size, and velocity. A plain instance
// does not have those fields. Every instance has a Name. The place snapshot
// is the authored tree; stop_simulation restores it.
//
// print(object.transform()) after a PreRender GameObject write shows the new
// value immediately, because this object is live memory. The GPU does not
// read it. Pixels come from VisualSnapshot, filled later in the same Prepare
// for path B, or not at all for a write that lands after the copy.
//
// Path A: SimulationThread phases write here. They dirty the queue and become
//         sim truth. The snapshot sees them at the next Prepare.
// Path B: RenderThread may write a GameObject only inside RenderStepped or
//         PreRender, before the copy, and only for visual_only parts
//         (or ForceSimWrite).
// Path C: does not enter this class. See SnapshotPump::override.
// Path D: a render-thread write outside that window fails the contract.
//         Perform, Present, and PostRender are outside it.
// Path E: any other OS thread enqueues a command. Simulation applies it.
class DataModel {
public:
    static constexpr std::size_t kMaxInstances = 16384;
    static constexpr std::size_t kMaxInvalidations = 65536;
    static constexpr std::size_t kMaxCommands = 4096;

    // parent() returns this when an instance has no parent. 0 is the root.
    static constexpr InstanceId kNoParent = 0xffffffffu;

    DataModel();
    virtual ~DataModel();

    DataModel(const DataModel&) = delete;
    DataModel& operator=(const DataModel&) = delete;
    DataModel(DataModel&&) noexcept;
    DataModel& operator=(DataModel&&) noexcept;

    // Zero on the root world. A created instance returns its slot id.
    InstanceId id() const { return id_; }

    // Stable label for tools such as the explorer. Subclasses return their own
    // name. The pointer remains valid after the call.
    virtual const char* class_name() const { return "DataModel"; }

    // Heartbeat calls this on every descendant of the root. dt is that phase's
    // step in seconds. The root itself is not stepped.
    virtual void step(double dt) { (void)dt; }

    void set_thread_ids(std::thread::id simulation, std::thread::id render);
    void set_threads_running(bool running);

    // Plain instance in this world. No transform, color, size, or velocity.
    DataModel& create();
    // Any subclass. The first create of a type allocates that type's pool.
    // Later creates of the same type do not.
    template <typename T>
    T& create();
    GameObject& create_game_object();
    void destroy(InstanceId id);

    void set_simulated(InstanceId id, bool simulated);
    void set_visual_only(InstanceId id, bool visual_only);
    // Hierarchy. Parent 0 is this root DataModel. kNoParent clears the parent.
    // Equal parent is a no-op. Emits Changed, property_changed,
    // ChildRemoved/ChildAdded, and AncestryChanged on this id and descendants.
    void set_parent(InstanceId id, InstanceId parent);

    // Path A. Default is class_name(). Siblings may share a name.
    // Equal values do not emit. Name does not dirty the visual snapshot.
    void set_name(InstanceId id, std::string name);
    // Empty when id is dead. Id 0 is the root DataModel.
    std::string name(InstanceId id) const;
    // First direct child in sibling order, or 0 when none matches.
    InstanceId find_first_child(InstanceId parent, std::string_view name) const;
    // Direct children in sibling order. A missing parent returns an empty vector.
    std::vector<InstanceId> get_children(InstanceId parent) const;

    // Place is the authored tree. The first start_simulation captures it when
    // nothing has been captured yet. capture_place replaces that tree.
    // start while running is an error. stop while stopped does nothing.
    // stop runs on SimulationThread. The stop hook runs first, while the play
    // tree is still alive. Then: drop queued events, disconnect every
    // connection, cancel session jobs, restore the place, bump world_generation.
    // The start hook runs after simulation_running is set, still under the write lock.
    void capture_place();
    void start_simulation();
    void stop_simulation();
    void set_stop_hook(std::function<void()> hook);
    void set_start_hook(std::function<void()> hook);
    void set_script_host(ScriptHost* host);
    std::uint32_t world_generation() const;
    bool simulation_running() const;

    // Per-instance signals. The reference dies with the instance.
    Signal& changed(InstanceId id);
    Signal& property_changed(InstanceId id, Field field);
    Signal& child_added(InstanceId id);
    Signal& child_removed(InstanceId id);
    Signal& ancestry_changed(InstanceId id);

    EventQueue& events();
    const EventQueue& events() const;
    void attach_scheduler(TaskScheduler* scheduler);

    // Null when the id is dead. A dead id does not alias a recycled slot.
    DataModel* instance(InstanceId id);
    const DataModel* instance(InstanceId id) const;

    // Null when the id is dead or the instance is not a GameObject.
    GameObject* game_object(InstanceId id);
    const GameObject* game_object(InstanceId id) const;

    bool alive(InstanceId id) const;
    bool simulated(InstanceId id) const;
    bool visual_only(InstanceId id) const;
    // kNoParent when id is dead or the instance has no parent.
    // 0 when it is a child of the root DataModel.
    InstanceId parent(InstanceId id) const;
    // Pass 0 to walk the root's children. A missing parent returns 0.
    InstanceId first_child(InstanceId parent) const;
    InstanceId next_sibling(InstanceId id) const;

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
    // Heartbeat. Calls step(dt) on every descendant of the root.
    void step_descendants(double dt);

    // Startup and resync. Visits live GameObjects only.
    void for_each_instance(const std::function<void(DataModel&)>& fn);

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
    // SimulationThread, or the caller when the engine threads are not running.
    bool on_gameplay_thread() const;

    // Pool construction. Outsiders cannot build a ChildTag or a State.
    class ChildTag {
        friend class DataModel;
        friend class GameObject;
        explicit ChildTag() = default;
    };

    struct State;
    DataModel(ChildTag, State& state, InstanceId id);

protected:
    // destroy() keeps the C++ object so a stale reference can fail closed.
    // on_release runs then. on_reuse runs when that storage is issued again.
    virtual void on_release() {}
    virtual void on_reuse() {}

    // Called at the end of a successful set_parent, after the signals are emitted.
    // Subclasses enqueue work from here. They do not resume scripts.
    virtual void on_parent_changed(InstanceId previous, InstanceId next) {
        (void)previous;
        (void)next;
    }

    void emit_own(Field field);
    ScriptHost* script_host() const;

    // Subclass bytes stored in the place snapshot. The base stores nothing.
    // Velocity is not place state; GameObject clears it on read.
    virtual void write_place(std::vector<std::byte>&) const {}
    virtual void read_place(const std::byte*, std::size_t) {}

private:
    friend class DataModelLock;
    friend class Engine;
    friend class GameObject;

    struct SpawnOps {
        const void* key = nullptr;
        std::size_t bytes = 0;
        std::size_t align = 0;
        DataModel* (*construct)(void* memory, ChildTag tag, State& state, InstanceId id) = nullptr;
        void (*destroy)(DataModel* object) = nullptr;
    };

    struct Slot {
        std::uint32_t generation = 1;
        bool alive = false;
        bool simulated = false;
        bool visual_only = false;
        std::uint16_t pool = 0;
        std::uint32_t storage = 0;
        DataModel* instance = nullptr;
        InstanceId parent = kNoParent;
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
    // One captured instance. Ids are the live ids at capture time.
    struct PlaceRecord {
        InstanceId id = 0;
        const void* type_key = nullptr;
        InstanceId parent = kNoParent;
        std::vector<InstanceId> children;
        std::string name;
        bool simulated = false;
        bool visual_only = false;
        std::vector<std::byte> extra;
    };

    struct PlaceSnapshot {
        std::string root_name;
        std::vector<InstanceId> root_children;
        std::vector<PlaceRecord> instances;
    };

    std::unique_ptr<State> owned_;
    State* state_ = nullptr;
    InstanceId id_ = 0;
    std::string name_;

    bool lock_write_blocking();
    bool lock_write_for(std::chrono::milliseconds budget);
    void unlock_write();

    Slot* slot(InstanceId id);
    const Slot* slot(InstanceId id) const;
    std::uint32_t slot_count() const;
    const GameObject* game_object_at_slot(std::uint32_t index) const;
    InstanceId allocate();
    template <typename T>
    static SpawnOps ops_for();
    DataModel& spawn(const SpawnOps& ops);
    void rebind(InstanceId id) { id_ = id; }

    void require_simulation_thread(const char* message) const;
    // SimulationThread, or the paused-edit caller Engine admitted.
    bool gameplay_thread() const;
    void perform_paused_edit(const std::function<void(DataModel&)>& fn);
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

    void capture_place_unlocked();
    void restore_place_unlocked();
    void retire_slot(std::uint32_t index, bool bump_generation);
    std::uint16_t pool_index_for(const void* type_key) const;
    void adopt_slot(std::uint16_t pool_index, InstanceId id);
    void link_children_front(InstanceId parent, const std::vector<InstanceId>& children);
    void clear_hierarchy();
    void rebuild_free_list();
    std::vector<InstanceId> child_ids(InstanceId parent) const;
    void restore_record(const PlaceRecord& record);
};

template <typename T>
DataModel::SpawnOps DataModel::ops_for() {
    static const char key = 0;
    SpawnOps ops;
    ops.key = &key;
    ops.bytes = sizeof(T);
    ops.align = alignof(T);
    ops.construct = [](void* memory, ChildTag tag, State& state, InstanceId id) -> DataModel* {
        return ::new (memory) T(tag, state, id);
    };
    ops.destroy = [](DataModel* object) { static_cast<T*>(object)->~T(); };
    return ops;
}

template <typename T>
T& DataModel::create() {
    static_assert(std::is_base_of<DataModel, T>::value, "create<T>() requires a DataModel subclass");
    return static_cast<T&>(spawn(ops_for<T>()));
}

}  // namespace engine_core
