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
#include "TerrainMemory.hpp"
#include "terrain/AlodStore.hpp"
#include "terrain/LodTree.hpp"
#include "terrain/TerrainMesher.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine_core {

class Terrain;
class TerrainTextures;
struct TerrainTextureSet;

// A Terrain's look: what each material Id draws as. Immutable once published.
struct TerrainLook {
    // 256 x 4 RGBA32F, row-major: row 0 color (sRGB values), row 1
    // (metalness, roughness, reflectivity, 1), row 2 (layer index,
    // TextureScale, BlendSharpness, HeightStrength), row 3 reserved (0).
    // Index 0 and unassigned Ids use the Material defaults, and layer 0 (no
    // texturing yet -- Task 6 uploads TerrainTextures' arrays).
    std::array<float, 256 * 4 * 4> texels{};
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
    // Task 5: this Terrain's latest published texture arrays, from
    // TerrainTextures::published(). Filled by attach_textures() after the
    // Engine's textures_.update(); the renderer does not consume it yet
    // (Task 6 uploads it).
    std::shared_ptr<const TerrainTextureSet> textures;
};

// Keeps every Terrain in Workspace meshed. SimulationThread, under the write
// lock: the Engine calls update once per tick, playing or stopped.
class TerrainWorld {
public:
    // build_node: TerrainMesher's node-build override (tests inject a failing one); empty builds normally.
    explicit TerrainWorld(terrain::TerrainMesher::BuildCollider build = {}, unsigned threads = 0,
                          terrain::TerrainMesher::BuildNode build_node = {});

    // Task 5: rebuild_look's source for each Id's layer index (row 2),
    // through textures->layer_of(). Null (the default; every test that does
    // not call this) keeps every Id's layer at 0, the pre-Task-5 look. The
    // Engine calls this once, wiring its sibling TerrainTextures in.
    void set_terrain_textures(const TerrainTextures* textures) { textures_ = textures; }
    // The Engine, right after textures.update(game): fills every view's
    // textures field from textures.published(). A no-op for a Terrain this
    // TerrainWorld has not published a view for.
    void attach_textures(const TerrainTextures& textures);

    // Finds Terrains, queues their dirty chunks (all of them the first time a
    // Terrain is seen), collects finished meshes, rebuilds changed looks.
    void update(DataModel& game);
    // The same, with the clock (milliseconds, any origin, never decreasing)
    // that spaces a stale LOD node's rebuilds kRebuildIntervalMs apart.
    // update(game) passes steady_clock; tests pass their own.
    void update(DataModel& game, double now_ms);
    const std::vector<TerrainView>& views() const { return views_; }

    // Colliders by chunk, for PhysicsWorld: the latest for each meshed chunk
    // within collider interest (set_collider_interest).
    struct ChunkCollider {
        terrain::ChunkCoord coord;
        std::uint64_t revision;
        std::shared_ptr<void> collider;
    };
    const std::vector<ChunkCollider>* colliders(InstanceId terrain) const;

    // Task 8: PhysicsWorld's ask, at the start of its own sync, for the
    // chunks (terrain-local) within kColliderChunks of each dynamic
    // PhysicsObject or PlayerController. Takes effect from the next update():
    // a chunk newly asked for whose collider is not known yet gets a re-mesh
    // flagged to build one (unless build_colliders_now already did, or no
    // chunk is stored in its 3x3x3 neighborhood: then it is known to have
    // nothing to collide with, with no job and no LOD tree node), once:
    // a chunk meshed with nothing to collide with is known too, and is not
    // meshed again until an edit changes it; one no longer asked for keeps
    // its collider for kColliderReleaseMs after it was last asked for, then
    // loses it. No-op for a terrain this TerrainWorld has not (yet, or any
    // longer) seen.
    void set_collider_interest(InstanceId terrain, std::vector<terrain::ChunkCoord> chunks);
    // Whether terrain has any collider interest (a record made again, say
    // after the Terrain left Workspace and came back, starts with none).
    bool has_collider_interest(InstanceId terrain) const;
    // PhysicsWorld, as bodies move: the chunks newly in its interest, and
    // those no longer in it. Same effect as set_collider_interest with the
    // whole new set, at the cost of the change alone.
    void change_collider_interest(InstanceId terrain, const std::vector<terrain::ChunkCoord>& added,
                                  const std::vector<terrain::ChunkCoord>& removed);
    // SimulationThread, called from PhysicsWorld's own sync: meshes and
    // builds colliders for chunks right here, off the job queue -- the
    // no-fall-through rule for the chunks under a body. Skips a chunk whose
    // collider is already known (built, or meshed empty), so calling it
    // every sync costs lookups only; a chunk with no stored chunk anywhere
    // in its 3x3x3 neighborhood (empty space, e.g. far below the island) is
    // known at once, with no meshing and no LOD tree node. The Terrain is
    // looked up in game afresh (never a pointer kept from the last update).
    // False when terrain is not one this TerrainWorld has published a view
    // for, is no longer a live Terrain in game (nothing built), or a build threw.
    bool build_colliders_now(DataModel& game, InstanceId terrain, const std::vector<terrain::ChunkCoord>& chunks);

