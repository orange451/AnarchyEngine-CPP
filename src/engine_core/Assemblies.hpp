#pragma once

#include "types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
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
    static constexpr std::uint8_t kHeld = 1u << 3;    // held by a hand (a bone edge's sub), or welded to what is:
                                                      // in play its body is kinematic, moved to the hand each step

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
        // Which end (1 or 2) names the parent: the end on the root's side.
        int parent_end = 1;
    };
    // An active JointConstraint (joint spec §2): node1 is PVInstance1's node
    // (the base), node2 PVInstance2's. It joins nothing.
    struct JointEdge {
        InstanceId constraint = 0;
        InstanceId node1 = 0;
        InstanceId node2 = 0;
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
    // Active RigidConstraints with a Bone end that poses a bone, or an
    // Attachment inside one: a hand that holds something. The Bone's end is
    // always the parent, and the two sides
    // stay apart, each its own assembly, since the bone moves under its
    // GameObject as the pose changes. Not in rigid_edges().
    const std::vector<RigidEdge>& bone_edges() const { return bone_; }
    // Active NoCollisionConstraints: the two physics nodes (PhysicsBase ids)
    // each keeps from colliding, lower id first, sorted, no repeats. They join
    // nothing, so neither is in edges or groups for it.
    const std::vector<std::pair<InstanceId, InstanceId>>& no_collide_pairs() const { return no_collide_; }
    // Active JointConstraints, by constraint id: two different assemblies,
    // each with a body, not both anchored, and no PlayerController at either
    // end or root. They join nothing.
    const std::vector<JointEdge>& joints() const { return joints_; }

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
    // A skinned GameObject's pose changed by other than its Bones' Offsets
    // (an Animator stepped by a script): stopped, what its hands hold
    // follows. Playing, place_held and the physics step see to it.
    void pose_changed(DataModel& game, InstanceId game_object);
    // Playing: a member's Velocity (has_linear) or AngularVelocity
    // (has_angular) was written. It is the assembly's: stored on every
    // physics member at once, and given to the body at the next sync.
    void velocity_written(DataModel& game, InstanceId id, bool has_linear, Vec3 linear, bool has_angular,
                          Vec3 angular);
    // A constraint's ends or Enabled changed, or a relevant instance moved in
    // the tree: the revision moves and, stopped, one follow pass runs.
    void constraint_changed(DataModel& game, InstanceId id);
    void tree_changed(DataModel& game, InstanceId id);
    // Something may have brought a GUID back (load, undo, redo, Stop, paste,
    // set_guid): dangling ends are looked for again at the next rebuild.
    void guids_may_have_returned();

    // The editor follow (§4). Stopped only. start == 0 runs every active rigid
    // edge in follow order; else start's own incoming edge, then every edge
    // downstream of it. A snapped sub brings its side: what it reaches through
    // other active constraints, as in play, unless that reaches its parent.
    // alone: start was moved by itself (a write to its own Transform), so if it
    // is a sub it goes back by itself, and what is welded to it stays. Returns
    // how many snaps wrote.
    int follow(DataModel& game, InstanceId start, bool alone = false);

    // For tests: how many times a rebuild searched for a GUID no instance holds.
    std::size_t find_guid_calls() const { return find_guid_calls_; }

    // Play (§5). When play starts, each assembly of more than one node gets a
    // body frame (its root's frame then) and each member an offset from it;
    // the physics world builds one Box3D body per assembly with physics from
    // these, and writes every member back from that body. SimulationThread.
    struct PlayMember {
        InstanceId node = 0;
        // A physics node's GameObject, written with it; else 0.
        InstanceId driven = 0;
        // frame⁻¹ × the node's frame when captured.
        Matrix4 offset{};
        // Moves when offset changes in place (an end Attachment slid, a Brush
        // recentred), so the physics world places only this member's shapes again.
        std::uint32_t revision = 0;
    };
    struct PlayAssembly {
        InstanceId root = 0;
        bool anchored = false;
        bool has_physics = false;
        // The body frame offsets are measured from; also where a member move puts the body.
        Matrix4 frame{};
        std::uint32_t first = 0;  // into play_members()
        std::uint32_t count = 0;
        // Moves when members or offsets change, so the physics world places shapes again.
        std::uint32_t stamp = 0;
        // A member was written: the physics world moves the body to frame.
        bool teleport = false;
        // A member's Velocity or AngularVelocity was written: the body takes it.
        bool velocity_pending = false;
        Vec3 velocity{};
        bool angular_pending = false;
        Vec3 angular{};
        // Made when play started: its body moves at its root's authored
        // velocity. Made later, by a weld made or broken in play: its body keeps
        // the momentum of the bodies its members were on.
        bool captured = true;
        // The stamp of a body this assembly takes over, keeping that body's
        // frame and its kept members' shapes: the piece of a split that holds
        // the old root, or the side of a merge whose root was root. 0: a new body.
        std::uint32_t reuse = 0;
    };
    // start_simulation, before any script runs.
    void capture_play(DataModel& game);
    // Playing: brings the play state up to date. PhysicsWorld::sync and
    // Engine::step_physics call it; readers of Active and AssemblyRoot call
    // refresh only, so a read never changes play state.
    void sync_play(DataModel& game);
    const std::vector<PlayAssembly>& play() const { return play_; }
    std::vector<PlayAssembly>& play() { return play_; }
    const std::vector<PlayMember>& play_members() const { return play_members_; }
    // The play assembly a node is in, or -1.
    int play_of(InstanceId node) const;
    // Play: where each held thing (a bone edge's sub) must be for its hand
    // now. assembly is its play assembly, whose body must be at frame; -1
    // when the node is alone, and then frame is the node's. Scale removed.
    struct HeldTarget {
        InstanceId node = 0;
        int assembly = -1;
        Matrix4 frame{};
    };
    void held_targets(const DataModel& game, std::vector<HeldTarget>& out) const;
    // Playing: writes each held thing where its hand is now, as the physics
    // world writes a body back: no Changed, no history. The physics world
    // calls it after each step, and the engine after the frame's scripts, so
    // what is drawn is on the hand however the hand moved.
    void place_held(DataModel& game);

    // Writes every member of play()[assembly] from body_pose × its offset,
    // keeping each one's own scale, as the physics world writes a body back:
    // no Changed, no history. skip: one already written by its own setter.
    void pose_members(DataModel& game, std::size_t assembly, const Matrix4& body_pose, Vec3 velocity, Vec3 angular,
                      InstanceId skip = 0);

    // frame with the length of each of scale_from's axes.
    static Matrix4 with_scale(const Matrix4& frame, const Matrix4& scale_from);

    // For a Brush recentre: inside one, a checked Transform write to id is a
    // change of pivot, not a move, so in play no other member moves.
    void begin_pivot_change(InstanceId id) { pivot_change_ = id; }
    void end_pivot_change() { pivot_change_ = 0; }

private:
    struct Edge {
        InstanceId constraint = 0;
        InstanceId node1 = 0;
        InstanceId node2 = 0;
        bool rigid = false;
        bool live = true;
        // A rigid edge's end (1 or 2) that is a Bone posing a bone, else 0.
        int bone_end = 0;
    };

    void rebuild(const DataModel& game);
    void changed(DataModel& game);
    // An end of constraint (1 or 2), skipping a GUID known missing until one may have come back.
    InstanceId end_of(const DataModel& game, InstanceId constraint, const InstanceRef& ref, int end);
    // The editor follow's helpers (AssemblyFollow.cpp).
    void mark_follow(InstanceId node, std::uint8_t bits);
    std::uint8_t follow_marks(InstanceId node) const;
    bool snap(DataModel& game, const RigidEdge& edge, InstanceId camera, bool alone);
    // Fills side_ with edge's sub, then, unless alone, every node it reaches
    // through active constraints but edge and bone edges. Only the sub when
    // that reaches edge's parent: moving the rest would move the parent.
    void gather_side(const RigidEdge& edge, bool alone);
    // Where edge's sub node must be (target) for its pivot to sit on its
    // parent's, from where it is (frame). False when an end does not resolve.
    bool sub_target(const DataModel& game, const RigidEdge& edge, Matrix4& frame, Matrix4& target) const;
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
    // Union-find over rigid edges alone, which breaks rigid loops.
    std::uint32_t rigid_find(std::uint32_t slot);
    // Rigid rule 2: points each rigid edge away from its assembly's root, and
    // fills rigid_ in follow order. Needs groups_ and nodes_.
    void orient_rigid(const DataModel& game);
    // Whether a joint between node1 and node2 holds, once groups_ is built.
    bool joint_holds(const DataModel& game, InstanceId node1, InstanceId node2) const;
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
    std::vector<std::uint32_t> rigid_uf_;
    // Per slot: a rigid tree's anchor, kept at the tree's union-find root, and
    // a node's place in nodes_.
    std::vector<InstanceId> anchor_;
    std::vector<std::int32_t> node_pos_;
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
    std::vector<RigidEdge> bone_;
    std::vector<std::pair<InstanceId, InstanceId>> no_collide_;
    std::vector<JointEdge> joints_;
    // orient_rigid's scratch, per place in nodes_: live edges by node, as
    // offsets into adj_; hops from the root over every live edge; hops from
    // the rigid tree's anchor over rigid edges; and the BFS queue.
    std::vector<std::uint32_t> adj_start_;
    std::vector<std::uint32_t> adj_cursor_;
    std::vector<std::uint32_t> adj_;
    std::vector<std::int32_t> hops_;
    std::vector<std::int32_t> rigid_hops_;
    std::vector<std::uint32_t> queue_;
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

    // Play state (AssemblyPlay.cpp).
    struct PlayLoser {
        InstanceId instance = 0;  // a PhysicsBase that lost its GameObject to a member
        std::uint32_t member = 0;
    };
    void write_member(DataModel& game, InstanceId node, InstanceId driven, const Matrix4& frame, Vec3 velocity,
                      Vec3 angular, InstanceId skip);
    std::vector<PlayAssembly> play_;
    std::vector<PlayMember> play_members_;
    std::vector<PlayLoser> play_losers_;
    std::vector<InstanceId> play_seen_;
    std::vector<std::int32_t> play_index_;
    std::vector<std::int32_t> play_member_index_;
    std::vector<std::uint32_t> play_touched_;
    std::uint32_t next_stamp_ = 0;
    bool play_captured_ = false;
    std::uint32_t play_generation_ = 0;
    // Rebuilds so far, and how many the play state has followed.
    std::uint64_t builds_ = 0;
    std::uint64_t play_builds_ = 0;
    // RigidConstraints active in play as of the last play sync.
    std::vector<InstanceId> play_rigid_;
    std::vector<PlayAssembly> old_play_;
    std::vector<PlayMember> old_members_;
    std::vector<InstanceId> side_;
    std::vector<std::uint8_t> side_mark_;
    // snap's scratch: the movers of side_ (mover_of), marked 2 in side_mark_.
    std::vector<InstanceId> side_movers_;
    InstanceId pivot_change_ = 0;
    void play_rebuilt(DataModel& game);
    void index_play();
    int old_play_of(InstanceId node) const;
    // The body's pose now, from where its members were last written.
    Matrix4 body_pose(const DataModel& game, const PlayAssembly& assembly,
                      const std::vector<PlayMember>& members) const;
    // A RigidConstraint newly active in play: the sub's side moves rigidly so
    // its pivot lands on the parent's.
    void snap_in_play(DataModel& game, const RigidEdge& edge);
};

// Holds Assemblies suspended for a scope (§3): load, undo and redo of edit
// waypoints, Stop's restore, paste.
// A Brush's recentre: its Transform moves one way and its faces the other,
// so nothing moves in the world. In play, its assembly keeps still.
class AssemblyPivotChange {
public:
    AssemblyPivotChange(DataModel& game, InstanceId id);
    ~AssemblyPivotChange();
    AssemblyPivotChange(const AssemblyPivotChange&) = delete;
    AssemblyPivotChange& operator=(const AssemblyPivotChange&) = delete;

private:
    DataModel& game_;
};

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
