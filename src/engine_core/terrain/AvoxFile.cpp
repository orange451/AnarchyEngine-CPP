#include "terrain/AvoxFile.hpp"

#include "terrain/ChunkFrame.hpp"

// CMake 3.16's Visual Studio generator does not pass SYSTEM include
// directories to MSVC, so zstd.h can still warn at /W4 even though
// cmake/zstd/CMakeLists.txt marks its include directory SYSTEM. Only this
// file includes zstd.h.
#pragma warning(push, 0)
#include <zstd.h>
#pragma warning(pop)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

namespace engine_core::terrain {
namespace {

constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kIndexEntrySize = 32;
constexpr std::size_t kFrameContentSize = 2 * static_cast<std::size_t>(kChunkCells);

// Little-endian field writers and readers: explicit byte shifts, since
// MSVC 14.23 has neither std::endian nor std::bit_cast to do this for us.
void put_u8(std::byte* at, std::uint8_t v) { at[0] = std::byte{v}; }
void put_u16(std::byte* at, std::uint16_t v) {
    at[0] = std::byte{static_cast<std::uint8_t>(v & 0xffu)};
    at[1] = std::byte{static_cast<std::uint8_t>((v >> 8) & 0xffu)};
}
void put_u32(std::byte* at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        at[i] = std::byte{static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu)};
    }
}
void put_i32(std::byte* at, std::int32_t v) { put_u32(at, static_cast<std::uint32_t>(v)); }
void put_u64(std::byte* at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        at[i] = std::byte{static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu)};
    }
}
void put_f32(std::byte* at, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(at, bits);
}

std::uint8_t get_u8(const std::byte* at) { return static_cast<std::uint8_t>(at[0]); }
std::uint16_t get_u16(const std::byte* at) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(get_u8(at)) |
                                       (static_cast<unsigned>(get_u8(at + 1)) << 8));
}
std::uint32_t get_u32(const std::byte* at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(get_u8(at + i)) << (8 * i);
    }
    return v;
}
std::int32_t get_i32(const std::byte* at) { return static_cast<std::int32_t>(get_u32(at)); }
std::uint64_t get_u64(const std::byte* at) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(get_u8(at + i)) << (8 * i);
    }
    return v;
}
float get_f32(const std::byte* at) {
    const std::uint32_t bits = get_u32(at);
    float v = 0.f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// The standard reflected CRC-32 (the zlib/PNG polynomial), bit by bit: an
// index is at most a few hundred KB even for a huge island, so a table buys
// nothing worth the extra code.
std::uint32_t crc32_of(const std::byte* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint8_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xedb88320u : (crc >> 1);
        }
    }
    return ~crc;
}

// The 3D Lorenzo prediction of cell i = cell_index(x, y, z)'s distance from
// its already-written neighbors; a neighbor outside the chunk counts as 0.
// Takes whether each axis is already at 0, hoisted by the caller's loop
// (z0/y0 only change once every 1,024/32 cells), so every neighbor is a
// plain pointer offset from i rather than a fresh cell_index() multiply for
// each of the 7 terms.
int predict_ptr(const Cell* cells, int i, bool x0, bool y0, bool z0) {
    const int dx = x0 ? 0 : cells[i - 1].distance;
    const int dy = y0 ? 0 : cells[i - kChunkSize].distance;
    const int dz = z0 ? 0 : cells[i - kChunkSize * kChunkSize].distance;
    const int dxy = (x0 || y0) ? 0 : cells[i - 1 - kChunkSize].distance;
    const int dxz = (x0 || z0) ? 0 : cells[i - 1 - kChunkSize * kChunkSize].distance;
    const int dyz = (y0 || z0) ? 0 : cells[i - kChunkSize - kChunkSize * kChunkSize].distance;
    const int dxyz = (x0 || y0 || z0) ? 0 : cells[i - 1 - kChunkSize - kChunkSize * kChunkSize].distance;
    return dx + dy + dz - dxy - dxz - dyz + dxyz;
}