    // What terrain holds in RAM, as of the last few updates (refreshed every
    // kMemoryRefreshUpdates). Any thread.
    TerrainMemory memory(InstanceId terrain) const;
    static constexpr int kMemoryRefreshUpdates = 30;
    // With a camera in the same chunk, the LOD residency pass, the published
    // node list, and the far-mesh cache's commit check each run at most this
    // often: each walks every node, which on a huge Terrain is milliseconds.
    static constexpr double kResidencyIntervalMs = 250.0;
    static constexpr double kNodeListIntervalMs = 100.0;
    static constexpr double kStoreCheckIntervalMs = 500.0;

    // A first sight queues its chunks a few at a time rather than all at
    // once: at most this many jobs per mesher thread are in flight, so a
    // huge Terrain's first build holds only a handful of meshes and decoded
    // chunks at a time.
    static constexpr std::size_t kFirstBuildJobsPerThread = 2;

    // For tests.
    void wait_idle() { mesher_.wait_idle(); }
    // A first sight's chunks not handed to the mesher yet.
    std::size_t first_build_remaining(InstanceId terrain) const {
        const auto found = terrains_.find(terrain);
        return found != terrains_.end() ? found->second.first_build.size() : 0;
    }
    // No Terrain has first-build chunks left to admit.
    bool first_build_done() const {
        for (const auto& [id, record] : terrains_) {
            (void)id;
            if (!record.first_build.empty()) {
                return false;
            }
        }
        return true;
    }
    // Chunk jobs queued or running for terrain.
    std::size_t jobs_in_flight(InstanceId terrain) const {
        const auto found = terrains_.find(terrain);
        return found != terrains_.end() ? found->second.pending_jobs.size() : 0;
    }
    std::uint64_t meshed_count() const { return meshed_count_; }
    // How many chunks build_colliders_now has meshed itself (off the queue).
    std::uint64_t sync_meshed_count() const { return sync_meshed_count_; }
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
        // Task 5: the Material's texture numbers, and which texture layer it
        // draws as (TerrainTextures::layer_of, read fresh each update since
        // it can change there without this Id's Material or its PBR values
        // changing at all -- a newly seen Material appended to the layer
        // list on this very tick).
        double texture_scale = 0.0;
        double blend_sharpness = 0.0;
        double height_strength = 0.0;
        int layer = 0;
        bool operator==(const LookInput& o) const {
            return material == o.material && same_color(color, o.color) && metalness == o.metalness &&
                   roughness == o.roughness && reflectivity == o.reflectivity && texture_scale == o.texture_scale &&
                   blend_sharpness == o.blend_sharpness && height_strength == o.height_strength && layer == o.layer;
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
        // Task 8: what the live job for a chunk (chunk_revisions' value)
        // should do when it lands, decided when it was queued. edited: its
        // voxels may have changed (an edit queued it), so every ancestor is
        // marked stale; false for a re-mesh asked for render residency or a
        // collider refresh, neither of which touches voxels. want_collider:
        // the job was told to build one (coord was in collider_interest when
        // queued) -- its result's collider_map write follows this bit, not
        // edited, so an edit outside collider_interest builds no collider
        // and a residency or collider-refresh job inside it still writes
        // one. A chunk leaves this map once its job lands or fails
        // (apply_result); while present, nothing else may queue that coord
        // again without first deciding to replace it (queue_dirty does, for
        // a newer edit; update_lod's residency and the collider-interest
        // refresh below both skip a coord already here -- an edit, or
        // whatever else is already in flight for it, wins).
        struct PendingJob {
            bool edited = false;
            bool want_collider = false;
        };
        std::unordered_map<terrain::ChunkCoord, PendingJob, terrain::ChunkCoordHash> pending_jobs;
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
        // R26: the chunk jobs one update's edits queued (a batch) publish
        // together, once each has landed or failed. R31: a later edit that
        // queues a chunk of a batch still pending again folds that batch into
        // its own (the older batch's landed results for chunks not queued
        // again are kept, the rest wait on the newer jobs), and a batch held
        // kEditBatchHoldMs since its oldest edit publishes what has landed
        // (current results only) without waiting further -- so a stroke
        // editing the same chunks every update still shows while it lasts.
        // A Terrain's first sight is not batched: its chunks show as they land.
        struct EditBatch {
            std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> members;   // waiting or landed
            std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> waiting;   // jobs not landed yet
            std::unordered_map<terrain::ChunkCoord, terrain::MeshResult, terrain::ChunkCoordHash> landed;   // held
            double start_ms = 0.0;   // now_ms of the oldest edit folded in
        };
        std::unordered_map<std::uint64_t, EditBatch> batches;
        // Per member chunk of a pending batch (waiting or landed): its batch.
        std::unordered_map<terrain::ChunkCoord, std::uint64_t, terrain::ChunkCoordHash> chunk_batch;
        // Task 8: the chunks PhysicsWorld's latest set_collider_interest
        // asked for (terrain-local), and, per chunk with a collider, the
        // now_ms it was last asked for -- past kColliderReleaseMs since,
        // update_collider_interest drops it. A coord leaves the first set
        // only by a later set_collider_interest leaving it out; it leaves
        // the second only when its collider goes.
        std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> collider_interest;
        std::unordered_map<terrain::ChunkCoord, double, terrain::ChunkCoordHash> collider_last_interest_ms;
        // The interest chunks update_collider_interest has still to look at:
        // those newly asked for, and those a landing left unknown. With a
        // few dozen bodies the interest holds thousands of chunks, and
        // walking all of it every update cost milliseconds a step, so only
        // these are walked. A chunk still in the interest is still asked for:
        // the release skips it, and only chunks that leave it
        // (collider_interest_left) get a fresh last-asked time.
        std::vector<terrain::ChunkCoord> collider_to_walk;
        std::vector<terrain::ChunkCoord> collider_interest_left;
        double collider_release_scan_ms = 0.0;
        double collider_previous_update_ms = 0.0;
        // Chunks whose collider_map entry reflects their voxels: a job that
        // wanted a collider landed for them (with one, or with none because
        // the chunk has no surface). Interest and build_colliders_now skip
        // these; an edit landing without a collider, or the release, takes
        // a chunk out again.
        std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> collider_ready;
        // Chunks build_colliders_now already built a collider for (or found
        // nothing to collide with in) from the voxels of an edit still in
        // flight: not built again every sync until that edit lands
        // (apply_result) or a newer edit queues them again (queue_dirty).
        std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> sync_built;
        // A first sight's chunks not queued yet: admit_first_build hands
        // them to the mesher a few at a time, nearest the camera first (the
        // nearest at the back). The LOD tree already counts them in flight.
        std::vector<terrain::ChunkCoord> first_build;
        // Chunks an edit queued while first_build still held them: already
        // meshed with the edit, so not admitted again.
        std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> first_build_skip;
        bool first_build_sorted = false;
        bool first_build_had_camera = false;
        terrain::ChunkCoord first_build_camera{};
        // The far-mesh cache beside the Terrain's .avox (null: none, e.g. no
        // project). tree writes its far nodes there and reads them back.
        std::unique_ptr<terrain::AlodStore> store;
        // The Terrain's content key as last seen (it changes on a save).
        std::uint64_t saved_key = 0;
        // An edit landed since saved_key: the tree no longer matches the
        // file on disk, so the store is not committed until the next save.
        bool edited_since_save = false;
        // The store's last commit matches saved_key's voxels.
        bool store_committed = false;
        // The saved_key whose rewrite failed: not tried again until the next save.
        std::uint64_t rewrite_failed_key = 0;
        // update_lod's chunk-map snapshot for node jobs, and the volume
        // revision it was taken at.
        std::shared_ptr<const terrain::ChunkMap> voxels_snapshot;
        std::uint64_t voxels_snapshot_revision = 0;
        // The last LOD residency pass: when, and the camera chunk it used.
        double residency_ms = -1e300;
        bool residency_had_camera = false;
        terrain::ChunkCoord residency_camera{};
        // When the node list was last rebuilt, and update_store last ran.
        double nodes_ms = -1e300;
        double store_ms = -1e300;
    };

