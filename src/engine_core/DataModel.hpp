#pragma once

#include "ChangeHistoryService.hpp"
#include "Contract.hpp"
#include "Events.hpp"
#include "UserInputService.hpp"
#include "InvalidationQueue.hpp"
#include "PropertyBag.hpp"
#include "SelectionService.hpp"
#include "types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace engine_core {

class DataModelLock;

// One explorer context action. name is the menu label. primary is the action a
// double-click runs. The shell performs the action. A subclass adds its own.
struct ContextAction {
    const char* name = nullptr;
    bool primary = false;
};

// One instance of the authored tree, for a project save. Children index the
// same vector in sibling order. The root is element 0 and has id 0.
struct AuthoredNode {
    InstanceId id = 0;
    std::string guid;
    std::string class_name;
    std::string name;
    // Filled only when the caller wanted this node. Known fields that differ
    // from the class default, then the extra keys the class does not know.
    bool has_properties = false;
    PropertyBag properties;
    // has_source is always set for a Script or ModuleScript. source is filled
    // only when the node was wanted.
    bool has_source = false;
    std::string source;
    std::vector<std::size_t> children;
};

// Edit-mode authored changes since the last clear. all means every instance.
struct AuthoredDirty {
    std::vector<InstanceId> ids;
    bool all = false;
};

// A GUID is 1 to 64 of [0-9a-z-] and does not start with '-'. Lowercase only,
// so two GUIDs never differ by case on a case-insensitive filesystem.
bool valid_guid(std::string_view guid);
// 16 lowercase hex digits from a seeded 64-bit generator.
std::string make_guid();

class Engine;
class GameObject;
class ScriptAnalysis;
class ScriptHost;

// Live source of truth. SimulationThread is the only thread that may run
// gameplay against it, and the only thread that may hold the write lock
// for longer than Prepare's budget.
//
// The root owns that world, and every other instance shares it. The root is
// a Game (engine_services), the only class that makes a new world.
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
    static constexpr std::size_t kInitialCommands = 4096;

    // parent() returns this when an instance has no parent. 0 is the root.
    static constexpr InstanceId kNoParent = 0xffffffffu;

    virtual ~DataModel();

    DataModel(const DataModel&) = delete;
    DataModel& operator=(const DataModel&) = delete;

    // Zero on the root world. A created instance returns its slot id.
    InstanceId id() const { return id_; }

    // Class identity. The pointer remains valid after the call. A plain
    // instance is DataModel. Game overrides it for the root.
    virtual const char* class_name() const { return "DataModel"; }

    // Cut, Paste, Rename, and Delete; the root has no Delete. A subclass appends
    // its own, or inserts a primary one.
    virtual void context_actions(std::vector<ContextAction>& out) const;

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
    // How many more instances create can make before the world is full.
    std::size_t room_left() const;
    void destroy(InstanceId id);
    // Destroys id and every descendant. destroy alone leaves the children alive
    // and unparented. Children go first, so undo revives each parent before its
    // children. The root is never destroyed.
    void destroy_tree(InstanceId id);

    void set_simulated(InstanceId id, bool simulated);
    void set_visual_only(InstanceId id, bool visual_only);
    // Hierarchy. Parent 0 is this root DataModel. kNoParent clears the parent.
    // The child goes last among the new parent's children, so siblings keep
    // the order they arrived in. Equal parent is a no-op. Emits Changed, property_changed,
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
    // Null until ScriptAnalysis is attached. Setters notify it. They do not analyze.
    void set_script_analysis(ScriptAnalysis* analysis);
    ScriptAnalysis* script_analysis() const;
    std::uint32_t world_generation() const;
    bool simulation_running() const;
    // Edit undo. Play waypoints live on a second stack that stop drops.
    ChangeHistoryService& history();
    const ChangeHistoryService& history() const;
    // What the studio has selected. Shared by every instance in this world.
    SelectionService& selection();
    const SelectionService& selection() const;
    // Keys and the mouse for game:GetService("UserInputService"). The scene view
    // posts to it; the play session's scripts read it.
    UserInputService& input();
    const UserInputService& input() const;

    // Stable authored identity, written to disk and used by references.
    // create assigns one. Empty when id is dead. Id 0 is the root.
    std::string guid(InstanceId id) const;
    // Project load only. The loader checks that GUIDs are unique; this does
    // not scan the world. Throws std::invalid_argument on a malformed GUID.
    // Does not record history.
    void set_guid(InstanceId id, std::string guid);
    // Linear. The live instance holding this GUID, or empty.
    std::optional<InstanceId> find_guid(std::string_view guid) const;
    // Every live GUID and its instance, the root's as 0. Where two share a
    // GUID, the one find_guid would return.
    std::unordered_map<std::string, InstanceId> guid_index() const;

    // Keys the class does not know. A project load fills them; save writes them back.
    // Empty when id is dead.
    const PropertyBag& extra_properties(InstanceId id) const;
    // Marks the instance dirty. Not recorded in undo history.
    void set_extra_property(InstanceId id, std::string key, JsonValue value);
    void erase_extra_property(InstanceId id, std::string_view key);

    // Authored fields other than class, id, Name, Source, and children.
    // A class writes only values that differ from its default.
    virtual void save_properties(PropertyBag& out) const;
    // The value save_properties leaves out, for every key this class owns: what
    // a key missing from its file means. The root owns none.
    virtual void default_properties(PropertyBag& out) const;
    // True when this class owns key. The value was applied, or error is set.
    // Runs on a live instance during project load.
    virtual bool load_property(const std::string& key, const JsonValue& value, std::string& error);

    // Edit mode: the live tree under the root. Play: the place snapshot, so
    // instances created during play are never included. Unparented instances
    // are not in the tree. want(id) false leaves properties and source empty.
    std::vector<AuthoredNode> authored_tree(const std::function<bool(InstanceId)>& want) const;
    // Mutators mark here from the same sites that record history, and only
    // while the simulation is stopped. Stop sets all: the restore may revert
    // edits that came after the last capture.
    AuthoredDirty authored_dirty() const;
    void clear_authored_dirty();
    void mark_authored_dirty(InstanceId id);
    // Bumps on every authored mark and on Stop. Never resets. Safe to read from
    // any thread, so a UI can notice a change without taking the lock.
    std::uint64_t authored_revision() const;
    // Bumps on every change to a name or to the hierarchy, during play too.
    // Moves under the write lock and never resets. A panel that shows the tree
    // reads it again only when this moved. Safe to read from any thread.
    std::uint64_t tree_revision() const;

    // Per-instance signals. The reference dies with the instance.
    // Id 0 is the root DataModel. It has no slot; its signals are not bags[0].
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
    // A new, empty world with this object as its root, named root_name.
    // Only Game makes one.
    explicit DataModel(const char* root_name);

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

    // Successful mutators record here. Equal values return before these run.
    // Velocity is not recorded. Undo application does not record.
    void record_transform(InstanceId id, const Transform& before, const Transform& after);
    void record_color(InstanceId id, ColorRgb before, ColorRgb after);
    void record_size(InstanceId id, float bx, float by, float bz, float ax, float ay, float az);
    void record_bool(InstanceId id, Field field, bool before, bool after);
    void record_string(InstanceId id, Field field, const std::string& before, const std::string& after);
    void record_position(InstanceId id, const Vec3& before, const Vec3& after);

    // Subclass bytes stored in the place snapshot. The base stores nothing.
    // Velocity is not place state; GameObject clears it on read.
    virtual void write_place(std::vector<std::byte>&) const {}
    virtual void read_place(const std::byte*, std::size_t) {}

