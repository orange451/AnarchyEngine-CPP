#pragma once

#include "Contract.hpp"
#include "Events.hpp"
#include "InstanceRef.hpp"
#include "InvalidationQueue.hpp"
#include "PropertyBag.hpp"
#include "types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

// flecs' world, declared as flecs.h does. Only engine_core sees the definition.
struct ecs_world_t;

namespace engine_core {

class DataModelLock;
struct EcsIds;

// What the explorer can do to an instance. Each class offers some of them;
// the shell performs them.
enum class InstanceAction { Edit, Cut, Copy, Paste, Duplicate, Rename, Delete };

// The action's name, as its menu item shows it.
const char* action_label(InstanceAction action);

struct ContextAction {
    InstanceAction action = InstanceAction::Rename;
    // Double-clicking the row runs it.
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

// A create when the place already holds DataModel::kMaxInstances instances.
// Nothing was created or changed. A caller says so and carries on.
class InstanceCapacityError : public std::runtime_error {
public:
    InstanceCapacityError();
};

class ChangeHistoryService;
class Engine;
class GameObject;
class PhysicsWorld;
class ScriptAnalysis;
class ScriptHost;
class SelectionService;
class UserInputService;
struct AuthoredRecord;
struct Mutation;
struct LuaSlot;
struct PropertyValue;

// Live source of truth. SimulationThread is the only thread that may run
// gameplay against it, and the only thread that may hold the write lock
// for longer than Prepare's budget.
//
// The root owns that world, and every other instance shares it. The root is
// a Game (engine_services), the only class that makes a new world.
// create<T>() makes any subclass. This class does not list those types.
// GameObject adds a transform, a velocity, and a Prefab. A plain instance
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

    // A service (engine_services): made with the world under game or under
    // another service, as Containment's kServices lists them. None can be
    // moved, renamed, or destroyed.
    virtual bool is_service() const { return false; }
    // Workspace, Lighting, Storage, and Scripts: the services scripts run and
    // render under. A Game makes one of each as its first children.
    virtual bool is_scene_service() const { return false; }
    // A game service, and so everything under it, has no row in the Game Explorer.
    virtual bool hidden_in_explorer() const { return false; }
    // True for an instance that stays under the parent it was first given,
    // as a TerrainMaterial stays in its Terrain. Load, paste, and undo set
    // its parent from none; destroy still takes it out.
    virtual bool parent_locked() const { return false; }
    // The root's child of this scene service class, or 0 when there is none.
    InstanceId scene_service(std::string_view class_name) const;
    // The service of this class, under game or under a service directly under
    // game, or 0 when there is none.
    InstanceId service(std::string_view class_name) const;
    // The Core service, or 0 in a DataModel that is not a Game.
    InstanceId core() const;

    // Why set_parent, set_name, or destroy would refuse, worded for the user,
    // or empty when it would go ahead. Setting the value an instance already
    // has is allowed. The mutators fail the contract on a reason, so a caller
    // that takes input from a script or the user asks first.
    std::optional<std::string> parent_error(InstanceId id, InstanceId new_parent) const;
    std::optional<std::string> rename_error(InstanceId id, std::string_view name) const;
    std::optional<std::string> destroy_error(InstanceId id) const;
    // Why a not-yet-made class_name would be refused under parent, asked before
    // creating it so a refused insert leaves nothing behind and no undo step.
    std::optional<std::string> placement_error_for_class(InstanceId parent, std::string_view class_name) const;
    // The class whose rule decides what goes under parent: parent's own, or that
    // of its first ancestor that is not a Folder. Empty when there is none.
    std::string placement_holder(InstanceId parent) const;

    // Cut, Paste, Rename, and Delete; the root has no Delete. A subclass appends
    // its own, or inserts a primary one.
    virtual void context_actions(std::vector<ContextAction>& out) const;

    // Heartbeat calls this on each instance under the root whose class steps.
    // dt is that phase's step in seconds. The root itself is not stepped.
    virtual void step(double dt) { (void)dt; }
    // True for a class Heartbeat steps. Read once, when its entity is issued.
    virtual bool steps() const { return false; }
    // True for a class the physics world simulates while it is in Workspace
    // (PhysicsObject). Read once, when its entity is issued.
    virtual bool physics_body() const { return false; }
    // True for a class TerrainWorld meshes while it is in Workspace (Terrain).
    // Read once, when its entity is issued.
    virtual bool terrain() const { return false; }
    // True for a class the audio world plays while it is under game
    // (SoundEmitter). Read once, when its entity is issued.
    virtual bool sound_source() const { return false; }
    // True for a class DraggerWorld drives while it is under game (Dragger).
    // Read once, when its entity is issued.
    virtual bool dragger() const { return false; }
    // True for a class runner::GuiLayer draws in the 3D world (BillboardGui).
    // Read once, when its entity is issued.
    virtual bool billboard_gui() const { return false; }

