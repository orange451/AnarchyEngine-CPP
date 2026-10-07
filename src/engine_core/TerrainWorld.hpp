#pragma once

// Keeps every Terrain in Workspace meshed. SimulationThread, under the
// DataModel's write lock: the Engine calls update() once per tick, playing or
// stopped. It queues each Terrain's dirty chunks with TerrainMesher, collects
// finished meshes, and publishes what the renderer (SnapshotPump) and
// PhysicsWorld read. Each Terrain also keeps a LOD octree (terrain/LodTree):
// node builds go on the same pool, levels 0-1 stay resident only near the
// camera, and every resident node is published for the renderer. The voxels
// themselves stay in Terrain/VoxelVolume; this class owns only meshes, LOD
// nodes, colliders, and look tables.

#include "DataModel.hpp"
#include "terrain/LodTree.hpp"
#include "terrain/TerrainMesher.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine_core {

class Terrain;

// A Terrain's look: what each material Id draws as. Immutable once published.
struct TerrainLook {
    // 256 x 2 RGBA8, row-major: row 0 sRGB color, row 1 (metalness, roughness,
    // reflectivity, 255). Index 0 and unassigned Ids use the Material defaults.
    std::array<std::uint8_t, 256 * 2 * 4> texels{};
    std::uint64_t revision = 0;   // unique across all looks
};

struct TerrainChunkView {
    terrain::ChunkCoord coord;
    std::uint64_t revision = 0;   // unique across all chunks
    std::shared_ptr<const anarchy::amesh::Data> mesh;
};

// What one Terrain shows, for the renderer and physics.
struct TerrainView {
    InstanceId terrain = 0;
    Matrix4 transform = matrix4_identity();
    bool can_collide = true;
    // Resident chunk meshes only (LodTree keeps levels 0-1 near the camera).
    // The renderer draws these until it selects from nodes instead.
    std::shared_ptr<const std::vector<TerrainChunkView>> chunks;   // replaced, never changed
    std::uint64_t chunks_revision = 0;
    std::shared_ptr<const TerrainLook> look;
    // Every resident LOD node with a mesh, sorted by (level, x, y, z).
    std::shared_ptr<const std::vector<TerrainNodeView>> nodes;   // replaced, never changed
    std::uint64_t nodes_revision = 0;
    int top_level = 0;   // the level of the octree's roots
};

// Keeps every Terrain in Workspace meshed. SimulationThread, under the write
// lock: the Engine calls update once per tick, playing or stopped.
class TerrainWorld {
public:
    // build_node: TerrainMesher's node-build override (tests inject a failing one); empty builds normally.
    explicit TerrainWorld(terrain::TerrainMesher::BuildCollider build = {}, unsigned threads = 0,
                          terrain::TerrainMesher::BuildNode build_node = {});

    // Finds Terrains, queues their dirty chunks (all of them the first time a
    // Terrain is seen), collects finished meshes, rebuilds changed looks.
    void update(DataModel& game);
    // The same, with the clock (milliseconds, any origin, never decreasing)
    // that spaces a stale LOD node's rebuilds kRebuildIntervalMs apart.
    // update(game) passes steady_clock; tests pass their own.
    void update(DataModel& game, double now_ms);
    const std::vector<TerrainView>& views() const { return views_; }

    // Colliders by chunk, for PhysicsWorld: the latest for each meshed chunk.
    struct ChunkCollider {
        terrain::ChunkCoord coord;
        std::uint64_t revision;
        std::shared_ptr<void> collider;
    };
    const std::vector<ChunkCollider>* colliders(InstanceId terrain) const;

    // For tests.
    void wait_idle() { mesher_.wait_idle(); }
    std::uint64_t meshed_count() const { return meshed_count_; }
    const terrain::LodTree* lod_tree(InstanceId terrain) const {
        const auto found = terrains_.find(terrain);
        return found != terrains_.end() ? found->second.tree.get() : nullptr;
    }

    // A mesh/collider/node build that threw: its chunk keeps its old mesh and
    // collider (a node, its old build, and it may be queued again), the LOD
    // tree stops waiting on it, and TerrainMesher keeps a count and the
    // latest message. The Engine reads these from SimulationThread each update() and
    // reports a rise once, the way it already does other faults.
    std::uint64_t mesh_failures() const { return mesher_.failure_count(); }
    std::string last_mesh_failure() const { return mesher_.last_failure(); }

private:
    // One Id's inputs to the look table, compared each update against what
    // was last published so an unrelated Material elsewhere never forces a
    // rebuild. Default-constructed (material == 0) for an unassigned Id or
    // one whose Material reference is nil or dead -- the same state, so
    // both draw the engine default and neither looks like a "change" to the
    // other.
    struct LookInput {
        InstanceId material = 0;
        ColorRgb color{};
        double metalness = 0.0;
        double roughness = 0.0;
        double reflectivity = 0.0;
        bool operator==(const LookInput& o) const {
            return material == o.material && same_color(color, o.color) && metalness == o.metalness &&
                   roughness == o.roughness && reflectivity == o.reflectivity;
        }
    };

