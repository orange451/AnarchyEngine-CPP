#include "terrain/AlodStore.hpp"

#include <cstring>
#include <system_error>

namespace engine_core::terrain {

namespace {

constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderSize = 40;
constexpr std::size_t kRecordHead = 16 + 4 + 24 + 4 + 24 + 12;
constexpr std::size_t kEntrySize = 16 + 8 + 4 + 4 + 24;

// Appends little-endian values to a byte buffer.
struct Writer {
    std::vector<std::byte> bytes;
    void raw(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::byte*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xffu));
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xffu));
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f32(float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    }
    void vec3(Vec3 v) {
        f32(v.x);
        f32(v.y);
        f32(v.z);
    }
    void u16s(const std::vector<std::uint16_t>& v) {
        for (std::uint16_t x : v) {
            bytes.push_back(static_cast<std::byte>(x & 0xffu));
            bytes.push_back(static_cast<std::byte>(x >> 8));
        }
    }
    void u8s(const std::vector<std::uint8_t>& v) { raw(v.data(), v.size()); }
    void u32s(const std::vector<std::uint32_t>& v) {
        for (std::uint32_t x : v) u32(x);
    }
};

// Reads little-endian values from a byte range; any read past the end
// leaves ok false and returns zeros.
struct Reader {
    const std::byte* data;
    std::size_t size;
    std::size_t at = 0;
    bool ok = true;
    bool take(std::size_t n) {
        if (!ok || size - at < n) {
            ok = false;
            return false;
        }
        return true;
    }
    std::uint32_t u32() {
        if (!take(4)) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += 4;
        return v;
    }
    std::uint64_t u64() {
        if (!take(8)) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(data[at + static_cast<std::size_t>(i)]) << (8 * i);
        at += 8;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() {
        const std::uint32_t bits = u32();
        float v = 0.f;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    Vec3 vec3() {
        const float x = f32();
        const float y = f32();
        const float z = f32();
        return Vec3{x, y, z};
    }
    void u16s(std::vector<std::uint16_t>& out, std::size_t count) {
        if (count > size || !take(count * 2)) return;
        out.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = static_cast<std::uint16_t>(static_cast<unsigned>(data[at]) | (static_cast<unsigned>(data[at + 1]) << 8));
            at += 2;
        }
    }
    void u8s(std::vector<std::uint8_t>& out, std::size_t count) {
        if (!take(count)) return;
        out.resize(count);
        std::memcpy(out.data(), data + at, count);
        at += count;
    }
    void u32s(std::vector<std::uint32_t>& out, std::size_t count) {
        if (count > size || !take(count * 4)) return;
        out.resize(count);
        for (std::size_t i = 0; i < count; ++i) out[i] = u32();
    }
};

void write_key(Writer& w, const NodeKey& key) {
    w.i32(key.level);
    w.i32(key.x);
    w.i32(key.y);
    w.i32(key.z);
}

NodeKey read_key(Reader& r) {
    NodeKey key;
    key.level = r.i32();
    key.x = r.i32();
    key.y = r.i32();
    key.z = r.i32();
    return key;
}

std::vector<std::byte> header_bytes(std::uint64_t content_key, float voxel_size, std::uint32_t footer_size,
                                    std::uint64_t footer_offset, std::uint64_t footer_hash) {
    Writer w;
    w.raw("ALOD", 4);
    w.u32(kVersion);
    w.u64(content_key);
    w.f32(voxel_size);
    w.u32(footer_size);
    w.u64(footer_offset);
    w.u64(footer_hash);
    return w.bytes;
}

bool read_at(std::fstream& file, std::uint64_t offset, std::byte* out, std::size_t size) {
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(file.gcount()) == size && !file.fail();
}

bool write_at(std::fstream& file, std::uint64_t offset, const std::vector<std::byte>& bytes) {
    file.clear();
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return !file.fail();
}

}  // namespace

std::uint64_t content_key_of(const std::byte* data, std::size_t size) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash != 0 ? hash : 1;
}