    void set_thread_ids(std::thread::id simulation, std::thread::id render);
    void set_threads_running(bool running);
    // What the two setters above last stored, so a test rig that borrows the
    // engine's thread roles can put them back.
    std::thread::id simulation_thread_id() const;
    std::thread::id render_thread_id() const;
    bool threads_running() const;

    // Plain instance in this world. No transform or velocity.
    DataModel& create();
    // Any subclass. The first create of a type allocates that type's pool.
    // Later creates of the same type do not.
    template <typename T>
    T& create();
    GameObject& create_game_object();
    // How many more instances the place can hold. create throws
    // InstanceCapacityError when it is 0.
    std::size_t room_left() const;
    // Live flecs entities that belong to instances: one per live instance.
    std::size_t entity_count() const;
    void destroy(InstanceId id);
    // Destroys id and every descendant. destroy alone leaves the children alive
    // and unparented. Children go first, so undo revives each parent before its
    // children. The root is never destroyed, and a scene service fails the contract.
    void destroy_tree(InstanceId id);

    void set_simulated(InstanceId id, bool simulated);
    void set_visual_only(InstanceId id, bool visual_only);
    // Hierarchy. Parent 0 is this root DataModel. kNoParent clears the parent.
    // The child goes last among the new parent's children, so siblings keep
    // the order they arrived in. Equal parent is a no-op. Emits Changed, property_changed,
    // ChildRemoved/ChildAdded, and AncestryChanged on this id and descendants.
    // A move parent_error refuses fails the contract.
    void set_parent(InstanceId id, InstanceId parent);

    // Path A. Default is class_name(). Siblings may share a name.
    // Equal values do not emit. Name does not dirty the visual snapshot.
    void set_name(InstanceId id, std::string name);
    // Empty when id is dead. Id 0 is the root DataModel.
    std::string name(InstanceId id) const;
    // First direct child in sibling order, or 0 when none matches.
    InstanceId find_first_child(InstanceId parent, std::string_view name) const;
    // Whether a save writes this instance and its subtree. Not saved itself,
    // not undone, and true for a new instance. The place capture keeps a
    // non-archivable instance, so Stop restores it like any other.
    bool archivable(InstanceId id) const;
    void set_archivable(InstanceId id, bool archivable);
    // Direct children in sibling order. A missing parent returns an empty vector.
    std::vector<InstanceId> get_children(InstanceId parent) const;

    // Place is the authored tree. The first start_simulation captures it when
    // nothing has been captured yet. capture_place replaces that tree.
    // start while running is an error. stop while stopped does nothing.
    // stop runs on SimulationThread. The stop hook runs first, while the play
    // tree is still alive. Then: drop queued events, disconnect every
    // connection, cancel session jobs, restore the place, bump world_generation.
    // The start hook runs after simulation_running is set, still under the write lock.
    // Script analysis hears of both after simulation_running changes: it checks
    // only the authored tree.
    void capture_place();
    void start_simulation();
    void stop_simulation();
    void set_stop_hook(std::function<void()> hook);
    void set_start_hook(std::function<void()> hook);
    void set_script_host(ScriptHost* host);
    // Null until ScriptAnalysis is attached. Setters notify it. They do not analyze.
    void set_script_analysis(ScriptAnalysis* analysis);
    ScriptAnalysis* script_analysis() const;
    // The physics world scripts' queries (Workspace:Raycast) use. The Engine
    // sets it; null without one.
    void set_physics(PhysicsWorld* physics);
    PhysicsWorld* physics() const;
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

    // The open project's resources folder, where Mesh, Texture, and Sound Paths
    // point and where a Mesh's shapes are written. Project sets it when it opens
    // or saves a folder, and reset_place clears it. Empty with no project. Any thread.
    std::filesystem::path resources_root() const;
    void set_resources_root(std::filesystem::path root);
    // Where engine instances report problems a user should see, such as a
    // Terrain whose voxel file is missing. Engine sends them to the Output
    // window; warn does nothing without a sink. Set the sink before the world
    // runs; warn is called on SimulationThread.
    void set_warning_sink(std::function<void(const std::string&)> sink);
    void warn(const std::string& text) const;

