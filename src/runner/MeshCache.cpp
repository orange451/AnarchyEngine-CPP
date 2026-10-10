#include "MeshCache.hpp"

#include "AssetLoads.hpp"

#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace runner {

namespace {

// How often a file already seen is looked at again.
constexpr std::chrono::seconds kRecheck{1};

}  // namespace

MeshCache::MeshCache(Report report) : report_(std::move(report)) {}

MeshCache::~MeshCache() {
    for (auto& [path, entry] : entries_) {
        entry.mesh.forget();
    }
    for (auto& [mesh, entry] : sessions_) {
        entry.mesh.forget();
    }
    for (auto& [revision, entry] : brushes_) {
        entry.mesh.forget();
    }
    for (auto& [key, entry] : nodes_) {
        entry.upload.mesh.forget();
    }
}

void MeshCache::setRoot(const std::filesystem::path& root) {
    if (root == root_) {
        return;
    }
    clear();
    root_ = root;
}

void MeshCache::clear() {
    for (auto& [path, entry] : entries_) {
        entry.mesh.destroy();
    }
    entries_.clear();
    for (auto& [mesh, entry] : sessions_) {
        entry.mesh.destroy();
    }
    sessions_.clear();
    for (auto& [revision, entry] : brushes_) {
        entry.mesh.destroy();
    }
    brushes_.clear();
    for (auto& [key, entry] : nodes_) {
        entry.upload.mesh.destroy();
    }
    nodes_.clear();
}

void MeshCache::uploadOnce(SessionEntry& entry, const anarchy::amesh::Data& data, std::uint64_t revision,
                           bool dynamic, const char* what) {
    entry.asked = true;
    if (entry.tried && entry.revision == revision) {
        return;
    }
    entry.tried = true;
    entry.revision = revision;
    if (data.indices.empty()) {
        entry.mesh.destroy();
        return;
    }
    try {
        entry.mesh.upload(data, dynamic);
    } catch (const std::exception& failure) {
        entry.mesh.destroy();
        if (report_) {
            report_(std::string(what) + failure.what());
        }
    }
}

const anarchy::amesh::GpuMesh* MeshCache::getSession(engine_core::InstanceId mesh, const anarchy::amesh::Data& data,
                                                    std::uint64_t revision) {
    SessionEntry& entry = sessions_[mesh];
    // Dynamic: a script may replace it again next frame.
    uploadOnce(entry, data, revision, true, "A Mesh's play-session shapes could not be drawn: ");
    return entry.mesh.valid() ? &entry.mesh : nullptr;
}

void MeshCache::sweepSessions() {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (!it->second.asked) {
            it->second.mesh.destroy();
            it = sessions_.erase(it);
        } else {
            it->second.asked = false;
            ++it;
        }
    }
}

const anarchy::amesh::GpuMesh* MeshCache::getBrush(std::uint64_t revision, const anarchy::amesh::Data& data) {
    SessionEntry& entry = brushes_[revision];
    uploadOnce(entry, data, revision, false, "Brush geometry could not be drawn: ");
    return entry.mesh.valid() ? &entry.mesh : nullptr;
}

void MeshCache::sweepBrushes() {
    for (auto it = brushes_.begin(); it != brushes_.end();) {
        if (!it->second.asked) {
            it->second.mesh.destroy();
            it = brushes_.erase(it);
        } else {
            it->second.asked = false;
            ++it;
        }
    }
}

std::size_t MeshCache::NodeCacheKeyHash::operator()(const NodeCacheKey& key) const {
    return engine_core::terrain::NodeKeyHash{}(key.key) * 31u + std::hash<engine_core::InstanceId>{}(key.terrain);
}

const anarchy::amesh::GpuMesh* MeshCache::getTerrainNode(engine_core::InstanceId terrain,
                                                        const engine_core::terrain::NodeKey& key,
                                                        const anarchy::amesh::Data& data, std::uint64_t revision) {
    SessionEntry& entry = nodes_[NodeCacheKey{terrain, key}].upload;
    uploadOnce(entry, data, revision, false, "A Terrain LOD node could not be drawn: ");
    return entry.mesh.valid() ? &entry.mesh : nullptr;
}

