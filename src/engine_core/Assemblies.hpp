#pragma once

#include "types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine_core {

class DataModel;
class InstanceRef;

// The PhysicsBase that drives a GameObject when several do: of candidates,
// each of which drives it, the first in a depth-first walk of the tree.
// Assemblies and PhysicsWorld::reconcile both settle claims with it.
// Allocates nothing.
InstanceId claim_winner(const DataModel& game, const InstanceId* candidates, std::size_t count);

// Which PVInstances move as one rigid thing (spec:
// docs/superpowers/specs/2026-10-11-assemblies-design.md, §2 and §3).
//
// A node is a physics node, a winning PhysicsBase in Workspace with the
// GameObject it drives (and any PhysicsBase that lost that GameObject), named
// by its PhysicsBase; or a plain node: a GameObject no body drives, a Camera,
// a PointLight, or a SpotLight in Workspace. Attachments and Bones are never
// nodes: an end that names one resolves to the node of the first PVInstance
// above its Attachments. Active constraints join nodes into assemblies; a node
// no active constraint touches is an assembly of one and is not listed.
//
// One per world, owned by DataModel state. A cache: refresh rebuilds it when
// the world changed, and never writes an instance. Every per-slot array is
// reserved at construction, so a rebuild allocates nothing. Callers hold the
// DataModel lock.
class Assemblies {
public:
    // Per-slot flags, for hooks that must return at once for everything else.
    static constexpr std::uint8_t kLinked = 1u << 0;  // an end, a holder, an Attachment on an end's walk, a
                                                      // member of a multi-node assembly, or a contested GameObject
    static constexpr std::uint8_t kMember = 1u << 1;  // in an assembly of more than one node (never an Attachment)
    static constexpr std::uint8_t kRigid = 1u << 2;   // on either side of an active RigidConstraint

    struct Group {
        InstanceId root = 0;  // the root node: a physics node's PhysicsBase, or a plain node
        bool anchored = false;
        bool has_physics = false;
        std::uint32_t first = 0;  // into group_nodes()
        std::uint32_t count = 0;
    };

    struct RigidEdge {
        InstanceId constraint = 0;
        InstanceId parent_node = 0;
        InstanceId sub_node = 0;
    };

    Assemblies();
    Assemblies(const Assemblies&) = delete;
    Assemblies& operator=(const Assemblies&) = delete;

    // Moves on every change that can change an assembly. Any thread.
    std::uint64_t revision() const { return revision_.load(std::memory_order_acquire); }
    void bump() { revision_.fetch_add(1, std::memory_order_acq_rel); }

    // Rebuilds when revision(), game.world_generation(), or
    // game.simulation_running() differs from the last build: in play, a rigid
    // edge that welds already join is inactive. Writes no instance.
    void refresh(const DataModel& game);

    // From the last build; 0, false, or -1 for an id it did not see.
    std::uint8_t flags(InstanceId id) const;
    // The node an instance belongs to: a physics node is named by its
    // PhysicsBase, a plain node by itself, an Attachment by its holder's node.
    InstanceId node_of(InstanceId id) const;
    bool active(InstanceId constraint) const;
    // The PhysicsBase that drives this GameObject, or 0.
    InstanceId claimant_of(InstanceId game_object) const;
    // AssemblyRoot: the root of id's assembly, or id's own node when it is
    // alone; 0 for anything that is no node and holds none (Terrain, a Dragger,
    // an Attachment with no PVInstance holder) and anything outside Workspace.
    // An instance the last build did not see is worked out live.
    InstanceId root_of(const DataModel& game, InstanceId id) const;

    // A node's frame: its position and rotation, scale removed. A physics
    // node's comes from the GameObject it drives (upright for a
    // PlayerController), never from its PhysicsBase's own Transform, which
    // lags that GameObject while stopped.
    Matrix4 node_frame(const DataModel& game, InstanceId node) const;
    // What the editor writes to place a node: a physics node's GameObject,
    // else its PhysicsBase, else the plain node itself.
    InstanceId mover_of(const DataModel& game, InstanceId node) const;
    // Where a constraint's end (1 or 2) is, with scale removed: its node's
    // frame, or an Attachment's frame worked out from its holder's node frame.
    // False when the end does not resolve.
    bool end_pivot(const DataModel& game, InstanceId constraint, int end, Matrix4& pivot) const;

    // Assemblies of more than one node, and their nodes.
    const std::vector<Group>& groups() const { return groups_; }
    const std::vector<InstanceId>& group_nodes() const { return group_nodes_; }
    int group_of(InstanceId node) const;
    // Active RigidConstraints in follow order: a parent's edge before its subs' edges.
    const std::vector<RigidEdge>& rigid_edges() const { return rigid_; }

    // Suspending (§3): while suspended, every hook returns at once; edits
    // still move the revision. The outermost resume runs one follow pass when
    // follow_on_resume asked for it (paste), else nothing. AssemblySuspend
    // holds one for a scope.
    void suspend() { ++suspended_; }
    void resume(DataModel& game);
    bool suspended() const { return suspended_ > 0; }
    void follow_on_resume() { follow_on_resume_ = true; }

    // Whether a change to id's place in the tree can change an assembly: a
    // linked slot, a Constraint, or a PhysicsBase whose GameObject is linked.
    // Reads the last build's flags.
    bool relevant(const DataModel& game, InstanceId id) const;