    // Stable authored identity, written to disk and used by references.
    // create assigns one. Empty when id is dead. Id 0 is the root.
    std::string guid(InstanceId id) const;
    // id is live, or the root, and its GUID is guid. Unlike guid(), copies nothing.
    bool has_guid(InstanceId id, std::string_view guid) const;
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
    // Marks the instance and the place dirty. Not an undo step.
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
    // Project save calls this on every live authored instance once nothing on
    // disk stops the save, with the project's resources folder. An instance
    // writes its own files there, such as a Terrain's .avox. A returned reason
    // fails the save. SimulationThread.
    virtual std::optional<std::string> save_resources(const std::filesystem::path& root);

    // Edit mode: the live tree under the root. Play: the place snapshot, so
    // instances created during play are never included. Unparented instances
    // are not in the tree. want(id) false leaves properties and source empty.
    std::vector<AuthoredNode> authored_tree(const std::function<bool(InstanceId)>& want) const;
    // While a place is captured, the bytes id wrote at its capture: what Stop
    // will restore. Null when id was not captured. id need not be live now.
    const std::vector<std::byte>* captured_place_bytes(InstanceId id) const;
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
    // Bumps on every write to a script's Source through set_source, during
    // play too. Stop moves authored_revision instead. Safe to read from any thread.
    std::uint64_t source_revision() const;

    // A UI's interest in the properties of some instances. notify runs on the
    // thread that made the change, whenever a property of a watched instance
    // changes, one is destroyed, or a write touches many at once: undo, redo,
    // and Stop. It must be quick, safe on any thread, and must not call these
    // three back; a panel sets a flag there and redraws on its own thread.
    // Any thread.
    // A watch with no instances costs a change nothing.
    std::uint64_t watch_changes(std::function<void()> notify);
    void set_watched(std::uint64_t watch, std::vector<InstanceId> ids);
    void unwatch_changes(std::uint64_t watch);

    // Per-instance signals. The reference dies with the instance.
    // Id 0 is the root DataModel. It has no slot; its signals are not bags[0].
    Signal& changed(InstanceId id);
    Signal& property_changed(InstanceId id, Field field);
    Signal& child_added(InstanceId id);
    Signal& child_removed(InstanceId id);
    Signal& ancestry_changed(InstanceId id);
    // An event the instance's class declares with lua_event, such as a Button's
    // Action, made on first use. name is the field's name.
    Signal& event_signal(InstanceId id, std::string_view name);
    // Fires that event for whatever is connected to it; nothing when nothing
    // is. Its handlers get the instance, Field::Reflected, and args through
    // EventQueue::current_args. args must match the event's declared arguments
    // (lua_event), or the contract fails, whether or not anything listens. An
    // event the class does not declare does nothing. SimulationThread.
    void fire_event(InstanceId id, std::string_view name, EventArgs args);
    // An event declared without arguments.
    void fire_event(InstanceId id, std::string_view name);

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
    // Scope. in_game: under game. in_workspace: under the Workspace service,
    // which is not inside itself. in_lighting and in_core: under the Lighting
    // and Core services, the same way. All are false for a dead id. Kept
    // current at every tree change, so reading them costs no walk.
    bool in_game(InstanceId id) const;
    bool in_workspace(InstanceId id) const;
    bool in_lighting(InstanceId id) const;
    bool in_core(InstanceId id) const;
    // Core or anything under it: what is never saved, never undone, and left
    // alone by New, Open, Play, and Stop. False for 0 and for a dead id.
    bool core_holds(InstanceId id) const;
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
    // Returns true once, then clears the stored rejection. reason, when given,
    // receives its message (a literal) on true.
    bool take_deferred_violation(const char** reason = nullptr);
    bool has_deferred_violation() const;

    // SimulationThread, under the write lock, at the start of the step.
    // Applies worker commands so they become sim truth before phases run.
    void drain_commands();

    // Moves every simulated, non-visual GameObject by its velocity and dirties Transform.
    void integrate_simulated(double dt);
    // The physics bodies in Workspace (physics_body()), in no set order, into
    // out, which is cleared first.
    void physics_bodies(std::vector<InstanceId>& out) const;
    // The Terrains in Workspace (terrain()), in no set order, into out, which
    // is cleared first.
    void terrains(std::vector<InstanceId>& out) const;
    // The audio sources under game (sound_source()), in no set order, into
    // out, which is cleared first.
    void sound_sources(std::vector<InstanceId>& out) const;
    // The Draggers under game (dragger()), in no set order, into out, which
    // is cleared first.
    void draggers(std::vector<InstanceId>& out) const;
    // The BillboardGuis under game (billboard_gui()), in no set order, into
    // out, which is cleared first. Whether each is drawn is BillboardGui::drawn.
    void billboards(std::vector<InstanceId>& out) const;
    // A GameObject's Transform as the physics world moved it: stored and
    // drawn, as integrate_simulated moves one, with no Changed, no history,
    // and no check. SimulationThread. A dead id or a non-GameObject does nothing.
    void write_simulated_transform(InstanceId id, const Matrix4& transform);
    // Heartbeat. Calls step(dt) on every stepping instance under the root, in
    // no set order. The ids are gathered first, so a step may create, destroy,
    // or reparent; an instance destroyed before its turn is skipped.
    void step_instances(double dt);
    // How many instances step_instances would step now.
    std::size_t stepper_count() const;

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