const anarchy::amesh::GpuMesh* MeshCache::getTerrainNode(engine_core::InstanceId terrain,
                                                        const engine_core::terrain::NodeKey& key,
                                                        const engine_core::terrain::CompactMesh& compact,
                                                        std::uint64_t revision) {
    SessionEntry& entry = nodes_[NodeCacheKey{terrain, key}].upload;
    if (!(entry.tried && entry.revision == revision)) {
        // Unpacked only to upload (spec decision 1); the copy goes when this returns.
        uploadOnce(entry, engine_core::terrain::unpack(compact), revision, false,
                   "A Terrain LOD node could not be drawn: ");
    }
    entry.asked = true;
    return entry.mesh.valid() ? &entry.mesh : nullptr;
}

bool MeshCache::touchTerrainNode(engine_core::InstanceId terrain, const engine_core::terrain::NodeKey& key,
                                 std::uint64_t revision) {
    const auto found = nodes_.find(NodeCacheKey{terrain, key});
    if (found == nodes_.end() || !found->second.upload.tried || found->second.upload.revision != revision) {
        return false;
    }
    found->second.upload.asked = true;
    return true;
}

void MeshCache::sweepTerrainNodes(double now_seconds) {
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        NodeEntry& entry = it->second;
        if (entry.upload.asked) {
            entry.upload.asked = false;
            entry.used = now_seconds;
            ++it;
        } else if (now_seconds - entry.used >= kTerrainNodeGraceSeconds) {
            entry.upload.mesh.destroy();
            it = nodes_.erase(it);
        } else {
            ++it;
        }
    }
}

const anarchy::amesh::GpuMesh* MeshCache::get(const std::string& path) {
    if (root_.empty() || path.empty()) {
        return nullptr;
    }
    Entry& entry = entries_[path];
    const auto now = std::chrono::steady_clock::now();
    if (!entry.tried || now - entry.checked >= kRecheck) {
        entry.checked = now;
        load(path, entry);
        // Mesh.Loaded: shown, or its file is missing or will not read.
        engine_core::set_asset_loaded(engine_core::AssetKind::Mesh, root_, path, entry.mesh.valid());
    }
    return entry.mesh.valid() ? &entry.mesh : nullptr;
}

void MeshCache::load(const std::string& path, Entry& entry) {
    // Mesh Paths use '/', which every platform's path splits on.
    const std::filesystem::path file = root_ / std::filesystem::u8path(path);
    std::error_code error;
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(file, error);
    if (error) {
        const bool was_there = !entry.tried || entry.mesh.valid() || entry.stamp != std::filesystem::file_time_type{};
        entry.tried = true;
        entry.stamp = {};
        entry.mesh.destroy();
        if (was_there && report_) {
            report_("Mesh " + path + " was not found in the resources folder");
        }
        return;
    }
    if (entry.tried && stamp == entry.stamp) {
        return;
    }
    entry.tried = true;
    entry.stamp = stamp;

    const std::uintmax_t size = std::filesystem::file_size(file, error);
    if (error || size > anarchy::amesh::kMaxFileSize) {
        entry.mesh.destroy();
        if (report_) {
            report_("Mesh " + path + " could not be read, or is larger than 512 MiB");
        }
        return;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        entry.mesh.destroy();
        if (report_) {
            report_("Mesh " + path + " could not be read");
        }
        return;
    }
    try {
        entry.mesh.upload(anarchy::amesh::read(bytes));
    } catch (const std::exception& failure) {
        // A mesh that stops reading stops drawing, rather than showing the old version.
        entry.mesh.destroy();
        if (report_) {
            report_("Mesh " + path + " is not an AMESH file it can draw: " + failure.what());
        }
    }
}

}  // namespace runner
