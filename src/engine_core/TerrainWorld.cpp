#include "TerrainWorld.hpp"

#include "AssetInstances.hpp"
#include "PVInstance.hpp"
#include "SceneService.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "TerrainTextures.hpp"
#include "terrain/ChunkCache.hpp"
#include "terrain/VoxelVolume.hpp"
#include "profiler/Profiler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace engine_core {

namespace {

// Where chunk coord's center sits in Terrain-local space.
Vec3 chunk_center_local(const terrain::ChunkCoord& coord, float voxel_size) {
    constexpr float half = terrain::kChunkSize * 0.5f;
    return Vec3{(coord.x * terrain::kChunkSize + half) * voxel_size, (coord.y * terrain::kChunkSize + half) * voxel_size,
                (coord.z * terrain::kChunkSize + half) * voxel_size};
}

float distance(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Task 8: a collider not asked for by set_collider_interest in this long
// goes (spec: "a few seconds", R8 fixes it at 5 s).
constexpr double kColliderReleaseMs = 5000.0;

// R31: an edit batch holds its landed results at most this long after its
// oldest edit, then publishes what has landed without waiting further.
constexpr double kEditBatchHoldMs = 150.0;

// Final review: true if coord or any of its 26 neighbors holds a stored
// chunk -- the chunks a mesh of coord reads. With none, coord meshes to
// nothing (quick_reject's all-air case), so it needs no job.
bool stored_near(const terrain::VoxelVolume& volume, const terrain::ChunkCoord& coord) {
    const terrain::ChunkMap& chunks = volume.chunks();
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (chunks.count(terrain::ChunkCoord{coord.x + dx, coord.y + dy, coord.z + dz}) != 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

TerrainWorld::TerrainWorld(terrain::TerrainMesher::BuildCollider build, unsigned threads,
                           terrain::TerrainMesher::BuildNode build_node)
    : mesher_(build, threads, std::move(build_node)), build_collider_(std::move(build)) {}

void TerrainWorld::accept_result(const terrain::MeshResult& result) {
    // SimulationThread (called from update(), which owns terrains_).
    auto found = terrains_.find(static_cast<InstanceId>(result.terrain));
    if (found == terrains_.end()) {
        return;   // the Terrain left Workspace (or was destroyed) since this job was queued
    }
    TerrainRecord& record = found->second;
    // A landing can leave a chunk of the interest unknown: look at it again.
    if (record.collider_interest.count(result.coord) != 0) {
        record.collider_to_walk.push_back(result.coord);
    }
    const auto revision_it = record.chunk_revisions.find(result.coord);
    if (revision_it == record.chunk_revisions.end() || revision_it->second != result.revision) {
        // Stale: an older job than the chunk's current value, or one for a
        // value this record never issued (left over from a dropped-then-
        // reseen Terrain). Results can arrive out of order across workers,
        // so only an exact match on the live value is accepted. Because that
        // value comes from next_job_revision_ (world-wide, never reused --
        // see queue_dirty), a job queued during a Terrain's previous stay in
        // Workspace can never match the value its chunk holds now, even if
        // both happened to be the first job ever queued for that coordinate
        // in their respective records.
        return;
    }
    if (!result.failed) {
        ++meshed_count_;   // counted on landing, whether or not its batch publishes yet
    }
    const auto member = record.chunk_batch.find(result.coord);
    if (member != record.chunk_batch.end()) {
        const std::uint64_t id = member->second;
        const auto batch_it = record.batches.find(id);
        if (batch_it != record.batches.end() && batch_it->second.waiting.erase(result.coord) != 0) {
            // An edit's job: published with its batch.
            TerrainRecord::EditBatch& batch = batch_it->second;
            batch.landed[result.coord] = result;
            if (batch.waiting.empty()) {
                publish_batch(record, id);
            }
            return;
        }
    }
    apply_result(record, result);
}

void TerrainWorld::publish_batch(TerrainRecord& record, std::uint64_t id) {
    const auto batch_it = record.batches.find(id);
    if (batch_it == record.batches.end()) {
        return;
    }
    TerrainRecord::EditBatch batch = std::move(batch_it->second);
    record.batches.erase(batch_it);
    for (const terrain::ChunkCoord& coord : batch.members) {
        const auto member = record.chunk_batch.find(coord);
        if (member != record.chunk_batch.end() && member->second == id) {
            record.chunk_batch.erase(member);
        }
    }
    // A held result whose chunk was queued again since (a later edit not
    // folded in) is dropped; that chunk shows with the later job.
    for (const auto& [coord, result] : batch.landed) {
        const auto live = record.chunk_revisions.find(coord);
        if (live != record.chunk_revisions.end() && live->second == result.revision) {
            apply_result(record, result);
        }
    }
}

void TerrainWorld::fold_batch(TerrainRecord& record, std::uint64_t from, std::uint64_t into) {
    if (from == into) {
        return;
    }
    TerrainRecord::EditBatch& target = record.batches[into];   // first: an insert may rehash (iterators, not references)
    const auto from_it = record.batches.find(from);
    if (from_it == record.batches.end()) {
        return;
    }
    TerrainRecord::EditBatch& source = from_it->second;
    for (const terrain::ChunkCoord& coord : source.members) {
        record.chunk_batch[coord] = into;
        target.members.insert(coord);
    }
    target.waiting.insert(source.waiting.begin(), source.waiting.end());
    for (auto& [coord, result] : source.landed) {
        target.landed[coord] = std::move(result);
    }
    target.start_ms = std::min(target.start_ms, source.start_ms);
    record.batches.erase(from_it);
}

void TerrainWorld::expire_batches(TerrainRecord& record, double now_ms) {
    std::vector<std::uint64_t> expired;
    for (const auto& [id, batch] : record.batches) {
        if (now_ms - batch.start_ms >= kEditBatchHoldMs) {
            expired.push_back(id);
        }
    }
    for (std::uint64_t id : expired) {
        publish_batch(record, id);
    }
}

void TerrainWorld::apply_result(TerrainRecord& record, const terrain::MeshResult& result) {
    // Task 8: what this job was asked to do, decided when it was queued
    // (queue_dirty, update_lod's residency needs, or update_collider_interest).
    // Gone (should not happen: every live job has an entry) defaults to
    // neither an edit nor wanting a collider, so it changes nothing it is
    // not sure of.
    const auto pending = record.pending_jobs.find(result.coord);
    const bool edited = pending != record.pending_jobs.end() && pending->second.edited;
    const bool want_collider = pending != record.pending_jobs.end() && pending->second.want_collider;
    if (pending != record.pending_jobs.end()) {
        record.pending_jobs.erase(pending);
    }
    if (edited) {
        record.sync_built.erase(result.coord);   // the edit landed: its own result decides now
    }
    if (result.failed) {
        // The job threw: the chunk keeps its mesh and collider, and its
        // LOD ancestors stop waiting on it.
        if (record.tree != nullptr) {
            record.tree->chunk_failed(result.coord);
        }
        return;
    }
    const std::uint64_t view_revision = next_chunk_revision_++;
    if (result.mesh.render != nullptr) {
        record.meshes[result.coord] = TerrainChunkView{result.coord, view_revision, result.mesh.render};
    } else {
        record.meshes.erase(result.coord);
    }
    // The collider write follows want_collider, not edited: an edit outside
    // collider_interest builds no collider, and a residency or
    // collider-refresh job asked to build one (in collider_interest, not
    // known yet) still writes it even though the voxels did not change.
    if (want_collider) {
        if (result.collider != nullptr) {
            record.collider_map[result.coord] = ChunkCollider{result.coord, view_revision, result.collider};
        } else {
            record.collider_map.erase(result.coord);
        }
        // Known now, even with nothing to collide with (air, or solid all
        // through): not meshed again for interest until it changes.
        record.collider_ready.insert(result.coord);
    } else if (edited) {
        // Its voxels changed without a collider built from them: whatever
        // collider it still holds is stale. Task 9 (carried from Task 8's
        // review): drop it at once rather than leaving it live until
        // kColliderReleaseMs's grace lapses -- a Raycast's march must see
        // this chunk's new voxels (no collider: it is the march's to cover)
        // the moment this result lands, never a shape built from the old
        // ones.
        record.collider_ready.erase(result.coord);
        if (record.collider_map.erase(result.coord) != 0) {
            record.chunks_dirty = true;
        }
        record.collider_last_interest_ms.erase(result.coord);
    }
    record.chunks_dirty = true;
    if (record.tree != nullptr) {
        if (result.mesh.render != nullptr) {
            record.tree->chunk_meshed(result.coord, result.mesh.render, edited);
        } else {
            record.tree->chunk_removed(result.coord);
        }
    }
}

void TerrainWorld::queue_dirty(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, bool first_seen,
                                bool has_camera, Vec3 camera_pos, double now_ms) {
    terrain::VoxelVolume& volume = terrain.volume();
    std::vector<terrain::ChunkCoord> dirty;
    if (first_seen) {
        // A Terrain seen for the first time (just arrived in Workspace, or
        // TerrainWorld itself just started): drain whatever take_dirty would
        // have returned -- it is about to be superseded -- then queue every
        // stored chunk and its 26 neighbors (deduplicated), the same
        // footprint an edit's mark_dirty gives. A surface quad belongs to
        // the chunk holding the edge's lower endpoint, which can be an
        // unstored (air) neighbor of a stored chunk, so meshing stored
        // chunks alone can leave gaps at a Terrain's outer boundary.
        std::vector<terrain::ChunkCoord> drained;
        volume.take_dirty(drained);
        std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> footprint;
        for (const auto& [coord, chunk] : volume.chunks()) {
            (void)chunk;
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        footprint.insert(terrain::ChunkCoord{coord.x + dx, coord.y + dy, coord.z + dz});
                    }
                }
            }
        }
        if (open_store(terrain, record)) {
            // The tree took every far node from the .alod: only what is
            // near gets meshed, as update_lod asks for it.
            record.first_build.clear();
            record.first_build_skip.clear();
            return;
        }
        // Not queued here: admit_first_build hands them to the mesher a few
        // at a time. The tree counts each in flight now, so no parent
        // builds before all of its chunks have landed.
        record.first_build.assign(footprint.begin(), footprint.end());
        record.first_build_skip.clear();
        record.first_build_sorted = false;
        for (const terrain::ChunkCoord& coord : record.first_build) {
            record.tree->chunk_queued(coord, true);
        }
        return;
    }
    volume.take_dirty(dirty);
    if (!dirty.empty()) {
        record.edited_since_save = true;
        record.tree->set_store_writes(false);   // before this edit's nodes rebuild
    }
    if (!record.first_build.empty()) {
        // Meshed by this edit with its new voxels: the first build leaves them.
        record.first_build_skip.insert(dirty.begin(), dirty.end());
    }
    const float voxel_size = static_cast<float>(volume.voxel_size());
    // R26: an edit's chunks (not a first sight's) publish together.
    std::uint64_t batch = 0;
    if (!first_seen && !dirty.empty()) {
        batch = ++next_batch_;
        record.batches[batch].start_ms = now_ms;
    }
    for (const terrain::ChunkCoord& coord : dirty) {
        if (batch != 0) {
            // R31: a chunk of a batch still pending folds that batch into
            // this one; a result of its held there is superseded.
            const auto member = record.chunk_batch.find(coord);
            if (member != record.chunk_batch.end() && member->second != batch) {
                fold_batch(record, member->second, batch);
            }
            TerrainRecord::EditBatch& current = record.batches[batch];
            current.landed.erase(coord);
            current.members.insert(coord);
            current.waiting.insert(coord);
            record.chunk_batch[coord] = batch;
        }
        record.sync_built.erase(coord);   // its voxels changed again
        // Drawn from a counter that lives on TerrainWorld, not this record,
        // so the value is unique for the TerrainWorld's whole life -- see the
        // comment on TerrainRecord::chunk_revisions.
        const std::uint64_t revision = ++next_job_revision_;
        record.chunk_revisions[coord] = revision;
        // An edit always wins any other job already in flight for coord
        // (queue_dirty is the only queuer that replaces one), and its
        // collider decision follows interest, same as every other job's.
        const bool want_collider = record.collider_interest.count(coord) != 0;
        record.pending_jobs[coord] = TerrainRecord::PendingJob{true, want_collider};
        record.tree->chunk_queued(coord, true);
        float job_distance = 0.f;
        if (has_camera) {
            const Vec3 world_center = matrix4_point(terrain.transform(), chunk_center_local(coord, voxel_size));
            job_distance = distance(world_center, camera_pos);
        }
        terrain::MeshInput input = terrain::mesh_input(volume, coord);
        input.build_collider = want_collider;
        mesher_.queue(terrain_id, revision, std::move(input), job_distance);
    }
}

bool TerrainWorld::open_store(Terrain& terrain, TerrainRecord& record) {
    record.tree->attach_store(nullptr);
    record.store.reset();
    record.saved_key = terrain.content_key();
    record.edited_since_save = false;
    record.store_committed = false;
    record.rewrite_failed_key = 0;
    const std::filesystem::path path = terrain.lod_cache_path();
    if (path.empty() || record.saved_key == 0) {
        return false;   // no project, or voxels no file holds yet
    }
    {
        // A rewrite a previous session could not finish or rename.
        std::filesystem::path temp = path;
        temp += ".tmp";
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
    }
    const float voxel_size = terrain.volume().voxel_size();
    if (std::optional<terrain::AlodStore> store = terrain::AlodStore::open(path, record.saved_key, voxel_size)) {
        record.store = std::make_unique<terrain::AlodStore>(std::move(*store));
        record.tree->attach_store(record.store.get());
        record.tree->adopt_store();
        record.store_committed = true;
        return true;
    }
    if (std::optional<terrain::AlodStore> store = terrain::AlodStore::create(path, record.saved_key, voxel_size)) {
        record.store = std::make_unique<terrain::AlodStore>(std::move(*store));
        record.tree->attach_store(record.store.get());
    }
    return false;
}

void TerrainWorld::update_store(Terrain& terrain, TerrainRecord& record) {
    if (record.store == nullptr || record.edited_since_save || !record.first_build.empty() ||
        !record.pending_jobs.empty() || !record.tree->settled()) {
        return;
    }
    if (record.saved_key == 0) {
        return;
    }
    if (record.store->content_key() == record.saved_key) {
        if (!record.store_committed) {
            record.store->set_surface_chunks(record.tree->surface_chunks());
            record.store_committed = record.store->commit();
        }
        return;
    }
    // A save changed the voxels' key: write every far node, as the tree
    // now has them, to a new store under it, and swap it in. One try per
    // key: a failure waits for the next save rather than retrying every
    // update.
    if (record.rewrite_failed_key == record.saved_key) {
        return;
    }
    PROFILE_SCOPE("Terrain far-mesh cache rewrite", profiler::Group::Engine);
    const std::filesystem::path path = terrain.lod_cache_path();
    std::filesystem::path temp = path;
    temp += ".tmp";
    const float voxel_size = terrain.volume().voxel_size();
    const auto give_up = [&] {
        record.rewrite_failed_key = record.saved_key;
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
    };
    if (path.empty()) {
        give_up();
        return;
    }
    {
        std::optional<terrain::AlodStore> fresh = terrain::AlodStore::create(temp, record.saved_key, voxel_size);
        if (!fresh) {
            give_up();
            return;
        }
        for (const auto& [key, node] : record.tree->nodes()) {
            if (key.level < 2 || !node.built || !node.has_surface) {
                continue;
            }
            std::shared_ptr<const terrain::CompactMesh> mesh = node.compact;
            if (mesh == nullptr) {
                mesh = record.store->load(key);
            }
            if (mesh == nullptr || !fresh->put(key, *mesh, node.error, node.bounds_min, node.bounds_max)) {
                fresh.reset();
                give_up();   // the old store stays; the next save tries again
                return;
            }
        }
        fresh->set_surface_chunks(record.tree->surface_chunks());
        if (!fresh->commit()) {
            fresh.reset();
            give_up();
            return;
        }
    }
    record.tree->attach_store(nullptr);
    record.store.reset();   // closes the file so it can be replaced
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    // Not renamed (something holds the old file open): the new store stays
    // where it is for this session. It holds every node, so the tree reads
    // back from it; the next open finds the old file stale and rebuilds.
    if (std::optional<terrain::AlodStore> store =
            terrain::AlodStore::open(error ? temp : path, record.saved_key, voxel_size)) {
        record.store = std::make_unique<terrain::AlodStore>(std::move(*store));
        record.tree->attach_store(record.store.get());
        record.tree->mark_all_persisted();
        record.store_committed = true;
    } else {
        // Neither file opens: nothing can be read back, so nothing may leave RAM.
        record.tree->forget_persisted();
        record.rewrite_failed_key = record.saved_key;
    }
}

void TerrainWorld::admit_first_build(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, bool has_camera,
                                     Vec3 camera_pos) {
    if (record.first_build.empty()) {
        return;
    }
    const std::size_t cap = static_cast<std::size_t>(mesher_.thread_count()) * kFirstBuildJobsPerThread;
    if (record.pending_jobs.size() >= cap) {
        return;
    }
    terrain::VoxelVolume& volume = terrain.volume();
    const float voxel_size = static_cast<float>(volume.voxel_size());
    const Matrix4& transform = terrain.transform();
    // Re-sorted only when the camera moves to another chunk: farthest
    // first, so the nearest is popped off the back.
    if (has_camera) {
        const Vec3 local = matrix4_point(matrix4_inverse(transform), camera_pos);
        const float span = terrain::kChunkSize * voxel_size;
        const terrain::ChunkCoord camera_chunk{static_cast<int>(std::floor(local.x / span)),
                                               static_cast<int>(std::floor(local.y / span)),
                                               static_cast<int>(std::floor(local.z / span))};
        if (!record.first_build_sorted || !record.first_build_had_camera ||
            !(camera_chunk == record.first_build_camera)) {
            std::vector<std::pair<float, terrain::ChunkCoord>> keyed;
            keyed.reserve(record.first_build.size());
            for (const terrain::ChunkCoord& coord : record.first_build) {
                const Vec3 center = matrix4_point(transform, chunk_center_local(coord, voxel_size));
                keyed.emplace_back(distance(center, camera_pos), coord);
            }
            std::sort(keyed.begin(), keyed.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
            for (std::size_t i = 0; i < keyed.size(); ++i) {
                record.first_build[i] = keyed[i].second;
            }
            record.first_build_sorted = true;
            record.first_build_had_camera = true;
            record.first_build_camera = camera_chunk;
        }
    }
    while (!record.first_build.empty() && record.pending_jobs.size() < cap) {
        const terrain::ChunkCoord coord = record.first_build.back();
        record.first_build.pop_back();
        if (record.first_build_skip.count(coord) != 0 || record.pending_jobs.count(coord) != 0) {
            continue;   // an edit (or another job) already meshes it with its current voxels
        }
        const std::uint64_t revision = ++next_job_revision_;
        record.chunk_revisions[coord] = revision;
        const bool want_collider = record.collider_interest.count(coord) != 0;
        record.pending_jobs[coord] = TerrainRecord::PendingJob{true, want_collider};
        const float job_distance =
            has_camera ? distance(matrix4_point(transform, chunk_center_local(coord, voxel_size)), camera_pos) : 0.f;
        terrain::MeshInput input = terrain::mesh_input(volume, coord);
        input.build_collider = want_collider;
        mesher_.queue(terrain_id, revision, std::move(input), job_distance);
    }
    if (record.first_build.empty()) {
        record.first_build_skip.clear();
    }
}

void TerrainWorld::update_lod(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, double now_ms,
                              bool has_camera, Vec3 camera_pos) {
    terrain::LodTree& tree = *record.tree;
    const float voxel_size = tree.voxel_size();
    const Matrix4& transform = terrain.transform();
    terrain::ChunkCoord camera_chunk;
    const terrain::ChunkCoord* camera = nullptr;   // no camera: everything stays resident
    if (has_camera) {
        const Vec3 local = matrix4_point(matrix4_inverse(transform), camera_pos);
        const float span = terrain::kChunkSize * voxel_size;
        camera_chunk = terrain::ChunkCoord{static_cast<int>(std::floor(local.x / span)),
                                           static_cast<int>(std::floor(local.y / span)),
                                           static_cast<int>(std::floor(local.z / span))};
        camera = &camera_chunk;
    }

    std::vector<terrain::ChunkCoord> drops;
    std::vector<terrain::ChunkCoord> needs;
    // A residency pass walks every node, so while the camera stays in its
    // chunk it runs at most every kResidencyIntervalMs, however many chunks
    // land meanwhile (a huge Terrain's first build lands some every update,
    // and play's collider jobs keep landing). Without a camera it always
    // runs: everything is wanted, and nothing waits on distance.
    const bool camera_moved = has_camera != record.residency_had_camera ||
                              (has_camera && !(camera_chunk == record.residency_camera));
    // With no camera every node is wanted. A Terrain with a far-mesh store
    // (one in a project, possibly huge, warm-opened with every far node out
    // of RAM) waits for the camera instead: wanting everything there means
    // meshing every chunk and reading every node at once, which is exactly
    // what the store is there to avoid. Studio's first updates after an
    // open come before Workspace has a camera.
    const bool wait_for_camera = !has_camera && record.store != nullptr;
    if (!wait_for_camera &&
        (!has_camera || camera_moved || now_ms - record.residency_ms >= kResidencyIntervalMs)) {
        PROFILE_SCOPE("Terrain residency", profiler::Group::Engine);
        tree.update_residency(camera, drops, needs);
        record.residency_ms = now_ms;
        record.residency_had_camera = has_camera;
        record.residency_camera = camera_chunk;
    }
    if (record.store != nullptr) {
        // Far nodes the tree wants back: a few KB to a few MB each, read
        // here rather than on a worker since the store is not thread-safe.
        std::vector<terrain::NodeKey> loads;
        tree.take_loads(loads);
        for (const terrain::NodeKey& key : loads) {
            tree.node_loaded(key, record.store->load(key));
        }
    }
    for (const terrain::ChunkCoord& coord : drops) {
        if (record.meshes.erase(coord) != 0) {
            record.chunks_dirty = true;   // colliders stay (R5)
        }
    }
    terrain::VoxelVolume& volume = terrain.volume();
    for (const terrain::ChunkCoord& coord : needs) {
        if (record.pending_jobs.count(coord) != 0) {
            // Must also fix (Task 7 review): an edit (or a collider refresh)
            // already has this coord in flight -- it wins. LodTree already
            // marked the node in_flight before returning coord here, so
            // skipping the queue leaves nothing unaccounted: whatever is
            // actually in flight will land and settle the tree's residency
            // state regardless of which job asked for it.
            continue;
        }
        const std::uint64_t revision = ++next_job_revision_;
        record.chunk_revisions[coord] = revision;
        // Residency brings a render mesh back; the voxels did not change, so
        // a collider already known stays as it is (no physics churn) and
        // only one in interest but not known yet is built on the way.
        const bool want_collider =
            record.collider_interest.count(coord) != 0 && record.collider_ready.count(coord) == 0;
        record.pending_jobs[coord] = TerrainRecord::PendingJob{false, want_collider};
        tree.chunk_queued(coord, false);
        const float job_distance =
            has_camera ? distance(matrix4_point(transform, chunk_center_local(coord, voxel_size)), camera_pos) : 0.f;
        terrain::MeshInput input = terrain::mesh_input(volume, coord);
        input.build_collider = want_collider;
        mesher_.queue(terrain_id, revision, std::move(input), job_distance);
    }

    std::vector<terrain::NodeBuildRequest> builds;
    {
        PROFILE_SCOPE("Terrain next builds", profiler::Group::Engine);
        tree.next_builds(now_ms, camera, builds);
    }
    if (builds.empty()) {
        return;
    }
    // Spec decision 4: a snapshot of the chunk map (a copy of pointers to
    // immutable chunks) for the node jobs, taken only when one is queued and
    // kept until the voxels change: copying a huge Terrain's map (tens of
    // thousands of entries) every update was most of an update.
    if (record.voxels_snapshot == nullptr || record.voxels_snapshot_revision != volume.revision()) {
        PROFILE_SCOPE("Terrain voxel snapshot", profiler::Group::Engine);
        record.voxels_snapshot = std::make_shared<const terrain::ChunkMap>(volume.chunks());
        record.voxels_snapshot_revision = volume.revision();
    }
    const std::shared_ptr<const terrain::ChunkMap>& voxels = record.voxels_snapshot;
    for (terrain::NodeBuildRequest& build : builds) {
        float job_distance = 0.f;
        if (has_camera) {
            Vec3 min, max;
            terrain::node_bounds(build.input.key, voxel_size, min, max);
            const Vec3 center{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
            job_distance = distance(matrix4_point(transform, center), camera_pos);
        }
        build.input.voxels = voxels;
        mesher_.queue_node(terrain_id, build.revision, std::move(build.input), job_distance);
    }
}

void TerrainWorld::update_collider_interest(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record,
                                            double now_ms) {
    // Chunks that left the interest were last asked for at the previous
    // update, the last one that saw them in it.
    for (const terrain::ChunkCoord& coord : record.collider_interest_left) {
        const auto stamp = record.collider_last_interest_ms.find(coord);
        if (stamp != record.collider_last_interest_ms.end()) {
            stamp->second = record.collider_previous_update_ms;
        }
    }
    record.collider_interest_left.clear();
    record.collider_previous_update_ms = now_ms;
    std::vector<terrain::ChunkCoord> to_walk;
    to_walk.swap(record.collider_to_walk);
    // Nothing new to look at and no release due yet.
    if (to_walk.empty() && now_ms >= record.collider_release_scan_ms && now_ms - record.collider_release_scan_ms < kColliderReleaseMs * 0.25) {
        return;
    }
    PROFILE_SCOPE("Terrain interest walk", profiler::Group::Engine);
    for (const terrain::ChunkCoord& coord : to_walk) {
        if (record.collider_interest.count(coord) == 0) {
            continue;   // left the interest again before this update
        }
        record.collider_last_interest_ms[coord] = now_ms;
        if (record.collider_ready.count(coord) != 0 || record.pending_jobs.count(coord) != 0) {
            // Already known (a collider, or meshed with none to make -- most
            // of a body's box is air or buried, and re-meshing those every
            // update starved the queue of the chunks a walking body needed
            // next), or an edit/residency/earlier refresh job already in
            // flight for it will decide this once it lands -- that job's
            // own want_collider already follows interest.
            continue;
        }
        if (!stored_near(terrain.volume(), coord)) {
            // Empty space (a body far below or beside the island): nothing
            // to collide with, known without a job or an LOD tree node.
            record.collider_ready.insert(coord);
            continue;
        }
        const std::uint64_t revision = ++next_job_revision_;
        record.chunk_revisions[coord] = revision;
        record.pending_jobs[coord] = TerrainRecord::PendingJob{false, true};
        record.tree->chunk_queued(coord, false);
        terrain::MeshInput input = terrain::mesh_input(terrain.volume(), coord);
        input.build_collider = true;
        mesher_.queue(terrain_id, revision, std::move(input), 0.f);
    }
    // A collider not asked for in kColliderReleaseMs goes.
    if (now_ms >= record.collider_release_scan_ms && now_ms - record.collider_release_scan_ms < kColliderReleaseMs * 0.25) {
        return;
    }
    record.collider_release_scan_ms = now_ms;
    for (auto it = record.collider_last_interest_ms.begin(); it != record.collider_last_interest_ms.end();) {
        if (now_ms - it->second > kColliderReleaseMs && record.collider_interest.count(it->first) == 0) {
            if (record.collider_map.erase(it->first) != 0) {
                record.chunks_dirty = true;
            }
            record.collider_ready.erase(it->first);
            it = record.collider_last_interest_ms.erase(it);
        } else {
            ++it;
        }
    }
}

void TerrainWorld::rebuild_look(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, bool force) {
    std::array<LookInput, 256> candidate{};   // index 0 stays default: the engine default material
    for (TerrainMaterial* entry : terrain.materials()) {
        const int id = entry->material_id();
        if (id < 1 || id > 255) {
            continue;   // defensive: Terrain::materials() already filters to 1..255
        }
        const InstanceId material_id = entry->material_instance();
        const auto* asset = material_id != 0 ? dynamic_cast<const Material*>(terrain.instance(material_id)) : nullptr;
        if (asset == nullptr) {
            continue;   // nil or dead Material: this Id keeps the default, LookInput{}
        }
        const int layer = textures_ != nullptr ? textures_->layer_of(terrain_id, material_id) : 0;
        candidate[static_cast<std::size_t>(id)] =
            LookInput{material_id,          asset->color(),           asset->metalness(), asset->roughness(),
                      asset->reflectivity(), asset->texture_scale(), asset->blend_sharpness(),
                      asset->height_strength(), layer};
    }
    if (!force && candidate == record.look_inputs) {
        return;   // nothing a TerrainMaterial, its Material, that Material's numbers, or its layer changed
    }
    record.look_inputs = candidate;
    auto look = std::make_shared<TerrainLook>();
    look->revision = next_look_revision_++;
    for (int id = 0; id < 256; ++id) {
        const LookInput& input = candidate[static_cast<std::size_t>(id)];
        const ColorRgb color = input.material != 0 ? input.color : Material::kDefaultColor;
        const double metalness = input.material != 0 ? input.metalness : Material::kDefaultMetalness;
        const double roughness = input.material != 0 ? input.roughness : Material::kDefaultRoughness;
        const double reflectivity = input.material != 0 ? input.reflectivity : Material::kDefaultReflectivity;
        const double texture_scale = input.material != 0 ? input.texture_scale : Material::kDefaultTextureScale;
        const double blend_sharpness =
            input.material != 0 ? input.blend_sharpness : Material::kDefaultBlendSharpness;
        const double height_strength =
            input.material != 0 ? input.height_strength : Material::kDefaultHeightStrength;
        const std::size_t row0 = static_cast<std::size_t>(id) * 4;
        const std::size_t row1 = 256 * 4 + static_cast<std::size_t>(id) * 4;
        const std::size_t row2 = 2 * 256 * 4 + static_cast<std::size_t>(id) * 4;
        look->texels[row0 + 0] = static_cast<float>(color.r);
        look->texels[row0 + 1] = static_cast<float>(color.g);
        look->texels[row0 + 2] = static_cast<float>(color.b);
        look->texels[row0 + 3] = static_cast<float>(color.a);
        look->texels[row1 + 0] = static_cast<float>(metalness);
        look->texels[row1 + 1] = static_cast<float>(roughness);
        look->texels[row1 + 2] = static_cast<float>(reflectivity);
        look->texels[row1 + 3] = 1.f;
        look->texels[row2 + 0] = static_cast<float>(input.material != 0 ? input.layer : 0);
        look->texels[row2 + 1] = static_cast<float>(texture_scale);
        look->texels[row2 + 2] = static_cast<float>(blend_sharpness);
        look->texels[row2 + 3] = static_cast<float>(height_strength);
        // row 3 stays 0: default std::array<float,...>{} zero-initializes it.
    }
    record.look = std::move(look);
}

void TerrainWorld::publish_chunks(TerrainRecord& record) {
    auto chunks = std::make_shared<std::vector<TerrainChunkView>>();
    chunks->reserve(record.meshes.size());
    for (const auto& [coord, view] : record.meshes) {
        (void)coord;
        chunks->push_back(view);
    }
    record.chunks = std::move(chunks);
    // World-wide, not a local increment: see the comment on
    // TerrainRecord::chunks_revision.
    record.chunks_revision = ++next_chunks_set_revision_;
    record.colliders_vec.clear();
    record.colliders_vec.reserve(record.collider_map.size());
    for (const auto& [coord, collider] : record.collider_map) {
        (void)coord;
        record.colliders_vec.push_back(collider);
    }
    record.chunks_dirty = false;
}

void TerrainWorld::update(DataModel& game) {
    const double now_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    update(game, now_ms);
}

void TerrainWorld::update(DataModel& game, double now_ms) {
    // SimulationThread, under game's write lock (the Engine's contract).
    std::vector<terrain::MeshResult> results;
    std::vector<terrain::NodeResult> node_results;
    mesher_.collect(results, node_results);
    for (const terrain::MeshResult& result : results) {
        accept_result(result);
    }
    for (const terrain::NodeResult& result : node_results) {
        const auto found = terrains_.find(static_cast<InstanceId>(result.terrain));
        if (found != terrains_.end() && found->second.tree != nullptr) {
            // The tree drops one from an older stay or revision.
            if (result.failed) {
                found->second.tree->node_failed(result);
            } else {
                found->second.tree->node_built(result);
            }
        }
    }

    // Camera distance for job ordering, as AudioWorld::place_listener reads
    // the same Workspace/Camera pair; no camera orders every job at 0.
    const auto* workspace = dynamic_cast<const Workspace*>(game.instance(game.service("Workspace")));
    const InstanceId camera_id = workspace != nullptr ? workspace->current_camera() : 0;
    const auto* eye = camera_id != 0 ? dynamic_cast<const PVInstance*>(game.instance(camera_id)) : nullptr;
    const bool has_camera = eye != nullptr;
    const Vec3 camera_pos = has_camera ? matrix4_position(eye->transform()) : Vec3{};

    std::vector<InstanceId> current;
    game.terrains(current);

    // A Terrain that left Workspace (or was destroyed) drops its record;
    // returning later re-queues everything (first_seen below).
    for (auto it = terrains_.begin(); it != terrains_.end();) {
        if (std::find(current.begin(), current.end(), it->first) == current.end()) {
            it = terrains_.erase(it);
        } else {
            ++it;
        }
    }

    views_.clear();
    views_.reserve(current.size());
    for (InstanceId id : current) {
        auto* terrain = dynamic_cast<Terrain*>(game.instance(id));
        if (terrain == nullptr) {
            continue;   // terrain() lied, or the instance died between the query and here
        }
        const auto [record_it, first_seen] = terrains_.try_emplace(id);
        TerrainRecord& record = record_it->second;

        // A new tree (first sight, or the voxel size changed under the old
        // one) starts from nothing: every chunk is queued again.
        const float voxel_size = terrain->volume().voxel_size();
        bool fresh = first_seen;
        if (record.tree == nullptr || record.tree->voxel_size() != voxel_size) {
            record.tree = std::make_unique<terrain::LodTree>(voxel_size, &next_node_revision_);
            record.pending_jobs.clear();
            record.collider_ready.clear();   // every chunk is meshed again at the new size
            record.collider_to_walk.assign(record.collider_interest.begin(), record.collider_interest.end());
            record.batches.clear();  // their jobs' results no longer match (every chunk is queued again)
            record.chunk_batch.clear();
            record.sync_built.clear();
            // Colliders were triangulated at the old size too; PhysicsWorld
            // must not keep handing them out until each chunk is re-walked.
            const bool had_colliders = !record.collider_map.empty();
            record.collider_map.clear();
            if (!record.meshes.empty()) {
                record.meshes.clear();   // made at the old voxel size
                record.chunks_dirty = true;
            }
            if (had_colliders) {
                record.chunks_dirty = true;   // publish_chunks rebuilds colliders_vec from the (now empty) map
            }
            fresh = true;
        }
        // R31: after this update's results landed and before its edits
        // queue, a batch past its hold publishes what has landed.
        {
            PROFILE_SCOPE("Terrain batches", profiler::Group::Engine);
            expire_batches(record, now_ms);
        }
        const std::uint64_t key = terrain->content_key();
        if (!fresh && key != 0 && key != record.saved_key) {
            // Saved since the last update: the file now holds the voxels as
            // they were then. An edit queued below (made after, or not yet
            // taken) marks them changed again. (0 means only that they
            // changed, which edited_since_save already tracks.)
            record.saved_key = key;
            record.edited_since_save = false;
            record.store_committed = false;
        }
        record.tree->set_store_writes(!record.edited_since_save);
        {
            PROFILE_SCOPE("Terrain queue", profiler::Group::Engine);
            queue_dirty(id, *terrain, record, fresh, has_camera, camera_pos, now_ms);
            admit_first_build(id, *terrain, record, has_camera, camera_pos);
        }
        {
            PROFILE_SCOPE("Terrain LOD", profiler::Group::Engine);
            update_lod(id, *terrain, record, now_ms, has_camera, camera_pos);
            if (!has_camera || now_ms - record.store_ms >= kStoreCheckIntervalMs) {
                record.store_ms = now_ms;
                update_store(*terrain, record);
            }
        }
        if (!terrain->can_collide()) {
            // Nothing to show through it: let any collider interest lapse
            // (set_collider_interest while CanCollide is false would only
            // build colliders no one can touch) and release what exists
            // once its kColliderReleaseMs is up, same as leaving interest.
            record.collider_interest.clear();
        }
        {
            PROFILE_SCOPE("Terrain colliders", profiler::Group::Engine);
            update_collider_interest(id, *terrain, record, now_ms);
        }
        rebuild_look(id, *terrain, record, first_seen);
        if (record.chunks_dirty || first_seen) {
            publish_chunks(record);
        }
        // Rebuilt at most every kNodeListIntervalMs (the tree's changed flag
        // waits until then), since the list covers every node.
        const bool list_due = !has_camera || now_ms - record.nodes_ms >= kNodeListIntervalMs;
        if (fresh || (list_due && record.tree->take_changed())) {
            PROFILE_SCOPE("Terrain node list", profiler::Group::Engine);
            if (fresh) {
                record.tree->take_changed();
            }
            record.nodes = std::make_shared<const std::vector<TerrainNodeView>>(record.tree->nodes_for_view());
            record.nodes_revision = ++next_nodes_set_revision_;
            record.nodes_ms = now_ms;
        }

        TerrainView view;
        view.terrain = id;
        view.transform = terrain->transform();
        view.can_collide = terrain->can_collide();
        view.chunks = record.chunks;
        view.chunks_revision = record.chunks_revision;
        view.look = record.look;
        view.nodes = record.nodes;
        view.nodes_revision = record.nodes_revision;
        view.top_level = record.tree->top_level();
        views_.push_back(std::move(view));
    }

    // memory()'s figures: a walk over every Terrain's meshes and nodes, so
    // only every so many updates.
    if (--memory_countdown_ <= 0) {
        memory_countdown_ = kMemoryRefreshUpdates;
        std::unordered_map<InstanceId, TerrainMemory> figures;
        for (const auto& [id, record] : terrains_) {
            TerrainMemory& memory = figures[id];
            for (const auto& [coord, chunk] : record.meshes) {
                (void)coord;
                if (chunk.mesh != nullptr) {
                    memory.chunk_meshes += chunk.mesh->vertices.size() * sizeof(anarchy::amesh::Vertex) +
                                           chunk.mesh->indices.size() * sizeof(std::uint32_t);
                }
            }
            memory.far_meshes = record.tree != nullptr ? record.tree->compact_bytes() : 0;
        }
        std::lock_guard<std::mutex> lock(memory_mutex_);
        memory_ = std::move(figures);
    }
}

TerrainMemory TerrainWorld::memory(InstanceId terrain) const {
    TerrainMemory out;
    {
        std::lock_guard<std::mutex> lock(memory_mutex_);
        const auto found = memory_.find(terrain);
        if (found != memory_.end()) {
            out = found->second;
        }
    }
    const terrain::ChunkCache& cache = terrain::ChunkCache::global();
    out.compressed_voxels = cache.compressed_bytes();
    out.decoded_cache = cache.bytes();
    out.decoded_budget = cache.budget();
    return out;
}

const std::vector<TerrainWorld::ChunkCollider>* TerrainWorld::colliders(InstanceId terrain) const {
    const auto found = terrains_.find(terrain);
    return found != terrains_.end() ? &found->second.colliders_vec : nullptr;
}

void TerrainWorld::set_collider_interest(InstanceId terrain, std::vector<terrain::ChunkCoord> chunks) {
    // PhysicsWorld, at the start of its own sync. Replaces whatever the
    // previous call (this frame's or an earlier one) asked for; takes effect
    // at the next update() (update_collider_interest).
    const auto found = terrains_.find(terrain);
    if (found == terrains_.end()) {
        return;   // not a Terrain this TerrainWorld currently shows
    }
    TerrainRecord& record = found->second;
    std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> next(chunks.begin(), chunks.end());
    if (next == record.collider_interest) {
        return;   // the same ask: nothing for update_collider_interest to walk again
    }
    for (const terrain::ChunkCoord& coord : record.collider_interest) {
        if (next.count(coord) == 0) {
            record.collider_interest_left.push_back(coord);   // asked for until now
        }
    }
    record.collider_interest = std::move(next);
    record.collider_to_walk.assign(record.collider_interest.begin(), record.collider_interest.end());
}

bool TerrainWorld::build_colliders_now(DataModel& game, InstanceId terrain,
                                       const std::vector<terrain::ChunkCoord>& chunks) {
    // PhysicsWorld's own sync, SimulationThread: no-fall-through for the
    // chunks under a body whose collider is not known yet. Meshes and builds right
    // here, off the job queue -- apply_result still runs, so the LOD tree,
    // chunk_revisions, and pending_jobs all stay consistent with a job
    // queued and landed through the normal path for the same coord.
    const auto found = terrains_.find(terrain);
    if (found == terrains_.end()) {
        return false;
    }
    // Looked up afresh: the Terrain may have been destroyed since the last
    // update(), which is when this record last saw it.
    auto* instance = dynamic_cast<Terrain*>(game.instance(terrain));
    if (instance == nullptr) {
        return false;
    }
    TerrainRecord& record = found->second;
    terrain::VoxelVolume& volume = instance->volume();
    bool ok = true;
    for (const terrain::ChunkCoord& coord : chunks) {
        if (record.collider_ready.count(coord) != 0) {
            continue;   // already known: built, or meshed with nothing to collide with
        }
        const auto pending = record.pending_jobs.find(coord);
        if (pending != record.pending_jobs.end() && pending->second.edited) {
            // An edit already in flight for this coord keeps its job (its
            // batch waits on that result, so it must not go stale here).
            // The body still needs ground this sync: build the collider
            // alone from the voxels as they are now (the edit is already in
            // them) and leave the mesh, the tree and the job to the edit's
            // own landing, which writes its collider again if still wanted.
            // Not marked ready: that is the landing's to decide. Built once
            // (sync_built), not again every sync until the edit lands --
            // even when there was nothing to collide with.
            if (record.collider_map.count(coord) != 0 || record.sync_built.count(coord) != 0) {
                continue;   // the one from before the edit, or the one built here, stands until the edit lands
            }
            try {
                ++sync_meshed_count_;
                const terrain::ChunkMesh mesh = terrain::surface_nets(terrain::mesh_input(volume, coord));
                if (build_collider_ && mesh.render != nullptr) {
                    if (auto collider = build_collider_(mesh)) {
                        record.collider_map[coord] = ChunkCollider{coord, next_chunk_revision_++, std::move(collider)};
                        record.chunks_dirty = true;
                    }
                }
                record.sync_built.insert(coord);
            } catch (...) {
                ok = false;   // nothing built; the edit's landing still decides
            }
            continue;
        }
        if (!stored_near(volume, coord)) {
            // Empty space (a body far below or beside the island): nothing
            // to collide with, known without meshing or an LOD tree node.
            // A job in flight for it (none can carry an edit here) still
            // lands as usual.
            record.collider_ready.insert(coord);
            continue;
        }
        // Any other job in flight for coord (residency, or an earlier
        // collider-interest refresh that has not landed yet) carries no
        // voxel change, so superseding it here (the revision bump below
        // makes its eventual result stale, same as queue_dirty replacing a
        // queued job elsewhere) is safe -- and necessary: the body needs a
        // collider in THIS sync, not whenever that worker job happens to
        // finish. Leaving it to land on its own was the no-fall-through gap
        // CS4 found: a walking body can cross into new territory, find
        // nothing covering it, but a job already queued (not yet landed)
        // for that exact coord meant this call did nothing, and physics
        // kept stepping underneath it in the meantime.
        const std::uint64_t revision = ++next_job_revision_;
        record.chunk_revisions[coord] = revision;
        record.pending_jobs[coord] = TerrainRecord::PendingJob{false, true};
        if (record.tree != nullptr) {
            record.tree->chunk_queued(coord, false);
        }
        terrain::MeshResult result;
        result.terrain = static_cast<std::uint64_t>(terrain);
        result.coord = coord;
        result.revision = revision;
        try {
            PROFILE_SCOPE("Terrain sync collider", profiler::Group::Physics);
            ++sync_meshed_count_;
            terrain::MeshInput input = terrain::mesh_input(volume, coord);
            input.build_collider = true;
            result.mesh = terrain::surface_nets(input);
            if (build_collider_ && result.mesh.render != nullptr) {
                result.collider = build_collider_(result.mesh);
            }
        } catch (...) {
            // Same contract as a mesher worker's job: a throw never escapes;
            // the chunk is simply not built this call (it keeps whatever it
            // had, which is nothing new here since it had no collider).
            result.failed = true;
            ok = false;
        }
        apply_result(record, result);
    }
    if (record.chunks_dirty) {
        PROFILE_SCOPE("Terrain publish chunks", profiler::Group::Physics);
        publish_chunks(record);
        // views() is a snapshot taken at the last update(): PhysicsWorld's
        // own sync calls this between that update() and its own
        // reconcile_terrain (same sync, no falling through), which reads
        // views() for chunks_revision. Patch the live entry so it sees this
        // call's new one, not next update()'s.
        for (TerrainView& view : views_) {
            if (view.terrain == terrain) {
                view.chunks = record.chunks;
                view.chunks_revision = record.chunks_revision;
                break;
            }
        }
    }
    return ok;
}

void TerrainWorld::attach_textures(const TerrainTextures& textures) {
    // The Engine, right after textures.update(game). views_ already reflects
    // this update()'s Terrains; each view just gets handed the latest set
    // its own TerrainTextures record has (null before its first publish).
    for (TerrainView& view : views_) {
        view.textures = textures.published(view.terrain);
    }
}

void TerrainWorld::change_collider_interest(InstanceId terrain, const std::vector<terrain::ChunkCoord>& added,
                                            const std::vector<terrain::ChunkCoord>& removed) {
    const auto found = terrains_.find(terrain);
    if (found == terrains_.end()) {
        return;   // not a Terrain this TerrainWorld currently shows
    }
    TerrainRecord& record = found->second;
    for (const terrain::ChunkCoord& coord : removed) {
        if (record.collider_interest.erase(coord) != 0) {
            record.collider_interest_left.push_back(coord);   // asked for until now
        }
    }
    for (const terrain::ChunkCoord& coord : added) {
        if (record.collider_interest.insert(coord).second) {
            record.collider_to_walk.push_back(coord);
        }
    }
}

bool TerrainWorld::has_collider_interest(InstanceId terrain) const {
    const auto found = terrains_.find(terrain);
    return found != terrains_.end() && !found->second.collider_interest.empty();
}

}  // namespace engine_core
