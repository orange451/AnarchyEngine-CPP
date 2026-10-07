#pragma once

// The mesher pool: runs Surface Nets (and an optional collider build) on
// worker threads, off SimulationThread. The caller queues MeshInputs keyed
// by (terrain, coord) and collects finished MeshResults later; workers never
// touch the caller's state, only the immutable chunks each job already holds.

#include "terrain/LodBuilder.hpp"
#include "terrain/SurfaceNets.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace engine_core::terrain {

// What a worker hands back for one chunk.
struct MeshResult {
    std::uint64_t terrain = 0;            // the caller's key (a Terrain's InstanceId)
    ChunkCoord coord;
    std::uint64_t revision = 0;           // the caller's revision for this chunk when queued
    ChunkMesh mesh;
    std::shared_ptr<void> collider;       // what build_collider made, or null
    // The job threw: mesh and collider are empty. The chunk should keep the
    // mesh it had; the job is over, so nothing should wait on it.
    bool failed = false;
};

// What a worker hands back for one LOD node build (spec decision 5: the
// mesher's job queue holds both chunk-mesh and node-build jobs, sharing
// workers and distance ordering).
struct NodeResult {
    std::uint64_t terrain = 0;   // the caller's key (a Terrain's InstanceId)
    NodeKey key;
    std::uint64_t revision = 0;  // the caller's revision for this node when queued
    LodResult result;
    // The job threw: result is empty (key set, no mesh). The node should
    // keep what it had and may be queued again.
    bool failed = false;
};

// Meshes chunks on worker threads (hardware threads less one, at least one,
// at most 4). The caller queues jobs and collects results on its own thread
// (SimulationThread); workers read only the jobs' immutable chunks.
class TerrainMesher {
public:
    // Builds a physics collider from a finished mesh, on the worker. Optional.
    using BuildCollider = std::function<std::shared_ptr<void>(const ChunkMesh&)>;
    // Builds a LOD node from its LodInput, on the worker. Optional: default
    // (empty) calls LodBuilder's own build_node() directly, the same way a
    // chunk job always calls surface_nets() -- the override exists only so
    // a test can substitute a throwing builder (TM11), the same role
    // BuildCollider already plays for chunk jobs (TM6).
    using BuildNode = std::function<LodResult(const LodInput&)>;
    explicit TerrainMesher(BuildCollider build = {}, unsigned threads = 0, BuildNode build_node_fn = {});
    ~TerrainMesher();   // drops waiting jobs, finishes running ones, joins

    TerrainMesher(const TerrainMesher&) = delete;
    TerrainMesher& operator=(const TerrainMesher&) = delete;

    // Queues input under (terrain, coord); replaces a queued, not yet started
    // job for the same key. distance orders the queue: nearest first.
    void queue(std::uint64_t terrain, std::uint64_t revision, MeshInput input, float distance);
    // Queues a LOD node build under (terrain, kind=node, key); replaces a
    // queued, not yet started job for the same key. Shares the same
    // distance-ordered heap and worker pool as queue()'s chunk jobs.
    void queue_node(std::uint64_t terrain, std::uint64_t revision, LodInput input, float distance);
    // Every chunk result finished since the last call. Asserts no node job
    // was ever queued (queue_node() was never called on this instance): once
    // node jobs are in play, their results must not be silently dropped, so
    // callers that mix job kinds must use collect(chunks, nodes) instead.
    // Failed jobs are left out (failure_count() still counts them).
    void collect(std::vector<MeshResult>& out);
    // Every chunk and node result finished since the last call, failed jobs
    // included (flagged failed, with an empty mesh or LodResult), so a
    // caller tracking jobs in flight can stop waiting on them.
    void collect(std::vector<MeshResult>& chunks, std::vector<NodeResult>& nodes);
    // No job waiting or running.
    bool idle() const;
    // For tests: blocks until idle.
    void wait_idle();

