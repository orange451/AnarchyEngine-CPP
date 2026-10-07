#include "TerrainWorld.hpp"

#include "AssetInstances.hpp"
#include "PVInstance.hpp"
#include "SceneService.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "terrain/VoxelVolume.hpp"

#include <algorithm>
#include <cmath>
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

}  // namespace

TerrainWorld::TerrainWorld(terrain::TerrainMesher::BuildCollider build, unsigned threads)
    : mesher_(std::move(build), threads) {}

void TerrainWorld::accept_result(const terrain::MeshResult& result) {
    // SimulationThread (called from update(), which owns terrains_).
    auto found = terrains_.find(static_cast<InstanceId>(result.terrain));
    if (found == terrains_.end()) {
        return;   // the Terrain left Workspace (or was destroyed) since this job was queued
    }
    TerrainRecord& record = found->second;
    const auto revision_it = record.chunk_revisions.find(result.coord);
    if (revision_it == record.chunk_revisions.end() || revision_it->second != result.revision) {
        // Stale: an older job than the chunk's current counter, or one for a
        // counter this record never issued (left over from a dropped-then-
        // reseen Terrain). Results can arrive out of order across workers,
        // so only an exact match on the live counter is accepted.
        return;
    }
    ++meshed_count_;
    const std::uint64_t view_revision = next_chunk_revision_++;
    if (result.mesh.render != nullptr) {
        record.meshes[result.coord] = TerrainChunkView{result.coord, view_revision, result.mesh.render};
    } else {
        record.meshes.erase(result.coord);
    }
    if (result.collider != nullptr) {
        record.collider_map[result.coord] = ChunkCollider{result.coord, view_revision, result.collider};
    } else {
        record.collider_map.erase(result.coord);
    }
    record.chunks_dirty = true;
}

void TerrainWorld::queue_dirty(InstanceId terrain_id, Terrain& terrain, TerrainRecord& record, bool first_seen,
                                bool has_camera, Vec3 camera_pos) {
    terrain::VoxelVolume& volume = terrain.volume();
    std::vector<terrain::ChunkCoord> dirty;
    if (first_seen) {
        // A Terrain seen for the first time (just arrived in Workspace, or
        // TerrainWorld itself just started): drain whatever take_dirty would
        // have returned -- it is about to be superseded -- then queue every
        // chunk the volume actually holds.
        std::vector<terrain::ChunkCoord> drained;
        volume.take_dirty(drained);
        dirty.reserve(volume.chunks().size());
        for (const auto& [coord, chunk] : volume.chunks()) {
            (void)chunk;
            dirty.push_back(coord);
        }
    } else {
        volume.take_dirty(dirty);
    }
    const float voxel_size = static_cast<float>(volume.voxel_size());
    for (const terrain::ChunkCoord& coord : dirty) {
        const std::uint64_t revision = ++record.chunk_revisions[coord];
        float job_distance = 0.f;
        if (has_camera) {
            const Vec3 world_center = matrix4_point(terrain.transform(), chunk_center_local(coord, voxel_size));
            job_distance = distance(world_center, camera_pos);
        }
        mesher_.queue(terrain_id, revision, terrain::mesh_input(volume, coord), job_distance);
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
    ++record.chunks_revision;
    record.colliders_vec.clear();
    record.colliders_vec.reserve(record.collider_map.size());
    for (const auto& [coord, collider] : record.collider_map) {
        (void)coord;
        record.colliders_vec.push_back(collider);
    }
    record.chunks_dirty = false;
}

void TerrainWorld::update(DataModel& game) {
    // SimulationThread, under game's write lock (the Engine's contract).
    std::vector<terrain::MeshResult> results;
    mesher_.collect(results);
    for (const terrain::MeshResult& result : results) {
        accept_result(result);
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

        queue_dirty(id, *terrain, record, first_seen, has_camera, camera_pos);
        rebuild_look(*terrain, record, first_seen);
        if (record.chunks_dirty || first_seen) {
            publish_chunks(record);
        }

        TerrainView view;
        view.terrain = id;
        view.transform = terrain->transform();
        view.can_collide = terrain->can_collide();
        view.chunks = record.chunks;
        view.chunks_revision = record.chunks_revision;
        view.look = record.look;
        views_.push_back(std::move(view));
    }
}

const std::vector<TerrainWorld::ChunkCollider>* TerrainWorld::colliders(InstanceId terrain) const {
    const auto found = terrains_.find(terrain);
    return found != terrains_.end() ? &found->second.colliders_vec : nullptr;
}

}  // namespace engine_core