    // Hooks. Each returns at once while suspended, while a pass is already
    // running, or for a slot that is not linked. Stopped, a rigid one runs the
    // editor follow (§4); playing, the play hooks (§5).
    void transform_written(DataModel& game, InstanceId id);
    void attachment_changed(DataModel& game, InstanceId id);
    // A constraint's ends or Enabled changed, or a relevant instance moved in
    // the tree: the revision moves and, stopped, one follow pass runs.
    void constraint_changed(DataModel& game, InstanceId id);
    void tree_changed(DataModel& game, InstanceId id);
    // Something may have brought a GUID back (load, undo, redo, Stop, paste,
    // set_guid): dangling ends are looked for again at the next rebuild.
    void guids_may_have_returned();

    // The editor follow (§4). Stopped only. start == 0 runs every active rigid
    // edge in follow order; else start's own incoming edge, then every edge
    // downstream of it. Returns how many movers it wrote.
    int follow(DataModel& game, InstanceId start);

    // For tests: how many times a rebuild searched for a GUID no instance holds.
    std::size_t find_guid_calls() const { return find_guid_calls_; }

private:
    struct Edge {
        InstanceId constraint = 0;
        InstanceId node1 = 0;
        InstanceId node2 = 0;
        bool rigid = false;
        bool live = true;
    };

    void rebuild(const DataModel& game);
    void changed(DataModel& game);
    // An end of constraint (1 or 2), skipping a GUID known missing until one may have come back.
    InstanceId end_of(const DataModel& game, InstanceId constraint, const InstanceRef& ref, int end);
    // The editor follow's helpers (AssemblyFollow.cpp).
    void mark_follow(InstanceId node, std::uint8_t bits);
    std::uint8_t follow_marks(InstanceId node) const;
    bool snap(DataModel& game, const RigidEdge& edge, InstanceId camera);
    void carry(DataModel& game, InstanceId mover, const Matrix4& delta);
    // Play hooks, filled by AssemblyPlay.cpp (Tasks 7 and 8).
    void play_transform_written(DataModel& game, InstanceId id);
    void play_attachment_changed(DataModel& game, InstanceId id);
    void clear_touched();
    // Marks slot as seen for id this build, clearing what it held.
    void touch(InstanceId id);
    bool seen(InstanceId id) const;
    // The node an end resolves to, or 0. mark: flag the walk and holder linked.
    InstanceId resolve_end(const DataModel& game, InstanceId end, bool mark);
    InstanceId resolve_live(const DataModel& game, InstanceId end) const;
    InstanceId claimant_live(const DataModel& game, InstanceId game_object) const;
    std::uint32_t find(std::uint32_t slot);
    void unite(std::uint32_t a, std::uint32_t b);
    bool root_before(const DataModel& game, InstanceId a, InstanceId b) const;
    Matrix4 holder_transform(const DataModel& game, InstanceId holder) const;
    Matrix4 attachment_frame(const DataModel& game, InstanceId attachment, int depth) const;

    std::atomic<std::uint64_t> revision_{0};
    bool built_ = false;
    std::uint64_t built_revision_ = 0;
    std::uint32_t built_generation_ = 0;
    bool built_running_ = false;

    // Per slot, valid while seen_[slot] holds the id the build saw there.
    std::vector<InstanceId> seen_;
    std::vector<InstanceId> node_;
    std::vector<std::int32_t> group_;
    std::vector<std::uint8_t> flags_;
    std::vector<std::uint8_t> active_;
    std::vector<InstanceId> claimant_;
    std::vector<std::uint32_t> uf_;
    std::vector<InstanceId> rigid_parent_;
    std::vector<std::uint8_t> rigid_node_;
    std::vector<std::int32_t> scratch_index_;
    std::vector<std::uint32_t> touched_;

    std::vector<InstanceId> bodies_;
    std::vector<InstanceId> constraints_;
    std::vector<InstanceId> contested_;
    std::vector<InstanceId> candidates_;
    std::vector<Edge> edges_;
    std::vector<std::uint32_t> rigid_order_;
    std::vector<InstanceId> nodes_;
    std::vector<Group> groups_;
    std::vector<InstanceId> group_nodes_;
    std::vector<RigidEdge> rigid_;
    mutable std::vector<InstanceId> live_scratch_;

    int suspended_ = 0;
    bool follow_on_resume_ = false;
    // Above 0 while a hook's pass writes, so its own writes start no pass.
    int busy_ = 0;
    std::uint64_t guid_epoch_ = 0;
    std::size_t find_guid_calls_ = 0;
    // Per constraint slot: which ends were missing, and at which GUID epoch.
    std::vector<InstanceId> dangling_id_;
    std::vector<std::uint8_t> dangling_mask_;
    std::vector<std::uint64_t> dangling_epoch_;
    // The follow's per-node marks, and its walk under a moved mover.
    std::vector<std::uint8_t> follow_mark_;
    std::vector<std::uint32_t> follow_marked_;
    std::vector<InstanceId> carry_stack_;
};

// Holds Assemblies suspended for a scope (§3): load, undo and redo of edit
// waypoints, Stop's restore, paste.
class AssemblySuspend {
public:
    explicit AssemblySuspend(DataModel& game);
    ~AssemblySuspend();
    AssemblySuspend(const AssemblySuspend&) = delete;
    AssemblySuspend& operator=(const AssemblySuspend&) = delete;

private:
    DataModel& game_;
};

}  // namespace engine_core
