#pragma once

// A Custom PhysicsObject's Mesh as convex pieces, for a body that moves: Box3D
// gives a triangle mesh contacts only on a static body. V-HACD makes them.

#include "Vector3.hpp"
#include "amesh.hpp"
#include "types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace engine_core {

class DataModel;
class Mesh;

// Which settings made a set of pieces. Bump it whenever a setting in
// ConvexDecomposition.cpp changes, so pieces stored with the old ones are made again.
inline constexpr std::uint32_t kRecipe = 1;

// The pieces of a mesh, in its own space. Empty when there is nothing to
// decompose or V-HACD finds no piece. Any thread; seconds on a large mesh.
std::vector<anarchy::amesh::ConvexPiece> decompose(const std::vector<Vec3>& points,
                                                   const std::vector<std::uint32_t>& triangles);
// decompose, given up soon after stop is set from another thread. A run given
// up gives no pieces.
std::vector<anarchy::amesh::ConvexPiece> decompose(const std::vector<Vec3>& points,
                                                   const std::vector<std::uint32_t>& triangles,
                                                   const std::atomic<bool>& stop);

// Pieces kept in memory, keyed by the geometry they were made from and
// kRecipe: the last 64 meshes. Any thread.
void remember_pieces(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                     std::vector<anarchy::amesh::ConvexPiece> pieces);

// The Mesh's pieces without decomposing: its file's, else the cache's for
// points and triangles (the Mesh's, as vertex_positions gives them). True when
// they are known, even known to be none.
bool known_pieces(const Mesh& mesh, const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                  std::vector<anarchy::amesh::ConvexPiece>& out);

// known_pieces, else decompose now and remember the result.
std::vector<anarchy::amesh::ConvexPiece> pieces_for(const Mesh& mesh, const std::vector<Vec3>& points,
                                                    const std::vector<std::uint32_t>& triangles);

// For tests: how many times decompose has run, and forgetting every cached piece.
std::uint64_t decompose_count();
void clear_piece_cache();

// The studio's decompositions: one worker thread that splits the Meshes of
// Custom PhysicsObjects while the place is stopped, so playing finds their
// pieces in their files.
class ConvexDecomposer {
public:
    ConvexDecomposer();
    // Drops work that has not started, cancels the decompose that has, and
    // joins the worker.
    ~ConvexDecomposer();
    ConvexDecomposer(const ConvexDecomposer&) = delete;
    ConvexDecomposer& operator=(const ConvexDecomposer&) = delete;

    // On the gameplay thread, with the write lock, while stopped. Writes
    // finished pieces into their Meshes' files when the files are as they
    // were, then queues a Custom's Mesh whose file has no pieces of kRecipe,
    // once per file_stamp. One Mesh a call, so opening a project with many
    // reads one Mesh's points a tick.
    void update(DataModel& game);
    // No work waiting or running. Finished work waits for the next update.
    bool idle() const;

private:
    struct Job {
        InstanceId mesh = 0;
        std::string stamp;
        std::vector<Vec3> points;
        std::vector<std::uint32_t> triangles;
        std::vector<anarchy::amesh::ConvexPiece> pieces;
    };
    void work();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> waiting_;
    std::vector<Job> done_;
    bool running_ = false;
    // Set with mutex_ held; the running decompose reads it without.
    std::atomic<bool> stopping_{false};
    // Cancels the decompose the worker runs, while it runs. Under mutex_.
    std::function<void()> cancel_;
    // The file_stamp each Mesh was last queued at. Gameplay thread only.
    std::unordered_map<InstanceId, std::string> queued_;
    std::vector<InstanceId> bodies_;
    std::thread thread_;
};

}  // namespace engine_core
