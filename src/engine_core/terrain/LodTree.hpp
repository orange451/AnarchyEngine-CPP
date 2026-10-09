#pragma once

// Task 5 of the terrain LOD plan: one Terrain's LOD octree bookkeeping.
// Which nodes exist, which are stale, which level-0/1 meshes are resident
// for a camera position, and which node builds are due next. Pure: no
// threads, no TerrainWorld, no voxels -- TerrainWorld feeds it chunk results
// and node results and acts on what it asks for (chunk jobs, node jobs,
// meshes to drop). See docs/superpowers/specs/2026-10-06-terrain-lod-
// design.md ("Memory", "Building and edits").

#include "Vector3.hpp"
#include "amesh.hpp"
#include "terrain/ChunkExtent.hpp"
#include "terrain/LodBuilder.hpp"
#include "terrain/LodNode.hpp"
#include "terrain/TerrainMesher.hpp"
#include "terrain/VoxelChunk.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace engine_core {

// One drawable LOD node, for the renderer (Task 6 selects among these).
struct TerrainNodeView {
    terrain::NodeKey key;
    std::uint64_t revision = 0;   // changes whenever mesh does; unique across every node a TerrainWorld publishes
    float error = 0.f;            // units; 0 for level 0 (the chunk mesh itself)
    // Terrain-local: the union of node_bounds(key) and the mesh's own AABB
    // (R2 -- Surface Nets boundary vertices and skirts lie outside the box).
    Vec3 bounds_min{}, bounds_max{};
    // R4: bit i set when children_of(key)[i] exists in the tree with surface:
    // a level-0 child whose last mesh had triangles (resident or not), a
    // level >= 1 child once built with surface (resident or not). Selection
    // descends only when every child in the mask is in the published set.
    // A child not built yet is left out (R24): a parent
    // is built only once its children are, so such a child is new since the
    // parent's build -- made by an edit, whose queued neighbor chunks are
    // mostly air and come back empty -- and its region had no surface in the
    // parent's mesh. Counting it would make the parent draw itself, coarse,
    // until it was built or removed. Its published descendants are roots for
    // selection meanwhile (TerrainSelection: no published ancestor covers them).
    // Always 0 at level 0.
    std::uint8_t child_mask = 0;
    // R26: the node awaits a rebuild because a chunk under it was queued
    // or re-meshed since its build (an edit): its mesh shows the terrain as
    // it was. Selection draws such a node only when it cannot descend (a
    // child in child_mask is not published). Always false at level 0.
    bool stale = false;
    // Exactly one of these is set (R12). Level 0: mesh is the chunk mesh
    // and compact is null. Levels >= 1: compact is the node's quantized
    // mesh, shared with LodTree (never copied), and mesh is null, so nothing
    // holds a level >= 1 node unpacked in RAM. The renderer unpacks compact
    // (terrain::unpack) when it uploads the node (Task 6) and lets the
    // unpacked copy go once the GPU has it.
    std::shared_ptr<const anarchy::amesh::Data> mesh;
    std::shared_ptr<const terrain::CompactMesh> compact;
};

}  // namespace engine_core

namespace engine_core::terrain {

// Levels 0-1 are resident within kNearChunks of the camera's chunk (L-infinity,
// chunk units, Terrain-local) and dropped beyond kFarChunks; between the two a
// mesh is kept if present and not fetched if absent (hysteresis).
inline constexpr int kNearChunks = 6;
inline constexpr int kFarChunks = 8;
// A stale node is queued for a rebuild at most once per this many ms.
inline constexpr double kRebuildIntervalMs = 100.0;
// With a store attached, a level >= 2 node is kept in RAM within this many
// nodes of the camera at its own level (L-infinity, in that level's node
// spans), and dropped beyond kFarRingNodes + 2 once the store holds its
// current build. Nodes per level stay about constant however big the
// Terrain is. The top level never leaves.
inline constexpr int kFarRingNodes = 4;

class AlodStore;

// A node build next_builds asks for. input.voxels is left null: the caller
// fills in its chunk-map snapshot (spec decision 4). revision goes back with
// the job and returns in NodeResult::revision.
struct NodeBuildRequest {
    LodInput input;
    std::uint64_t revision = 0;
};

class LodTree {
public:
    // revisions: a counter shared by every tree of one TerrainWorld (never
    // reused for its life), so a node job or view revision from a dropped
    // tree can never match one this tree hands out. Null uses the tree's own.
    explicit LodTree(float voxel_size, std::uint64_t* revisions = nullptr);