    // Everything TerrainWorld keeps for one live Terrain.
    struct TerrainRecord {
        // Per-chunk coordinate, set to a fresh value from next_job_revision_
        // every queue(): a result is accepted only when it still matches the
        // live value (see accept_result). Drawn from a counter that lives on
        // TerrainWorld, not on this record, so a value is never reused even
        // after this record is dropped (Terrain left Workspace) and a later
        // record is created for the same Terrain returning: a job queued
        // during a previous stay can then never again match the live value
        // for any chunk, however the two records' own lifetimes line up.
        std::unordered_map<terrain::ChunkCoord, std::uint64_t, terrain::ChunkCoordHash> chunk_revisions;
        // Chunks whose live job was queued only to bring a dropped mesh back
        // (residency), not because their voxels changed: the result leaves
        // the collider alone and does not mark LOD ancestors stale. A chunk
        // leaves the set when an edit queues it again.
        std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> residency_jobs;
        // The LOD octree: made on first sight, and again (re-queueing every
        // chunk) if the volume's voxel size changes.
        std::unique_ptr<terrain::LodTree> tree;
        std::shared_ptr<const std::vector<TerrainNodeView>> nodes;
        std::uint64_t nodes_revision = 0;   // from next_nodes_set_revision_, as chunks_revision is
        // Published meshes and colliders, by chunk coordinate: the source
        // accept_result writes and publish_chunks reads to rebuild the
        // vectors views() and colliders() hand out.
        std::unordered_map<terrain::ChunkCoord, TerrainChunkView, terrain::ChunkCoordHash> meshes;
        std::unordered_map<terrain::ChunkCoord, ChunkCollider, terrain::ChunkCoordHash> collider_map;
        bool chunks_dirty = false;   // a result landed since the last publish
        std::shared_ptr<const std::vector<TerrainChunkView>> chunks;
        // Set from next_chunks_set_revision_ each publish_chunks, for the
        // same reason chunk_revisions draws from next_job_revision_: a
        // record recreated after a Terrain returns to Workspace must never
        // repeat a value an earlier record (for the same Terrain) already
        // published, in case a consumer is keyed on (terrain, chunks_revision).
        std::uint64_t chunks_revision = 0;
        std::vector<ChunkCollider> colliders_vec;
        std::shared_ptr<const TerrainLook> look;
        std::array<LookInput, 256> look_inputs{};
    };

    void accept_result(const terrain::MeshResult& result);
    void queue_dirty(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, bool first_seen, bool has_camera,
                      Vec3 camera_pos);
    // One Terrain's LOD work for this update: drops and re-queues chunk
    // meshes as its LodTree asks, and queues the node builds now due.
    void update_lod(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, double now_ms, bool has_camera,
                    Vec3 camera_pos);
    void rebuild_look(Terrain& terrain, TerrainRecord& record, bool force);
    void publish_chunks(TerrainRecord& record);

    terrain::TerrainMesher mesher_;
    std::unordered_map<InstanceId, TerrainRecord> terrains_;
    std::vector<TerrainView> views_;
    std::uint64_t meshed_count_ = 0;
    std::uint64_t next_chunk_revision_ = 1;   // unique across every TerrainChunkView this world publishes
    std::uint64_t next_look_revision_ = 1;    // unique across every TerrainLook this world publishes
    // World-wide, never reused for the life of this TerrainWorld -- unlike a
    // per-record counter, which restarts at 1 whenever a Terrain's record is
    // dropped and recreated (leaves Workspace, then returns).
    std::uint64_t next_job_revision_ = 1;         // backs every TerrainRecord::chunk_revisions value
    std::uint64_t next_chunks_set_revision_ = 1;  // backs every TerrainRecord::chunks_revision value
    std::uint64_t next_nodes_set_revision_ = 1;   // backs every TerrainRecord::nodes_revision value
    // Shared by every Terrain's LodTree (node job and TerrainNodeView
    // revisions): never reused for this TerrainWorld's life, so a node job
    // from a Terrain's previous stay in Workspace can never be accepted.
    std::uint64_t next_node_revision_ = 0;
};

}  // namespace engine_core