// The same predictor, reading a chunk already being decoded through its
// dense_at() accessor (a plain inlined array read) instead of a raw
// pointer, since a decoding chunk has no pointer of its own to hand out.
int predict_chunk(const ChunkData& chunk, int i, bool x0, bool y0, bool z0) {
    const int dx = x0 ? 0 : chunk.dense_at(i - 1).distance;
    const int dy = y0 ? 0 : chunk.dense_at(i - kChunkSize).distance;
    const int dz = z0 ? 0 : chunk.dense_at(i - kChunkSize * kChunkSize).distance;
    const int dxy = (x0 || y0) ? 0 : chunk.dense_at(i - 1 - kChunkSize).distance;
    const int dxz = (x0 || z0) ? 0 : chunk.dense_at(i - 1 - kChunkSize * kChunkSize).distance;
    const int dyz = (y0 || z0) ? 0 : chunk.dense_at(i - kChunkSize - kChunkSize * kChunkSize).distance;
    const int dxyz = (x0 || y0 || z0) ? 0 : chunk.dense_at(i - 1 - kChunkSize - kChunkSize * kChunkSize).distance;
    return dx + dy + dz - dxy - dxz - dyz + dxyz;
}

// Decodes one dense chunk's frame straight into chunk (already a dense
// clone): false on any damage (wrong content size, a zstd checksum failure,
// or short output). Any thread: chunk belongs only to the caller until it
// joins and publishes it.
bool decode_chunk_frame(const std::byte* frame, std::size_t frame_size, ChunkData& chunk) {
    if (ZSTD_getFrameContentSize(frame, frame_size) != static_cast<unsigned long long>(kFrameContentSize)) {
        return false;
    }
    std::vector<std::uint8_t> payload(kFrameContentSize);
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (dctx == nullptr) {
        return false;  // out of memory or similar: treat like any other damage
    }
    const std::size_t written = ZSTD_decompressDCtx(dctx, payload.data(), payload.size(), frame, frame_size);
    ZSTD_freeDCtx(dctx);
    if (ZSTD_isError(written) || written != payload.size()) {
        return false;
    }
    // Residuals first, in the same z,y,x order they were predicted in: each
    // cell's prediction only reads neighbors already restored this way.
    // Reading and writing chunk through dense_at()/set_dense_at() (plain
    // array accessors) rather than through a separate scratch buffer that
    // would need copying into the chunk afterward.
    for (int z = 0; z < kChunkSize; ++z) {
        const bool z0 = (z == 0);
        for (int y = 0; y < kChunkSize; ++y) {
            const bool y0 = (y == 0);
            int i = cell_index(0, y, z);
            for (int x = 0; x < kChunkSize; ++x, ++i) {
                const int p = predict_chunk(chunk, i, x == 0, y0, z0);
                const std::uint8_t residual = payload[static_cast<std::size_t>(i)];
                // The 8-bit wrap happens through std::uint8_t then a cast to
                // std::int8_t, so it is exact regardless of p's sign.
                chunk.set_dense_at(
                    i, Cell{static_cast<std::int8_t>(static_cast<std::uint8_t>(p + residual)), 0});
            }
        }
    }
    for (int i = 0; i < kChunkCells; ++i) {
        Cell c = chunk.dense_at(i);
        c.material = payload[static_cast<std::size_t>(kChunkCells + i)];
        chunk.set_dense_at(i, c);
    }
    return true;
}

// One index entry's fields, decoded from the file's own little-endian bytes.
struct IndexEntry {
    ChunkCoord coord;
    std::uint8_t form = 0;     // 0 uniform, 1 dense
    std::int8_t distance = 0;  // uniform only
    std::uint8_t id = 0;       // uniform only
    std::uint64_t offset = 0;  // dense only
    std::uint32_t size = 0;    // dense only
};

bool by_zyx(const std::pair<ChunkCoord, ChunkPtr>& a, const std::pair<ChunkCoord, ChunkPtr>& b) {
    if (a.first.z != b.first.z) {
        return a.first.z < b.first.z;
    }
    if (a.first.y != b.first.y) {
        return a.first.y < b.first.y;
    }
    return a.first.x < b.first.x;
}