    LodTree(const LodTree&) = delete;
    LodTree& operator=(const LodTree&) = delete;

    float voxel_size() const { return voxel_size_; }

    // A chunk job was queued for coord. edited: its voxels (may have)
    // changed, so every ancestor is marked stale; false for a re-mesh asked
    // for residency only. Until its result lands (chunk_meshed or
    // chunk_removed), coord's parent cannot build.
    void chunk_queued(ChunkCoord coord, bool edited);
    // coord's job landed with triangles. edited as for chunk_queued; a
    // chunk that had no surface before counts as edited regardless.
    void chunk_meshed(ChunkCoord coord, std::shared_ptr<const anarchy::amesh::Data> mesh, bool edited);
    // coord's job landed with no triangles: its level-0 node no longer
    // exists; ancestors left with no children are removed, the rest marked stale.
    void chunk_removed(ChunkCoord coord);
    // coord's job failed (TerrainMesher reported it: MeshResult::failed).
    // The chunk keeps the mesh it had, if any, and stops blocking its
    // parent, which builds from that old mesh (or without the chunk when
    // none is resident). A residency re-mesh is not asked for again until
    // the chunk is next queued, so a chunk that always fails does not retry
    // every update. A chunk that never had surface is removed as
    // chunk_removed would.
    void chunk_failed(ChunkCoord coord);
    // A node job finished. Ignored unless the node still exists and the
    // result is newer than the one it has (results may arrive out of order).
    // A level >= 1 result with no triangles removes the node (only nodes
    // with surface exist), and each ancestor left with no children with it.
    void node_built(const NodeResult& result);
    // A node job failed (NodeResult::failed). If it was the node's latest
    // queued build, the node may be queued again once kRebuildIntervalMs
    // has passed since that build was queued.
    void node_failed(const NodeResult& result);

    // Levels 0-1 against camera_chunk (null: no camera, everything resident).
    // Appends to out_drop_chunks the chunk meshes it dropped (the caller
    // stops showing them) and to out_need_chunks the chunks it wants meshed
    // again (the caller queues each, then calls chunk_queued(coord, false)).
    // A mesh is dropped only beyond kFarChunks and only once its parent is
    // built and current; it is wanted within kNearChunks, when it has no
    // parent, or when its parent is stale (a rebuild needs it). A level-1
    // node wanted but not resident is marked stale, so it rebuilds from its
    // (then fetched) chunks.
    void update_residency(const ChunkCoord* camera_chunk, std::vector<ChunkCoord>& out_drop_chunks,
                          std::vector<ChunkCoord>& out_need_chunks);
    // Stale nodes whose children are all present (not in flight, not stale,
    // and resident where they have surface) and whose last build was queued
    // at least kRebuildIntervalMs before now_ms: lowest level first, nearest
    // camera_chunk first within a level. Marks each queued.
    void next_builds(double now_ms, const ChunkCoord* camera_chunk, std::vector<NodeBuildRequest>& out);

    // Far nodes (level >= 2) are written to store as they build, and from
    // then on may leave RAM (see kFarRingNodes). Null: every node stays.
    // The store must outlive the tree or be detached first.
    void attach_store(AlodStore* store) { store_ = store; }
    // Off while the voxels differ from the saved file (edits not yet saved):
    // rebuilt nodes are not written to the store, and so stay in RAM, until
    // a save rewrites the store from RAM. On by default.
    void set_store_writes(bool on) { store_writes_ = on; }
    // The store now holds every current level >= 2 build (it was just
    // rewritten from them): each may leave RAM.
    void mark_all_persisted();
    // Nothing can be read back: every level >= 2 node stays in RAM (those
    // already out are read from nowhere, so they rebuild when wanted).
    void forget_persisted();
    // A warm start, on a tree with no nodes yet: every node the store holds,
    // built and current but not in RAM, a level-1 node over each of its
    // surface chunks (built, not in RAM), and a level-0 node per surface
    // chunk with no mesh. update_residency then asks for what is near.
    void adopt_store();
    // Level >= 2 nodes update_residency wants read back from the store;
    // the caller reads each and hands it to node_loaded.
    void take_loads(std::vector<NodeKey>& out);
    // key's mesh as the store had it (null: unreadable, so it is rebuilt).
    void node_loaded(const NodeKey& key, std::shared_ptr<const CompactMesh> mesh);
    // Bytes of the level >= 1 meshes in RAM.
    std::size_t compact_bytes() const;
    // Every level-0 chunk with surface, for the store's next commit.
    std::vector<ChunkCoord> surface_chunks() const;
    // No chunk job in flight and no level >= 1 node stale: every built node
    // shows the voxels as they are.
    bool settled() const;