    // A job whose surface_nets/collider/node build throws comes back flagged
    // failed (MeshResult::failed, NodeResult::failed) with nothing built: its
    // chunk or node keeps whatever it had (see worker_loop). failure_count/last_failure let
    // the owning thread (TerrainWorld, from SimulationThread) notice a rise
    // and report it once, the way Engine::report_fault already does for
    // other faults -- worker threads here never log directly.
    std::uint64_t failure_count() const;
    std::string last_failure() const;

    // Test-only: when true, a worker that is not already running a job blocks
    // before it takes its next one, so a test can queue several jobs for the
    // same key without any of them starting -- making the queued-job-replaced
    // path deterministic instead of racing a worker. Toggling back to false
    // wakes the workers.
    void pause_for_test(bool paused);

private:
    // The two job kinds sharing this pool's queue (spec decision 5).
    enum class JobKind : std::uint8_t { Chunk, Node };

    struct Job {
        std::uint64_t terrain = 0;
        JobKind kind = JobKind::Chunk;
        ChunkCoord coord;        // meaningful when kind == Chunk
        NodeKey node;            // meaningful when kind == Node
        std::uint64_t revision = 0;
        MeshInput mesh_input;    // meaningful when kind == Chunk
        LodInput node_input;     // meaningful when kind == Node
        float distance = 0.f;
        std::uint64_t sequence = 0;   // matches the HeapEntry that is still live for this key
    };

    // One key's identity in the waiting map and the heap: (terrain, kind,
    // coord-or-node) -- one queued job per key, whichever kind it is.
    struct Key {
        std::uint64_t terrain;
        JobKind kind;
        ChunkCoord coord;   // meaningful when kind == Chunk
        NodeKey node;       // meaningful when kind == Node
        bool operator==(const Key& o) const {
            if (terrain != o.terrain || kind != o.kind) return false;
            return kind == JobKind::Chunk ? coord == o.coord : node == o.node;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const;
    };

    // A heap entry: orders by distance, and carries a sequence number so a
    // stale entry (superseded by a newer queue() for the same key) can be
    // recognized and skipped when popped -- "lazy deletion" instead of
    // searching the heap to erase it.
    struct HeapEntry {
        float distance;
        std::uint64_t sequence;
        Key key;
    };
    struct HeapOrder {
        // std::priority_queue is a max-heap; nearest-first wants the smallest
        // distance on top, so this is reversed.
        bool operator()(const HeapEntry& a, const HeapEntry& b) const { return a.distance > b.distance; }
    };

    void worker_loop();
    // Pops the next live job off the heap under lock, or returns false when
    // there is nothing left to take (heap drained of live entries).
    bool take_job(Job& out);

    BuildCollider build_collider_;
    BuildNode build_node_fn_;
    unsigned thread_count_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;   // guards waiting_/heap_/running_/paused_ changes

    // The waiting set: one job per key, keyed so a later queue() for the same
    // key can replace it in place. Protected by mutex_.
    std::unordered_map<Key, Job, KeyHash> waiting_;
    // Min-heap (by distance) of keys with a job waiting. Protected by mutex_.
    // May hold stale entries for keys already taken or replaced; take_job
    // skips those by checking sequence_ against the live waiting_ entry.
    std::vector<HeapEntry> heap_;
    std::uint64_t next_sequence_ = 0;

    std::size_t running_ = 0;       // jobs a worker currently holds. Protected by mutex_.
    bool paused_ = false;           // test-only hold. Protected by mutex_.
    bool stopping_ = false;         // destructor requested shutdown. Protected by mutex_.

    std::vector<MeshResult> results_;       // finished chunk jobs since the last collect(). Protected by mutex_.
    std::vector<NodeResult> node_results_;  // finished node jobs since the last collect(). Protected by mutex_.
    bool node_job_queued_ = false;          // true once queue_node() has ever been called. Protected by mutex_.

    std::uint64_t failure_count_ = 0;   // jobs whose mesh/collider/node build threw. Protected by mutex_.
    std::string last_failure_;          // the most recent one's what() (or a fixed message). Protected by mutex_.

    std::vector<std::thread> workers_;   // started last, so they see a fully built mutex_/cv_
};

}  // namespace engine_core::terrain