private:
    friend class DataModelLock;
    friend class Engine;
    friend class GameObject;
    friend class ChangeHistoryService;

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
        // instance as a GameObject, or null. Set with instance, so the physics
        // step does not cast each body on every substep.
        GameObject* body = nullptr;
        InstanceId parent = kNoParent;
        InstanceId first_child = 0;
        // So appending a child does not walk its siblings.
        InstanceId last_child = 0;
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
        std::string guid;
        PropertyBag extras;
        // What a save during play writes: the class and fields at capture.
        std::string class_name;
        PropertyBag properties;
        bool has_source = false;
        std::string source;
        bool simulated = false;
        bool visual_only = false;
        std::vector<std::byte> extra;
    };

    struct PlaceSnapshot {
        std::string root_name;
        std::string root_guid;
        PropertyBag root_extras;
        std::vector<InstanceId> root_children;
        std::vector<PlaceRecord> instances;
    };

    std::unique_ptr<State> owned_;
    State* state_ = nullptr;
    InstanceId id_ = 0;
    std::string name_;
    std::string guid_;
    PropertyBag extras_;

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
    struct InstancePool;
    // Where the next object in pool lives: the storage freed last, else the next unused.
    static std::uint32_t take_storage(InstancePool& pool);
    // The object for id at storage: the one released there, reused, or a new one.
    DataModel* pooled_object(InstancePool& pool, std::uint32_t storage, InstanceId id);
    // Gives a slot's object back to its pool. The slot is no longer alive.
    void release_to_pool(Slot& part);
    void rebind(InstanceId id) { id_ = id; }

    void require_simulation_thread(const char* message) const;
    // SimulationThread, or the paused-edit caller Engine admitted.
    bool gameplay_thread() const;
    void perform_paused_edit(const std::function<void(DataModel&)>& fn);
    bool authorize(const Slot& part, bool force_sim_write);
    bool reject_write(const char* message);
    // Whether this thread holds this world's write lock.
    bool holds_write() const;
    void note(InstanceId id, VisualField fields, WriteOrigin origin);
    // Whether a Transform or Color write from this thread goes to the command queue.
    bool queues_visual_write() const;
    // The GameObject a Transform or Color write lands on, or null after the write
    // is refused. The messages are literals: a deferred violation keeps the pointer.
    GameObject* visual_target(InstanceId id, bool force, const char* dead, const char* not_object);
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
    void link_children(InstanceId parent, const std::vector<InstanceId>& children);
    void clear_hierarchy();
    void rebuild_free_list();
    std::vector<InstanceId> child_ids(InstanceId parent) const;
    void restore_record(const PlaceRecord& record);

    void record_parent(InstanceId id, InstanceId old_parent, InstanceId new_parent, int old_index);
    void record_created(InstanceId id);
    void record_destroyed(AuthoredRecord record);
    AuthoredRecord capture_record(InstanceId id, bool subtree) const;
    const void* type_key_of(InstanceId id) const;
    int sibling_index_of(InstanceId id) const;
    void take_free_index(std::uint32_t index);
    void apply_history(const Mutation& mutation, bool inverse);
    void apply_property(InstanceId id, const PropertyValue& value);
    void apply_parent(InstanceId id, InstanceId parent, int sibling_index);
    void revive_record(const AuthoredRecord& record);
    void revive_tree(const AuthoredRecord& record);
    void reparent_record(const AuthoredRecord& record);
    void place_at_sibling(InstanceId id, int index);
    void apply_record_fields(const AuthoredRecord& record);
    // Scripts look the tree up by name. Tells analysis the tree moved.
    void note_tree_changed();
    PropertyBag merged_properties(const DataModel& object) const;
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