    // The GameObjects the render snapshot holds: live and under Workspace.
    // A query, not a scan of every slot. The snapshot's resync uses it.
    void for_each_rendered(const std::function<void(const GameObject&)>& fn) const;

    void set_prerender_window(bool open);
    bool prerender_window() const;
    // Set only by the render thread, while it holds the write lock, around Lua
    // execution inside the window (ScriptRuntime::render_step). While set,
    // authorize's render-thread branch admits a write as a sim write would.
    void set_window_script(bool active);
    bool window_script() const;
    int write_depth() const;
    // Whether this thread may mutate the world now: SimulationThread, a paused
    // edit, the caller when the engine threads are not running, or a script
    // running in the render window (see mutation_thread). Every setter's
    // SimulationThread guard asks this.
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

    // True for a class that is not a GameObject but still has a render
    // snapshot row while it is in Workspace, as DirectionalLight does. Moving
    // it into or out of Workspace then tells the snapshot, as it does for a
    // GameObject.
    virtual bool has_visual_row() const { return false; }
    // Tells the render snapshot that fields of this instance's row changed.
    void note_visual_row(VisualField fields);

    // A write that changes what a save writes but is never an undo step: it
    // marks the save set and the place dirty. Not during play, not in Core,
    // and the place only while history is on.
    void note_unrecorded_edit(InstanceId id);
    // When the open recording created id, its record takes id's place bytes
    // as they are now, so redo brings back what a write that records nothing
    // set while making it (a pasted Terrain's voxels and DataPath).
    void refresh_created_record(InstanceId id);
    // The place bytes of the record refresh_created_record would refresh;
    // null when the open recording did not create id.
    const std::vector<std::byte>* open_created_place(InstanceId id) const;

    // Successful mutators record here. Equal values return before these run.
    // Velocity is not recorded. Undo application does not record.
    void record_transform(InstanceId id, const Matrix4& before, const Matrix4& after);
    void record_bool(InstanceId id, Field field, bool before, bool after);
    void record_string(InstanceId id, Field field, const std::string& before, const std::string& after);
    // A registry property of this instance (lua_saved_property) changed from
    // before to after, as its read gives them. Its setter calls this once the
    // value is stored. Records undo, which puts values back through the
    // property's write, and fires Changed with the property's name.
    void note_property_change(std::string_view property, const LuaSlot& before, const LuaSlot& after);
    // A registry property of this instance that is not an edit changed: fires
    // Changed with its name, and tells the panels that show it, but records
    // nothing and leaves the place unchanged.
    void emit_property(std::string_view property);

    // The GUID in text; the live target, if any, in id, with kind Instance, else
    // Nil. A target whose class does not inherit klass, as a hand-edited file
    // can name, reads Nil too.
    LuaSlot instance_reference_slot(const InstanceRef& ref, const char* klass) const;
    // Shared logic for a saved reference property held by an InstanceRef, as
    // Material's DiffuseTexture and GameObject's Prefab both use. nil clears; a
    // live instance whose class inherits klass is stored by GUID; one of
    // another class is refused as "<property> must be a <klass>"; a slot
    // naming a GUID (a load, Stop, or undo) is stored as it is. Calls
    // note_property_change and returns nullopt on success.
    std::optional<std::string> set_instance_reference(std::string_view property, const char* klass, InstanceRef& ref,
                                                       const LuaSlot& value);

    // Subclass bytes stored in the place snapshot. The base stores the class's
    // saved registry properties, so Stop puts them back; a class that has none
    // stores nothing. A subclass that overrides these and also has saved
    // registry properties calls the base. Velocity is not place state;
    // GameObject clears it on read.
    virtual void write_place(std::vector<std::byte>& out) const;
    virtual void read_place(const std::byte* data, std::size_t size);

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
        std::uint16_t pool = 0;
        std::uint32_t storage = 0;
        DataModel* instance = nullptr;
        // instance as a GameObject, or null. Set with instance, so the physics
        // step does not cast each body on every substep.
        GameObject* body = nullptr;
        // This instance's flecs entity: issued with the slot, deleted when it
        // is released. 0 while the slot is free.
        std::uint64_t entity = 0;
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
        // The lua_event signals made so far, by name.
        struct Event {
            std::string name;
            Signal signal;
        };
        std::vector<std::unique_ptr<Event>> events;
    };