    // A result off the pool: dropped if stale, held if its edit batch still
    // waits on other jobs, else applied (with its batch's held results).
    void accept_result(const terrain::MeshResult& result);
    // Publishes one current result: meshes, colliders, the LOD tree.
    void apply_result(TerrainRecord& record, const terrain::MeshResult& result);
    // Applies a batch's landed results that are still current, and forgets
    // the batch: its members' later results apply as they land.
    void publish_batch(TerrainRecord& record, std::uint64_t batch);
    // R31: moves batch from's members, waiting jobs and held results into
    // batch into (keeping the older start), and forgets from.
    void fold_batch(TerrainRecord& record, std::uint64_t from, std::uint64_t into);
    // R31: publishes every batch held kEditBatchHoldMs or longer.
    void expire_batches(TerrainRecord& record, double now_ms);
    void queue_dirty(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, bool first_seen, bool has_camera,
                      Vec3 camera_pos, double now_ms);
    // First sight: opens the Terrain's .alod (true: the tree adopted it, so
    // there is no first build) or starts a new one.
    bool open_store(Terrain& terrain, TerrainRecord& record);
    // Commits the store once the tree has settled on the saved voxels, or
    // rewrites it under a new content key after a save.
    void update_store(Terrain& terrain, TerrainRecord& record);
    // Queues first_build's chunks, nearest first, while fewer than
    // kFirstBuildJobsPerThread jobs per mesher thread are in flight.
    void admit_first_build(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, bool has_camera,
                           Vec3 camera_pos);
    // One Terrain's LOD work for this update: drops and re-queues chunk
    // meshes as its LodTree asks, and queues the node builds now due.
    void update_lod(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, double now_ms, bool has_camera,
                    Vec3 camera_pos);
    // Task 8: chunks newly in collider_interest with a mesh but no collider
    // and nothing else already in flight for them get a re-mesh flagged to
    // build one; a collider not asked for in kColliderReleaseMs goes.
    void update_collider_interest(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, double now_ms);
    void rebuild_look(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, bool force);
    void publish_chunks(TerrainRecord& record);