// Joins every thread it holds in its own destructor. Without this, an
// exception thrown while more workers are still being started (std::thread's
// constructor throws std::system_error if the OS can't start one) would
// unwind through a plain std::vector<std::thread> that still holds earlier,
// successfully-started, still-joinable threads: its destructor calls
// std::terminate on any one of them. Being destroyed during that same
// unwind, this joins them all first instead.
struct JoiningThreads {
    std::vector<std::thread> threads;
    ~JoiningThreads() {
        for (std::thread& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
};

}  // namespace

// ChunkFrame.hpp's declaration: the only part of the frame codec visible
// outside this file, so ChunkData::encoded() (in VoxelChunk.cpp) can call it
// without VoxelChunk.cpp including zstd.h itself.
std::vector<std::byte> encode_chunk_frame(const Cell* cells) {
    std::vector<std::uint8_t> payload(kFrameContentSize);
    for (int z = 0; z < kChunkSize; ++z) {
        const bool z0 = (z == 0);
        for (int y = 0; y < kChunkSize; ++y) {
            const bool y0 = (y == 0);
            int i = cell_index(0, y, z);
            for (int x = 0; x < kChunkSize; ++x, ++i) {
                const int p = predict_ptr(cells, i, x == 0, y0, z0);
                // residual = (d - p) mod 256; wraps through unsigned
                // arithmetic so decoding (which adds p back) is exact.
                payload[static_cast<std::size_t>(i)] =
                    static_cast<std::uint8_t>(static_cast<int>(cells[i].distance) - p);
            }
        }
    }
    for (int i = 0; i < kChunkCells; ++i) {
        payload[static_cast<std::size_t>(kChunkCells + i)] = cells[i].material;
    }

    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    if (cctx == nullptr) {
        // Out of memory or similar: encode_chunk_frame has no error channel
        // of its own (ChunkData::encoded() calls it under std::call_once
        // expecting a vector back), so this is the one way to report it
        // rather than silently treating a garbage size as a real one below.
        throw std::runtime_error("zstd: failed to create a compression context");
    }
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1);
    const std::size_t bound = ZSTD_compressBound(payload.size());
    std::vector<std::byte> out(bound);
    const std::size_t written = ZSTD_compress2(cctx, out.data(), bound, payload.data(), payload.size());
    ZSTD_freeCCtx(cctx);
    if (ZSTD_isError(written)) {
        // written is ZSTD's error code in this case, not a byte count:
        // resizing to it (as if it were always a valid size) would ask the
        // vector for a bogus length instead of reporting the real failure.
        throw std::runtime_error(std::string("zstd: failed to compress a chunk frame: ") +
                                  ZSTD_getErrorName(written));
    }
    out.resize(written);
    return out;
}

std::vector<std::byte> encode_avox(const VoxelVolume& volume) {
    std::vector<std::pair<ChunkCoord, ChunkPtr>> entries(volume.chunks().begin(), volume.chunks().end());
    // A VoxelVolume never keeps an all-air chunk in its map, but skipping one
    // defensively costs nothing and matches the spec exactly ("only chunks
    // that are not all air are written").
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const auto& e) { return e.second->is_air(); }),
                  entries.end());
    std::sort(entries.begin(), entries.end(), by_zyx);

    const std::uint32_t chunk_count = static_cast<std::uint32_t>(entries.size());
    const std::size_t index_start = kHeaderSize;
    const std::size_t frames_start = index_start + static_cast<std::size_t>(chunk_count) * kIndexEntrySize;

    // Each dense chunk's frame, fetched (and compressed, if this is its
    // first time) before any offset is known, so a single pass over the
    // sorted entries below can place them one after another.
    std::vector<const std::vector<std::byte>*> frames(entries.size(), nullptr);
    std::size_t total_frame_bytes = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].second->is_uniform()) {
            frames[i] = &entries[i].second->encoded();
            total_frame_bytes += frames[i]->size();
        }
    }

    std::vector<std::byte> out(frames_start + total_frame_bytes);

    std::uint64_t offset = static_cast<std::uint64_t>(frames_start);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        std::byte* entry = out.data() + index_start + i * kIndexEntrySize;
        const ChunkCoord& coord = entries[i].first;
        put_i32(entry + 0, coord.x);
        put_i32(entry + 4, coord.y);
        put_i32(entry + 8, coord.z);
        const bool dense = frames[i] != nullptr;
        put_u8(entry + 12, dense ? 1u : 0u);
        if (dense) {
            put_u8(entry + 13, 0);
            put_u8(entry + 14, 0);
        } else {
            const Cell value = entries[i].second->cell(0);
            put_u8(entry + 13, static_cast<std::uint8_t>(value.distance));
            put_u8(entry + 14, value.material);
        }
        put_u8(entry + 15, 0);  // reserved
        if (dense) {
            const std::uint32_t frame_size = static_cast<std::uint32_t>(frames[i]->size());
            put_u64(entry + 16, offset);
            put_u32(entry + 24, frame_size);
            std::memcpy(out.data() + offset, frames[i]->data(), frame_size);
            offset += frame_size;
        } else {
            put_u64(entry + 16, 0);
            put_u32(entry + 24, 0);
        }
        put_u32(entry + 28, 0);  // reserved
    }

    // The header is written last since index_crc32 covers the index this
    // loop just filled in.
    std::byte* header = out.data();
    header[0] = std::byte{'A'};
    header[1] = std::byte{'V'};
    header[2] = std::byte{'O'};
    header[3] = std::byte{'X'};
    put_u16(header + 4, 1);  // version_major
    put_u16(header + 6, 0);  // version_minor
    put_f32(header + 8, volume.voxel_size());
    put_u32(header + 12, static_cast<std::uint32_t>(kChunkSize));
    put_u32(header + 16, chunk_count);
    const std::uint32_t index_crc =
        crc32_of(out.data() + index_start, static_cast<std::size_t>(chunk_count) * kIndexEntrySize);
    put_u32(header + 20, index_crc);
    put_u64(header + 24, 0);  // reserved
    return out;
}