    struct Command {
        enum class Type { Transform, Destroy } type = Type::Transform;
        InstanceId id = 0;
        Matrix4 transform = matrix4_identity();
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
        bool archivable = true;
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
    bool archivable_ = true;
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
    // Gives a slot's object back to its pool and deletes its entity. The slot
    // is no longer alive.
    void release_to_pool(Slot& part);
    // Creates part's entity for id. Runs before the object is constructed or
    // reused, so construction can write its components.
    void issue_entity(Slot& part, InstanceId id);
    // This world's flecs state (Ecs.hpp). engine_core and GameObject only.
    ecs_world_t* ecs_world() const;
    const EcsIds& component_ids() const;
    // id's entity, or 0 when id is dead.
    std::uint64_t entity_of(InstanceId id) const;
    // Recomputes id's scope tags from its parent. Unchanged tags return at
    // once; changed tags walk the subtree. Runs after every tree change.
    void refresh_scope(InstanceId id);
    void apply_scope(InstanceId id, bool in_game_now, bool in_workspace_now, bool in_lighting_now, bool in_core_now);
    void rebind(InstanceId id) { id_ = id; }

    void require_simulation_thread(const char* message) const;
    // SimulationThread, or the paused-edit caller Engine admitted. Place
    // capture, start, and stop ask only this: they swap the whole tree and
    // close the play VM, which a window handler must not do from inside it.
    bool gameplay_thread() const;
    // gameplay_thread, or the render thread while a script runs in the window
    // (prerender_window and window_script both set). The write lock serializes
    // a window script's mutation as it does a sim step's, so the instance
    // operations a script reaches (create, destroy, Parent, Name, every
    // property setter) guard on this.
    bool mutation_thread() const;
    void perform_paused_edit(const std::function<void(DataModel&)>& fn);
    bool authorize(const Slot& part, bool force_sim_write);
    bool reject_write(const char* message);
    // Whether this thread holds this world's write lock.
    bool holds_write() const;
    void note(InstanceId id, VisualField fields, WriteOrigin origin);
    // Whether a Transform write from this thread goes to the command queue.
    bool queues_visual_write() const;
    // The GameObject a Transform write lands on, or null after the write
    // is refused. The messages are literals: a deferred violation keeps the pointer.
    GameObject* visual_target(InstanceId id, bool force, const char* dead, const char* not_object);
    void apply_transform(InstanceId id, const Matrix4& transform, bool force);
    void enqueue(Command command);
    WriteOrigin current_origin() const;

    InstanceSignals* bag_for(InstanceId id);
    InstanceSignals& ensure_bag(InstanceId id);
    Signal& ensure_signal(InstanceId id, SignalKind kind, Field field);
    // payload is for Field::Reflected: the property's lua_property_id.
    void emit_change(InstanceId id, Field field, WriteOrigin origin, std::uint64_t payload = 0);
    // Tells the watchers of id, or every watcher, that what they show changed.
    void notify_watchers(InstanceId id);
    void notify_all_watchers();
    void emit_child(InstanceId parent, SignalKind kind, InstanceId child, WriteOrigin origin);
    void emit_ancestry(InstanceId id, WriteOrigin origin);
    void detach_links(InstanceId id, Slot& part);
    void unlink_parent(InstanceId id, Slot& part);
    void link_child(InstanceId parent, InstanceId child);
    bool is_under(InstanceId ancestor, InstanceId node) const;
    // The class whose rule decides what goes in parent: its own, or for a
    // Folder, that of the first ancestor that is not a Folder, walking as
    // though moved were already under moved_to. Empty when the walk leaves the tree.
    std::string rule_class(InstanceId parent, InstanceId moved, InstanceId moved_to) const;
    // The first placement rule that id and its descendants would break under new_parent.
    std::optional<std::string> placement_error_for(InstanceId id, InstanceId new_parent) const;
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
    // Moves for undo or redo when parent_error allows it. A write outside any
    // recording may have destroyed the parent, put it under the instance, or
    // made it refuse the instance; then the instance stays where it is, which
    // for one just revived is unparented. True when it is under parent after.
    bool history_move(InstanceId id, InstanceId parent);
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