std::optional<AlodStore> AlodStore::open(const std::filesystem::path& path, std::uint64_t content_key,
                                         float voxel_size) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }
    const std::uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error || file_size < kHeaderSize) {
        return std::nullopt;
    }
    auto file = std::make_unique<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file->is_open()) {
        return std::nullopt;
    }
    std::byte header[kHeaderSize];
    if (!read_at(*file, 0, header, kHeaderSize) || std::memcmp(header, "ALOD", 4) != 0) {
        return std::nullopt;
    }
    Reader h{header, kHeaderSize, 4};
    const std::uint32_t version = h.u32();
    const std::uint64_t stored_key = h.u64();
    const float stored_size = h.f32();
    const std::uint32_t footer_size = h.u32();
    const std::uint64_t footer_offset = h.u64();
    const std::uint64_t footer_hash = h.u64();
    if (version != kVersion || stored_key != content_key || stored_size != voxel_size ||
        footer_offset < kHeaderSize || footer_offset > file_size || footer_size > file_size - footer_offset) {
        return std::nullopt;
    }
    std::vector<std::byte> footer(footer_size);
    if (!read_at(*file, footer_offset, footer.data(), footer.size()) ||
        content_key_of(footer.data(), footer.size()) != footer_hash) {
        return std::nullopt;
    }
    AlodStore store;
    Reader r{footer.data(), footer.size()};
    const std::uint32_t entries = r.u32();
    if (!r.ok || entries > footer.size() / kEntrySize) {
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < entries; ++i) {
        AlodEntry entry;
        entry.key = read_key(r);
        entry.offset = r.u64();
        entry.size = r.u32();
        entry.error = r.f32();
        entry.bounds_min = r.vec3();
        entry.bounds_max = r.vec3();
        if (!r.ok || entry.offset < kHeaderSize || entry.offset > footer_offset ||
            entry.size > footer_offset - entry.offset) {
            return std::nullopt;
        }
        store.entries_[entry.key] = entry;
    }
    const std::uint32_t chunks = r.u32();
    if (!r.ok || chunks > footer.size() / 12) {
        return std::nullopt;
    }
    store.surface_chunks_.reserve(chunks);
    for (std::uint32_t i = 0; i < chunks; ++i) {
        ChunkCoord coord;
        coord.x = r.i32();
        coord.y = r.i32();
        coord.z = r.i32();
        store.surface_chunks_.push_back(coord);
    }
    if (!r.ok || r.at != footer.size()) {
        return std::nullopt;
    }
    store.path_ = path;
    store.file_ = std::move(file);
    store.content_key_ = content_key;
    store.voxel_size_ = voxel_size;
    store.end_ = file_size;
    return store;
}

std::optional<AlodStore> AlodStore::create(const std::filesystem::path& path, std::uint64_t content_key,
                                           float voxel_size) {
    {
        std::ofstream make(path, std::ios::binary | std::ios::trunc);
        if (!make.is_open()) {
            return std::nullopt;
        }
    }
    AlodStore store;
    store.file_ = std::make_unique<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!store.file_->is_open()) {
        return std::nullopt;
    }
    store.path_ = path;
    store.content_key_ = content_key;
    store.voxel_size_ = voxel_size;
    store.end_ = kHeaderSize;
    if (!store.commit()) {
        return std::nullopt;
    }
    return store;
}

