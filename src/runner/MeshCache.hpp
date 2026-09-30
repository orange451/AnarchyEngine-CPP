#pragma once

#include "amesh.hpp"
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
        bool asked = false;
    };

    void load(const std::string& path, Entry& entry);

    Report report_;
    std::filesystem::path root_;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<engine_core::InstanceId, SessionEntry> sessions_;
};

}  // namespace runner
