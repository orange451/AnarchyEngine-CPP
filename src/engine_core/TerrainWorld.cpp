#include "TerrainWorld.hpp"

#include "AssetInstances.hpp"
#include "PVInstance.hpp"
#include "SceneService.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "terrain/VoxelVolume.hpp"

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

std::uint8_t to_u8(float value) {
    const float clamped = std::clamp(value, 0.f, 1.f);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.f));
}

std::uint8_t to_u8(double value) { return to_u8(static_cast<float>(value)); }

// Task 8: a collider not asked for by set_collider_interest in this long
// goes (spec: "a few seconds", R8 fixes it at 5 s).
constexpr double kColliderReleaseMs = 5000.0;

// R31: an edit batch holds its landed results at most this long after its
// oldest edit, then publishes what has landed without waiting further.
constexpr double kEditBatchHoldMs = 150.0;

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
        dirty.assign(footprint.begin(), footprint.end());
    } else {
        volume.take_dirty(dirty);
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
    tree.update_residency(camera, drops, needs);
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
    tree.next_builds(now_ms, camera, builds);
    if (builds.empty()) {
        return;
    }
    // Spec decision 4: one snapshot of the chunk map (a copy of pointers to
    // immutable chunks) per update, and only when a node job is queued.
    const auto voxels = std::make_shared<const terrain::ChunkMap>(volume.chunks());
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
    for (const terrain::ChunkCoord& coord : record.collider_interest) {
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
        const std::uint64_t revision = ++next_job_revision_;
        record.chunk_revisions[coord] = revision;
        record.pending_jobs[coord] = TerrainRecord::PendingJob{false, true};
        record.tree->chunk_queued(coord, false);
        terrain::MeshInput input = terrain::mesh_input(terrain.volume(), coord);
        input.build_collider = true;
        mesher_.queue(terrain_id, revision, std::move(input), 0.f);
    }
    // A collider not asked for in kColliderReleaseMs goes.
    for (auto it = record.collider_last_interest_ms.begin(); it != record.collider_last_interest_ms.end();) {
        if (now_ms - it->second > kColliderReleaseMs) {
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

void TerrainWorld::rebuild_look(Terrain& terrain, TerrainRecord& record, bool force) {
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
        candidate[static_cast<std::size_t>(id)] =
            LookInput{material_id, asset->color(), asset->metalness(), asset->roughness(), asset->reflectivity()};
    }
    if (!force && candidate == record.look_inputs) {
        return;   // nothing a TerrainMaterial, its Material, or that Material's PBR values did changed
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
        const std::size_t row0 = static_cast<std::size_t>(id) * 4;
        const std::size_t row1 = 256 * 4 + static_cast<std::size_t>(id) * 4;
        look->texels[row0 + 0] = to_u8(color.r);
        look->texels[row0 + 1] = to_u8(color.g);
        look->texels[row0 + 2] = to_u8(color.b);
        look->texels[row0 + 3] = to_u8(color.a);
        look->texels[row1 + 0] = to_u8(metalness);
        look->texels[row1 + 1] = to_u8(roughness);
        look->texels[row1 + 2] = to_u8(reflectivity);
        look->texels[row1 + 3] = 255;
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
            record.batches.clear();  // their jobs' results no longer match (every chunk is queued again)
            record.chunk_batch.clear();
            if (!record.meshes.empty()) {
                record.meshes.clear();   // made at the old voxel size
                record.chunks_dirty = true;
            }
            fresh = true;
        }
        record.instance = terrain;   // for build_colliders_now, called from PhysicsWorld's own sync

        // R31: after this update's results landed and before its edits
        // queue, a batch past its hold publishes what has landed.
        expire_batches(record, now_ms);
        queue_dirty(id, *terrain, record, fresh, has_camera, camera_pos, now_ms);
        update_lod(id, *terrain, record, now_ms, has_camera, camera_pos);
        if (!terrain->can_collide()) {
            // Nothing to show through it: let any collider interest lapse
            // (set_collider_interest while CanCollide is false would only
            // build colliders no one can touch) and release what exists
            // once its kColliderReleaseMs is up, same as leaving interest.
            record.collider_interest.clear();
        }
        update_collider_interest(id, *terrain, record, now_ms);
        rebuild_look(*terrain, record, first_seen);
        if (record.chunks_dirty || first_seen) {
            publish_chunks(record);
        }
        if (record.tree->take_changed() || fresh) {
            record.nodes = std::make_shared<const std::vector<TerrainNodeView>>(record.tree->nodes_for_view());
            record.nodes_revision = ++next_nodes_set_revision_;
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
    found->second.collider_interest.clear();
    found->second.collider_interest.insert(chunks.begin(), chunks.end());
}

bool TerrainWorld::build_colliders_now(InstanceId terrain, const std::vector<terrain::ChunkCoord>& chunks) {
    // PhysicsWorld's own sync, SimulationThread: no-fall-through for the
    // chunks under a body whose collider is not known yet. Meshes and builds right
    // here, off the job queue -- apply_result still runs, so the LOD tree,
    // chunk_revisions, and pending_jobs all stay consistent with a job
    // queued and landed through the normal path for the same coord.
    const auto found = terrains_.find(terrain);
    if (found == terrains_.end() || found->second.instance == nullptr) {
        return false;
    }
    TerrainRecord& record = found->second;
    terrain::VoxelVolume& volume = record.instance->volume();
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
            // Not marked ready: that is the landing's to decide.
            if (record.collider_map.count(coord) != 0) {
                continue;   // the one from before the edit stands until the edit lands
            }
            try {
                const terrain::ChunkMesh mesh = terrain::surface_nets(terrain::mesh_input(volume, coord));
                if (build_collider_ && mesh.render != nullptr) {
                    if (auto collider = build_collider_(mesh)) {
                        record.collider_map[coord] = ChunkCollider{coord, next_chunk_revision_++, std::move(collider)};
                        record.chunks_dirty = true;
                    }
                }
            } catch (...) {
                ok = false;   // nothing built; the edit's landing still decides
            }
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

}  // namespace engine_core