std::shared_ptr<const CompactMesh> AlodStore::load(const NodeKey& key) {
    const auto found = entries_.find(key);
    if (found == entries_.end() || !file_) {
        return nullptr;
    }
    const AlodEntry& entry = found->second;
    std::vector<std::byte> bytes(entry.size);
    if (!read_at(*file_, entry.offset, bytes.data(), bytes.size())) {
        return nullptr;
    }
    Reader r{bytes.data(), bytes.size()};
    if (!(read_key(r) == key)) {
        return nullptr;
    }
    r.f32();                 // error: the index has it
    r.vec3();                // bounds: likewise
    r.vec3();
    auto mesh = std::make_shared<CompactMesh>();
    mesh->surface_index_count = r.u32();
    mesh->origin = r.vec3();
    mesh->scale = r.vec3();
    const std::size_t vertices = r.u32();
    const std::size_t indices16 = r.u32();
    const std::size_t indices32 = r.u32();
    if (!r.ok || vertices > bytes.size()) {
        return nullptr;
    }
    r.u16s(mesh->positions, vertices * 3);
    r.u8s(mesh->normals, vertices * 2);
    r.u8s(mesh->ids, vertices * 4);
    r.u8s(mesh->weights, vertices * 4);
    r.u16s(mesh->indices, indices16);
    r.u32s(mesh->indices32, indices32);
    if (!r.ok || r.at != bytes.size()) {
        return nullptr;
    }
    return mesh;
}

bool AlodStore::put(const NodeKey& key, const CompactMesh& mesh, float error, Vec3 bounds_min, Vec3 bounds_max) {
    if (!file_) {
        return false;
    }
    Writer w;
    w.bytes.reserve(kRecordHead + mesh.positions.size() * 2 + mesh.normals.size() + mesh.ids.size() +
                    mesh.weights.size() + mesh.indices.size() * 2 + mesh.indices32.size() * 4);
    write_key(w, key);
    w.f32(error);
    w.vec3(bounds_min);
    w.vec3(bounds_max);
    w.u32(mesh.surface_index_count);
    w.vec3(mesh.origin);
    w.vec3(mesh.scale);
    w.u32(static_cast<std::uint32_t>(mesh.positions.size() / 3));
    w.u32(static_cast<std::uint32_t>(mesh.indices.size()));
    w.u32(static_cast<std::uint32_t>(mesh.indices32.size()));
    w.u16s(mesh.positions);
    w.u8s(mesh.normals);
    w.u8s(mesh.ids);
    w.u8s(mesh.weights);
    w.u16s(mesh.indices);
    w.u32s(mesh.indices32);
    if (!write_at(*file_, end_, w.bytes)) {
        return false;
    }
    AlodEntry entry;
    entry.key = key;
    entry.offset = end_;
    entry.size = static_cast<std::uint32_t>(w.bytes.size());
    entry.error = error;
    entry.bounds_min = bounds_min;
    entry.bounds_max = bounds_max;
    entries_[key] = entry;
    end_ += w.bytes.size();
    return true;
}

bool AlodStore::commit() {
    if (!file_) {
        return false;
    }
    Writer w;
    w.u32(static_cast<std::uint32_t>(entries_.size()));
    for (const auto& [key, entry] : entries_) {
        write_key(w, key);
        w.u64(entry.offset);
        w.u32(entry.size);
        w.f32(entry.error);
        w.vec3(entry.bounds_min);
        w.vec3(entry.bounds_max);
    }
    w.u32(static_cast<std::uint32_t>(surface_chunks_.size()));
    for (const ChunkCoord& coord : surface_chunks_) {
        w.i32(coord.x);
        w.i32(coord.y);
        w.i32(coord.z);
    }
    const std::uint64_t footer_offset = end_;
    if (!write_at(*file_, footer_offset, w.bytes)) {
        return false;
    }
    file_->flush();
    end_ += w.bytes.size();
    // The footer is on disk before the header points at it.
    if (!write_at(*file_, 0,
                  header_bytes(content_key_, voxel_size_, static_cast<std::uint32_t>(w.bytes.size()), footer_offset,
                               content_key_of(w.bytes.data(), w.bytes.size())))) {
        return false;
    }
    file_->flush();
    return !file_->fail();
}

std::uint64_t AlodStore::live_bytes() const {
    std::uint64_t total = 0;
    for (const auto& [key, entry] : entries_) {
        (void)key;
        total += entry.size;
    }
    return total;
}

}  // namespace engine_core::terrain
