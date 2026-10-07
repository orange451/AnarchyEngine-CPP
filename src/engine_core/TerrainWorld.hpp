#pragma once

// Keeps every Terrain in Workspace meshed. SimulationThread, under the
// DataModel's write lock: the Engine calls update() once per tick, playing or
// stopped. It queues each Terrain's dirty chunks with TerrainMesher, collects
// finished meshes, and publishes what the renderer (SnapshotPump) and
// PhysicsWorld read. The voxels themselves stay in Terrain/VoxelVolume; this
// class owns only meshes, colliders, and look tables.

#include "DataModel.hpp"
#include "terrain/TerrainMesher.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
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
    std::shared_ptr<const std::vector<TerrainChunkView>> chunks;   // replaced, never changed
    std::uint64_t chunks_revision = 0;
    std::shared_ptr<const TerrainLook> look;
};

// Keeps every Terrain in Workspace meshed. SimulationThread, under the write
// lock: the Engine calls update once per tick, playing or stopped.
class TerrainWorld {
public:
    explicit TerrainWorld(terrain::TerrainMesher::BuildCollider build = {}, unsigned threads = 0);

    // Finds Terrains, queues their dirty chunks (all of them the first time a
    // Terrain is seen), collects finished meshes, rebuilds changed looks.
    void update(DataModel& game);
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
        // Per-chunk coordinate, bumped every queue(): a result is accepted
        // only when it still matches the live counter (see accept_result).
        std::unordered_map<terrain::ChunkCoord, std::uint64_t, terrain::ChunkCoordHash> chunk_revisions;
        // Published meshes and colliders, by chunk coordinate: the source
        // accept_result writes and publish_chunks reads to rebuild the
        // vectors views() and colliders() hand out.
        std::unordered_map<terrain::ChunkCoord, TerrainChunkView, terrain::ChunkCoordHash> meshes;
        std::unordered_map<terrain::ChunkCoord, ChunkCollider, terrain::ChunkCoordHash> collider_map;
        bool chunks_dirty = false;   // a result landed since the last publish
        std::shared_ptr<const std::vector<TerrainChunkView>> chunks;
        std::uint64_t chunks_revision = 0;
        std::vector<ChunkCollider> colliders_vec;
        std::shared_ptr<const TerrainLook> look;
        std::array<LookInput, 256> look_inputs{};
    };

    void accept_result(const terrain::MeshResult& result);
    void queue_dirty(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, bool first_seen, bool has_camera,
                      Vec3 camera_pos);
    void rebuild_look(Terrain& terrain, TerrainRecord& record, bool force);
    void publish_chunks(TerrainRecord& record);

    terrain::TerrainMesher mesher_;
    std::unordered_map<InstanceId, TerrainRecord> terrains_;
    std::vector<TerrainView> views_;
    std::uint64_t meshed_count_ = 0;
    std::uint64_t next_chunk_revision_ = 1;   // unique across every TerrainChunkView this world publishes
    std::uint64_t next_look_revision_ = 1;    // unique across every TerrainLook this world publishes
};

}  // namespace engine_core