    terrain::TerrainMesher mesher_;
    // A copy of the constructor's BuildCollider, for build_colliders_now,
    // which runs on SimulationThread itself rather than a mesher worker.
    terrain::TerrainMesher::BuildCollider build_collider_;
    // Task 5: set_terrain_textures. Null in every test that does not wire
    // one in, so layer_of is never called and every Id's layer stays 0.
    const TerrainTextures* textures_ = nullptr;
    std::unordered_map<InstanceId, TerrainRecord> terrains_;
    std::vector<TerrainView> views_;
    std::uint64_t meshed_count_ = 0;
    std::uint64_t sync_meshed_count_ = 0;
    std::uint64_t next_chunk_revision_ = 1;   // unique across every TerrainChunkView this world publishes
    std::uint64_t next_look_revision_ = 1;    // unique across every TerrainLook this world publishes
    // World-wide, never reused for the life of this TerrainWorld -- unlike a
    // per-record counter, which restarts at 1 whenever a Terrain's record is
    // dropped and recreated (leaves Workspace, then returns).
    std::uint64_t next_job_revision_ = 1;         // backs every TerrainRecord::chunk_revisions value
    std::uint64_t next_chunks_set_revision_ = 1;  // backs every TerrainRecord::chunks_revision value
    std::uint64_t next_nodes_set_revision_ = 1;   // backs every TerrainRecord::nodes_revision value
    std::uint64_t next_batch_ = 0;                // names each TerrainRecord::EditBatch
    // Shared by every Terrain's LodTree (node job and TerrainNodeView
    // revisions): never reused for this TerrainWorld's life, so a node job
    // from a Terrain's previous stay in Workspace can never be accepted.
    std::uint64_t next_node_revision_ = 0;
    // memory()'s figures per Terrain: chunk and far mesh bytes, refreshed by
    // update every kMemoryRefreshUpdates.
    mutable std::mutex memory_mutex_;
    std::unordered_map<InstanceId, TerrainMemory> memory_;
    int memory_countdown_ = 0;
};

}  // namespace engine_core