    // Every resident node with a mesh: level 0 with its chunk mesh, levels
    // >= 1 with their shared compact mesh (R12: nothing is unpacked here).
    std::vector<TerrainNodeView> nodes_for_view();
    // True once since anything nodes_for_view reports changed.
    bool take_changed();

    // The level whose nodes are the roots: the lowest at which every node
    // has collapsed into one per axis, or, for a Terrain straddling a node
    // boundary through the origin (floor division never joins -1 and 0),
    // into the two either side of it -- so up to 8 roots.
    int top_level();

    // Bookkeeping per node, exposed read-only for tests.
    struct Node {
        // Level 0 only.
        std::shared_ptr<const anarchy::amesh::Data> chunk_mesh;   // resident chunk mesh, or null
        bool in_flight = false;                                    // a chunk job is queued
        bool failed = false;   // the last job failed: not asked for again for residency until queued
        // Every level. Level 0: the last result had triangles. Level >= 1:
        // the last build's result had triangles (meaningless until built;
        // a build without triangles removes the node, so once built it is true).
        bool has_surface = false;
        std::uint64_t mesh_revision = 0;   // TerrainNodeView::revision of the current mesh
        Vec3 bounds_min{}, bounds_max{};
        // Level >= 1 only.
        std::uint64_t revision = 0;          // bumped each time the node is marked stale
        std::uint64_t min_revision = 0;      // revision at creation: older results are someone else's
        std::uint64_t built_revision = 0;    // revision of the last accepted build
        std::uint64_t queued_revision = 0;   // revision of the last queued build
        double last_build_ms = -std::numeric_limits<double>::infinity();
        bool built = false;
        bool resident = false;   // compact holds the mesh
        // Level >= 2: the store holds this build, so compact may be dropped
        // and read back. Cleared when the node goes stale.
        bool persisted = false;
        bool loading = false;    // asked for through take_loads, not landed
        std::shared_ptr<const CompactMesh> compact;   // shared with each TerrainNodeView of this build
        float error = 0.f;
        bool stale() const { return built_revision != revision; }
    };
    const std::unordered_map<NodeKey, Node, NodeKeyHash>& nodes() const { return nodes_; }
    const Node* find(const NodeKey& key) const;

private:
    std::uint64_t next_revision() { return ++*revisions_; }
    Node* find_mutable(const NodeKey& key);
    // Recomputes top_ from the level-0 extent (level0_), and
    // adds or removes levels to match.
    void refresh_top();
    // Creates coord's missing ancestors up to top_ (created ones are stale);
    // with mark, also marks the existing ones stale.
    void ensure_ancestors(ChunkCoord coord, bool mark);
    void mark_stale(Node& node);
    bool has_children(const NodeKey& key) const;
    bool surfaced(const NodeKey& key, const Node& node) const;
    bool child_ready(const NodeKey& key, const Node& node) const;
    // The parent exists, is built and current: a child may be dropped.
    bool parent_covers(const NodeKey& key) const;
    // The parent (if any) is stale, or there is none: key's mesh is wanted.
    bool parent_wants(const NodeKey& key) const;
    // Erases key and then each ancestor left with no children. The first
    // ancestor that keeps children is marked stale when had_surface (the
    // erased node's mesh was part of it).
    void prune_up(const NodeKey& key, bool had_surface);

    // update_residency's pass over levels >= 2: evicts persisted far nodes
    // and asks for wanted ones back.
    void update_far_residency(const ChunkCoord* camera_chunk);

    float voxel_size_;
    AlodStore* store_ = nullptr;
    bool store_writes_ = true;
    std::vector<NodeKey> loads_;
    std::uint64_t own_revisions_ = 0;
    std::uint64_t* revisions_;
    std::unordered_map<NodeKey, Node, NodeKeyHash> nodes_;   // every level, level 0 included
    ChunkExtent level0_;   // every level-0 node's coord: the extent top_ follows
    int top_ = 0;
    bool changed_ = false;
    // update_residency skips its pass while neither the tree (any public
    // mutator) nor the camera's chunk has changed since the last one.
    bool residency_dirty_ = true;
    bool residency_had_camera_ = false;
    ChunkCoord residency_camera_{};
};

}  // namespace engine_core::terrain
