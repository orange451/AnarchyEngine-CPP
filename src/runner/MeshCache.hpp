#pragma once

#include "amesh.hpp"
#include "terrain/LodNode.hpp"
#include "types.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

namespace runner {

// A Scene View's uploaded meshes, by Mesh Path. A path loads the first time a
// frame draws it, as AMESH, from the project's resources folder. Its file is
// looked at again at most once a second and reloaded when it changed, so a
// re-baked mesh shows without reopening the place. A file that is missing or
// does not read draws nothing; report hears why, once per version of the file.
// Every call but the destructor needs the GL context the meshes were uploaded in.
class MeshCache {
public:
    using Report = std::function<void(const std::string& message)>;

    explicit MeshCache(Report report = {});
    // Needs no GL: a cache still holding meshes is one whose context is gone.
    ~MeshCache();

    MeshCache(const MeshCache&) = delete;
    MeshCache& operator=(const MeshCache&) = delete;

    // The resources folder paths are under. Empty loads nothing. Another root clears the cache.
    void setRoot(const std::filesystem::path& root);
    // The mesh for path, relative to the root with '/' between names, or null
    // when there is none to draw.
    const anarchy::amesh::GpuMesh* get(const std::string& path);
    // The upload of a Mesh's play-session geometry, kept per Mesh and uploaded
    // again when revision changes. Null for geometry with no triangles.
    const anarchy::amesh::GpuMesh* getSession(engine_core::InstanceId mesh, const anarchy::amesh::Data& data,
                                             std::uint64_t revision);
    // Deletes the session uploads no getSession asked for since the last sweep,
    // such as those a Stop ended. Call once a frame, after that frame's
    // getSession calls: the uploads they returned stay.
    void sweepSessions();
    // Brush geometry (engine_core::BrushVisuals), keyed by its revision, which
    // is unique to each mesh: an edited cell is a new key, its old upload swept.
    const anarchy::amesh::GpuMesh* getBrush(std::uint64_t revision, const anarchy::amesh::Data& data);
    // Deletes the Brush uploads no getBrush asked for since the last sweep.
    void sweepBrushes();
    // The upload of one Terrain LOD node's mesh, kept per (terrain, key) and
    // uploaded again when revision (TerrainNodeView::revision) changes. A
    // level-0 node's chunk mesh (data) uploads as it is; a coarser node's
    // compact mesh is unpacked only when its revision is new to the cache,
    // and the unpacked copy is let go once uploaded. Null for a mesh with no
    // triangles. Its bounds, which culling reads, are in the Terrain's space.
    const anarchy::amesh::GpuMesh* getTerrainNode(engine_core::InstanceId terrain,
                                                 const engine_core::terrain::NodeKey& key,
                                                 const anarchy::amesh::Data& data, std::uint64_t revision);
    const anarchy::amesh::GpuMesh* getTerrainNode(engine_core::InstanceId terrain,
                                                 const engine_core::terrain::NodeKey& key,
                                                 const engine_core::terrain::CompactMesh& compact,
                                                 std::uint64_t revision);
    // Whether the node's upload of revision is held already (uploaded, or found
    // to have no triangles); if so it counts as asked for, as getTerrainNode
    // would, so the sweep keeps it. Uploads nothing: for prefetching.
    bool touchTerrainNode(engine_core::InstanceId terrain, const engine_core::terrain::NodeKey& key,
                          std::uint64_t revision);
    // Deletes the node uploads no getTerrainNode (or touchTerrainNode) asked for in the last
    // kTerrainNodeGraceSeconds by now_seconds (the caller's clock, never
    // decreasing): nodes no longer drawn, and Terrains that left Workspace.
    // Call once a frame, after that frame's getTerrainNode calls.
    void sweepTerrainNodes(double now_seconds);
    static constexpr double kTerrainNodeGraceSeconds = 5.0;
    // How many node uploads the cache holds, for tests.
    std::size_t terrainNodeCount() const { return nodes_.size(); }
    // Deletes every mesh.
    void clear();

private:
    struct Entry {
        anarchy::amesh::GpuMesh mesh;
        // The file's time when it was last read; unset before the first try.
        std::filesystem::file_time_type stamp{};
        bool tried = false;
        std::chrono::steady_clock::time_point checked{};
    };

    struct SessionEntry {
        anarchy::amesh::GpuMesh mesh;
        std::uint64_t revision = 0;
        // Whether revision has been uploaded (or found empty) yet, so a first revision of 0 still uploads.
        bool tried = false;
        bool asked = false;
    };

    struct NodeEntry {
        SessionEntry upload;
        // now_seconds at the last sweep that found it asked for.
        double used = 0.0;
    };
    struct NodeCacheKey {
        engine_core::InstanceId terrain = 0;
        engine_core::terrain::NodeKey key;
        bool operator==(const NodeCacheKey& other) const { return terrain == other.terrain && key == other.key; }
    };
    struct NodeCacheKeyHash {
        std::size_t operator()(const NodeCacheKey& key) const;
    };

    void load(const std::string& path, Entry& entry);
    // Marks entry asked, and uploads data into it when revision is new. What
    // failed goes to report_, after what.
    void uploadOnce(SessionEntry& entry, const anarchy::amesh::Data& data, std::uint64_t revision, bool dynamic,
                    const char* what);

    Report report_;
    std::filesystem::path root_;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<engine_core::InstanceId, SessionEntry> sessions_;
    std::unordered_map<std::uint64_t, SessionEntry> brushes_;
    std::unordered_map<NodeCacheKey, NodeEntry, NodeCacheKeyHash> nodes_;
};

}  // namespace runner