std::optional<std::string> decode_avox(const std::byte* data, std::size_t size, VoxelVolume& out,
                                        unsigned threads) {
    out = VoxelVolume();
    if (size < kHeaderSize) {
        return std::string("not an .avox file");
    }
    if (static_cast<char>(data[0]) != 'A' || static_cast<char>(data[1]) != 'V' ||
        static_cast<char>(data[2]) != 'O' || static_cast<char>(data[3]) != 'X') {
        return std::string("not an .avox file");
    }
    const std::uint16_t major = get_u16(data + 4);
    if (major != 1) {
        return std::string("unsupported .avox version");
    }
    const float voxel_size = get_f32(data + 8);
    const std::uint32_t chunk_size = get_u32(data + 12);
    const std::uint32_t chunk_count = get_u32(data + 16);
    const std::uint32_t index_crc32 = get_u32(data + 20);
    if (chunk_size != static_cast<std::uint32_t>(kChunkSize)) {
        return std::string("damaged .avox file");
    }
    if (!std::isfinite(voxel_size) || voxel_size <= 0.f) {
        return std::string("damaged .avox file");
    }
    const std::size_t index_start = kHeaderSize;
    const std::uint64_t index_bytes = static_cast<std::uint64_t>(chunk_count) * kIndexEntrySize;
    if (static_cast<std::uint64_t>(index_start) + index_bytes > static_cast<std::uint64_t>(size)) {
        return std::string("damaged .avox file");
    }
    if (crc32_of(data + index_start, static_cast<std::size_t>(index_bytes)) != index_crc32) {
        return std::string("damaged .avox file");
    }

    const std::uint64_t frames_start = static_cast<std::uint64_t>(index_start) + index_bytes;
    std::vector<IndexEntry> entries(chunk_count);
    std::vector<std::uint32_t> dense_indices;
    for (std::uint32_t i = 0; i < chunk_count; ++i) {
        const std::byte* e = data + index_start + static_cast<std::size_t>(i) * kIndexEntrySize;
        IndexEntry& entry = entries[i];
        entry.coord = ChunkCoord{get_i32(e + 0), get_i32(e + 4), get_i32(e + 8)};
        if (i > 0) {
            // The index must be sorted by (z, y, x), strictly: a duplicate or
            // out-of-order coordinate is damage, not something to silently
            // overwrite or reorder underneath the caller.
            const ChunkCoord& prev = entries[i - 1].coord;
            const bool increasing = (entry.coord.z > prev.z) ||
                                    (entry.coord.z == prev.z && entry.coord.y > prev.y) ||
                                    (entry.coord.z == prev.z && entry.coord.y == prev.y && entry.coord.x > prev.x);
            if (!increasing) {
                return std::string("damaged .avox file");
            }
        }
        entry.form = get_u8(e + 12);
        if (entry.form > 1) {
            return std::string("damaged .avox file");
        }
        entry.distance = static_cast<std::int8_t>(get_u8(e + 13));
        entry.id = get_u8(e + 14);
        entry.offset = get_u64(e + 16);
        entry.size = get_u32(e + 24);
        if (entry.form == 1) {
            if (entry.offset < frames_start || entry.offset > static_cast<std::uint64_t>(size) ||
                entry.offset + entry.size > static_cast<std::uint64_t>(size)) {
                return std::string("damaged .avox file");
            }
            dense_indices.push_back(i);
        }
    }

    // Build every dense entry's chunk (decode, finish(), adopt its frame) on
    // up to `threads` worker threads, each taking every n-th dense entry
    // into its own slot of `built` so no two threads ever touch the same
    // slot. Doing the whole chunk, not just the zstd call, on the worker is
    // what gets 4,096 chunks under budget: the Lorenzo un-prediction and
    // finish()'s scan are each a full pass over the chunk's 32,768 cells,
    // and only run once per thread's own share this way instead of once,
    // single-threaded, after every thread has already finished decoding.
    std::vector<std::shared_ptr<ChunkData>> built(dense_indices.size());
    std::atomic<bool> damaged{false};
    unsigned worker_count = threads;
    if (worker_count == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        worker_count = hw > 1 ? hw - 1 : 1;
    }
    // No more threads than dense chunks: a small island starts none.
    worker_count = static_cast<unsigned>(
        std::max<std::size_t>(1, std::min<std::size_t>(worker_count, dense_indices.size())));
    if (!dense_indices.empty()) {
        auto work = [&](unsigned start) {
            for (std::size_t j = start; j < dense_indices.size(); j += worker_count) {
                try {
                    const IndexEntry& entry = entries[dense_indices[j]];
                    std::shared_ptr<ChunkData> chunk = ChunkData::air()->clone_dense();
                    if (!decode_chunk_frame(data + entry.offset, entry.size, *chunk)) {
                        damaged.store(true, std::memory_order_relaxed);
                        continue;
                    }
                    chunk->finish();
                    if (chunk->is_air()) {
                        // A dense frame that happens to decode to nothing but
                        // air collapses the same way an edit would: dropped
                        // rather than kept, since a missing chunk is air
                        // already (built[j] stays null; the merge below
                        // skips it).
                        continue;
                    }
                    if (!chunk->is_uniform()) {
                        // Only a chunk that stays dense gets a cached frame:
                        // encoded() must stay empty for a uniform chunk, the
                        // same as one built by an ordinary edit.
                        std::vector<std::byte> frame(entry.size);
                        std::memcpy(frame.data(), data + entry.offset, entry.size);
                        chunk->adopt_encoded(std::move(frame));
                    }
                    built[j] = std::move(chunk);
                } catch (...) {
                    // Any exception here (e.g. bad_alloc while cloning or
                    // decoding a chunk) is reported as damage rather than
                    // left to escape a worker thread, which would otherwise
                    // call std::terminate.
                    damaged.store(true, std::memory_order_relaxed);
                }
            }
        };
        try {
            JoiningThreads joiner;
            for (unsigned w = 1; w < worker_count; ++w) {
                joiner.threads.emplace_back(work, w);
            }
            work(0);  // this thread takes a share too, instead of only joining
        } catch (...) {
            // A worker thread failed to start (e.g. the OS is out of
            // resources). JoiningThreads's destructor, run by this catch's
            // own unwind, already joined whatever threads it held, so there
            // is nothing left joinable here.
            damaged.store(true, std::memory_order_relaxed);
        }
    }
    if (damaged.load(std::memory_order_relaxed)) {
        out = VoxelVolume();
        return std::string("damaged .avox file");
    }

    ChunkMap map;
    for (std::uint32_t i = 0; i < chunk_count; ++i) {
        const IndexEntry& entry = entries[i];
        if (entry.form != 0) {
            continue;
        }
        const Cell value{entry.distance, entry.id};
        if (value.distance == kAirDistance) {
            continue;  // all-air: a missing chunk is air already
        }
        map[entry.coord] = ChunkData::uniform(value);
    }
    for (std::size_t j = 0; j < dense_indices.size(); ++j) {
        if (built[j]) {
            map[entries[dense_indices[j]].coord] = std::move(built[j]);
        }
    }

    out = VoxelVolume(voxel_size);
    out.set_chunks(std::move(map));
    std::vector<ChunkCoord> discard;
    out.take_dirty(discard);  // a freshly loaded volume starts with nothing dirty
    return std::nullopt;
}

}  // namespace engine_core::terrain
