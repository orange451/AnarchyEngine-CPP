#pragma once

// The .alod file beside a Terrain's .avox: its built LOD nodes (level >= 2)
// as CompactMeshes, so a far node can be dropped from RAM and read back, and
// a later open can show the whole island without meshing every chunk.
//
// Layout, little-endian:
//   header (40 bytes): "ALOD", u32 version, u64 content_key (the .avox
//     bytes' content_key_of), f32 voxel_size, u32 reserved, u64 offset of
//     the committed footer, u64 content_key_of(footer bytes).
//   records, appended: i32 level, x, y, z; f32 error; f32 x6 bounds; u32
//     surface_index_count; f32 x6 origin, scale; u32 vertices, indices16,
//     indices32; then positions (u16 x3 per vertex), normals (u8 x2), ids
//     (u8 x4), weights (u8 x4), indices16, indices32.
//   footer: u32 entry count; per entry i32 level, x, y, z, u64 offset, u32
//     size, f32 error, f32 x6 bounds; u32 surface chunk count; per chunk
//     i32 x, y, z.
// Records and footers are only ever appended; commit() writes a footer at
// the end and then points the header at it. Records put after the last
// commit are invisible to a reopen, so a store cut short (Studio stopped
// mid-build) still reads as its last commit. A header, footer or record
// that does not check out makes open() fail: the caller then builds the
// nodes from voxels and makes a new store.
//
// SimulationThread only (TerrainWorld): not thread-safe.

#include "Vector3.hpp"
#include "terrain/LodNode.hpp"
#include "terrain/VoxelChunk.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace engine_core::terrain {

struct AlodEntry {
    NodeKey key;
    std::uint64_t offset = 0;   // the record's first byte
    std::uint32_t size = 0;     // the record's bytes
    float error = 0.f;
    Vec3 bounds_min{}, bounds_max{};
};

class AlodStore {
public:
    // path as a valid store of (content_key, voxel_size), at its last commit;
    // nullopt if it is missing, damaged, or for other voxels.
    static std::optional<AlodStore> open(const std::filesystem::path& path, std::uint64_t content_key,
                                         float voxel_size);
    // An empty, committed store at path, replacing any file there; nullopt
    // when it cannot be written.
    static std::optional<AlodStore> create(const std::filesystem::path& path, std::uint64_t content_key,
                                           float voxel_size);

    AlodStore(AlodStore&&) = default;
    AlodStore& operator=(AlodStore&&) = default;

    // Every node put, committed or not, at its newest record.
    const std::unordered_map<NodeKey, AlodEntry, NodeKeyHash>& entries() const { return entries_; }
    const std::vector<ChunkCoord>& surface_chunks() const { return surface_chunks_; }
    // key's newest mesh, or null when absent or unreadable.
    std::shared_ptr<const CompactMesh> load(const NodeKey& key);
    // Appends key's record; the index points at it from now on.
    bool put(const NodeKey& key, const CompactMesh& mesh, float error, Vec3 bounds_min, Vec3 bounds_max);
    // The level-0 chunks with surface, written with the next commit.
    void set_surface_chunks(std::vector<ChunkCoord> coords) { surface_chunks_ = std::move(coords); }
    // Makes every put so far (and the surface chunks) what a reopen sees.
    bool commit();
    std::uint64_t content_key() const { return content_key_; }
    float voxel_size() const { return voxel_size_; }
    const std::filesystem::path& path() const { return path_; }
    // Bytes in the file: records (live and superseded), footers, header.
    std::uint64_t file_bytes() const { return end_; }
    // Bytes of the records entries() points at.
    std::uint64_t live_bytes() const;

private:
    AlodStore() = default;

    std::filesystem::path path_;
    std::unique_ptr<std::fstream> file_;
    std::uint64_t content_key_ = 0;
    float voxel_size_ = 1.f;
    std::uint64_t end_ = 0;   // where the next append goes
    std::unordered_map<NodeKey, AlodEntry, NodeKeyHash> entries_;
    std::vector<ChunkCoord> surface_chunks_;
};

// FNV-1a, 64-bit: an .avox file's content key. Never 0 for any input.
std::uint64_t content_key_of(const std::byte* data, std::size_t size);

}  // namespace engine_core::terrain
