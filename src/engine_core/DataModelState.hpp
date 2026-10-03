#pragma once

// Internal to the DataModel*.cpp files: the world state behind a DataModel and
// the small helpers they share. Nothing else includes this.

#include "DataModel.hpp"
#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "DraggerWorld.hpp"
#include "Ecs.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "Ring.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"
#include "SelectionService.hpp"
#include "TaskScheduler.hpp"
#include "UserInputService.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
namespace datamodel_detail {

// pool_index_for found no pool for the type. Also the most pools there can be.
constexpr std::uint16_t kNoPool = 0xffffu;

inline int field_index(Field field) {
    const int index = static_cast<int>(field);
    if (index < 0 || index >= static_cast<int>(Field::Count)) {
        contract_fail("unknown field");
    }
    return index;
}

inline GameObject* as_game_object(DataModel* instance) { return dynamic_cast<GameObject*>(instance); }

inline const PropertyBag& empty_bag() {
    static const PropertyBag bag;
    return bag;
}

}  // namespace datamodel_detail

using namespace datamodel_detail;

struct DataModel::InstancePool {
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

struct DataModel::State {
    // Every instance's entity, with its components and tags. First, so it is
    // destroyed last, after everything that might reach it during teardown.
    // Touched under write_mu, like the slots.
    EcsProcessSetup ecs_setup;
    flecs::world ecs;
    EcsIds ecs_ids;
    // Built once with the world; after it, so they are destroyed first.
    // Stepping instances under game: Instance, with Steps and InGame.
    flecs::query<> step_query;
    // Moving bodies: Instance (in), Transform (in-out), Velocity (in), with
    // Simulated and without VisualOnly.
    flecs::query<> physics_query;
    // Rendered GameObjects: Instance (in), with InWorkspace and Transform,
    // which only GameObjects carry.
    flecs::query<> render_query;
    // Rendered GameObjects in Core: the same, with InCore. Core draws as Workspace does.
    flecs::query<> core_render_query;
    // Rigid bodies the physics world simulates: Instance (in), with
    // PhysicsBody and InWorkspace.
    flecs::query<> body_query;
    // Audio sources the audio world plays: Instance (in), with SoundSource
    // and InGame.
    flecs::query<> source_query;
    // Draggers under game: Instance (in), with DraggerTag and InGame.
    flecs::query<> dragger_query;

    // Guards slots, free lists, invalidation, and resync.
    // SimulationThread may hold Write across a whole step and may re-enter
    // (this world's owner and depth below; the mutex is taken once).
    // RenderThread may hold Write only inside Prepare (RenderStepped, PreRender, copy),
    // budget 2ms. PostRender does not hold it.
    // Workers never take it.
    std::timed_mutex write_mu;
    // The thread that holds write_mu, and how many guards it has open. Only that
    // thread changes these, so another reads owner only to learn it is not the
    // holder. The mutex itself is taken only for the outermost guard, so
    // RenderThread's try_lock waits on a normal timed mutex instead of a
    // recursive one.
    std::atomic<std::thread::id> owner{};
    int write_depth = 0;
    // Writes rejected while their thread held this world's lock, kept until the
    // loop that made them takes them. Rare, so a mutex and a list are enough.
    std::mutex deferred_mu;
    std::vector<std::pair<std::thread::id, const char*>> deferred;
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
    // Its signals are hosted by events below. ScriptRuntime binds and releases them.
    UserInputService input;
    // Turns the input's mouse records into Dragger hovers and drags.
    DraggerWorld draggers;
    std::vector<std::unique_ptr<InstancePool>> pools;
    // First child of the root DataModel. 0 means the root has no children.
    InstanceId root_first_child = 0;
    InstanceId root_last_child = 0;
    InvalidationQueue invalidation;

    EventQueue events;
    std::vector<std::unique_ptr<InstanceSignals>> bags;
    // Id 0 is the root and has no slot. bags[0] belongs to the first created
    // instance, whose id is (generation << 16) | 0 and generation starts at 1.
    std::unique_ptr<InstanceSignals> root_signals;
    std::vector<InstanceId> walk;
    // Ids gathered for Heartbeat. Separate from walk, which ancestry mutates.
    std::vector<InstanceId> step_ids;
    // The subtree refresh_scope walks. Its own, since walk belongs to emit_ancestry.
    std::vector<InstanceId> scope_walk;

    // The root object. Children share this State and reach the root through here.
    DataModel* root = nullptr;
    std::uint32_t world_generation = 0;
    bool simulation_running = false;
    bool place_captured = false;
    PlaceSnapshot place;
    // place_slots[index]: a captured instance owns slot index. While the
    // simulation runs, destroying one keeps its slot off the free list, so no
    // new instance, such as one made in Core, takes it before Stop restores it.
    std::vector<bool> place_slots;
    // Dead slots kept off the free list because undo or redo may bring their
    // instance back. A full place takes them, dropping the undo history.
    std::vector<std::uint32_t> history_held;
    // Core's id once found. Core cannot move or be destroyed, so it holds.
    InstanceId core_id = 0;
    // Edit-mode authored changes a project save has not written yet.
    std::unordered_set<InstanceId> dirty;
    bool dirty_all = false;
    std::atomic<std::uint64_t> revision{0};
    std::atomic<std::uint64_t> tree_revision{0};
    std::atomic<std::uint64_t> source_revision{0};

    // UIs watching instances' properties. watched_count, the ids watched in
    // all, lets a change skip the lock when nothing is watched.
    struct ChangeWatcher {
        std::uint64_t watch = 0;
        // Sorted.
        std::vector<InstanceId> ids;
        std::function<void()> notify;
    };
    // DataModel::resources_root. The UI thread and SimulationThread both read it.
    mutable std::mutex resources_mu;
    std::filesystem::path resources_root;
    std::mutex watch_mu;
    std::vector<ChangeWatcher> watchers;
    std::atomic<std::size_t> watched_count{0};
    std::uint64_t next_watch = 1;
    std::function<void()> on_stop;
    std::function<void()> on_start;
    ScriptHost* script_host = nullptr;
    ScriptAnalysis* script_analysis = nullptr;
};

}  // namespace engine_core
