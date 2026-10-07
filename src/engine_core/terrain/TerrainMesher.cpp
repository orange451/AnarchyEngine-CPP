#include "terrain/TerrainMesher.hpp"

#include <algorithm>
#include <cassert>
#include <exception>
#include <utility>

namespace engine_core::terrain {
namespace {

// Hardware threads less one, at least one, at most four -- used only when
// the caller asks for the default (threads == 0).
unsigned default_thread_count() {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 2;   // hardware_concurrency() may not know; assume at least 2
    unsigned count = hw > 1 ? hw - 1 : 1;
    if (count > 4) count = 4;
    return count;
}

}  // namespace

std::size_t TerrainMesher::KeyHash::operator()(const Key& k) const {
    std::size_t h = k.kind == JobKind::Chunk ? ChunkCoordHash{}(k.coord) : NodeKeyHash{}(k.node);
    h ^= std::hash<std::uint64_t>{}(k.terrain) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= (static_cast<std::size_t>(k.kind) + 0x1000193u) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

TerrainMesher::TerrainMesher(BuildCollider build, unsigned threads, BuildNode build_node_fn)
    : build_collider_(std::move(build)),
      build_node_fn_(std::move(build_node_fn)),
      thread_count_(threads != 0 ? threads : default_thread_count()) {
    workers_.reserve(thread_count_);
    for (unsigned i = 0; i < thread_count_; ++i) workers_.emplace_back([this] { worker_loop(); });
}

TerrainMesher::~TerrainMesher() {
    {
        // SimulationThread (or whichever thread owns the mesher): drop every
        // waiting job and tell workers to stop once their current job (if
        // any) finishes.
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        paused_ = false;   // never let a paused test leave the destructor stuck
        waiting_.clear();
        heap_.clear();
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) worker.join();
}

void TerrainMesher::queue(std::uint64_t terrain, std::uint64_t revision, MeshInput input, float distance) {
    // Caller's thread (SimulationThread).
    const Key key{terrain, JobKind::Chunk, input.coord, NodeKey{}};
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t sequence = next_sequence_++;
    Job job;
    job.terrain = terrain;
    job.kind = JobKind::Chunk;
    job.coord = input.coord;
    job.revision = revision;
    job.mesh_input = std::move(input);
    job.distance = distance;
    job.sequence = sequence;
    // Overwrites a queued-but-not-yet-started job for this key in place; a
    // job already taken by a worker was erased from waiting_, so this starts
    // a fresh entry for after that one finishes. Either way the old heap
    // entry for this key (if any) is left behind as stale.
    waiting_[key] = std::move(job);
    heap_.push_back(HeapEntry{distance, sequence, key});
    std::push_heap(heap_.begin(), heap_.end(), HeapOrder{});
    cv_.notify_all();
}

void TerrainMesher::queue_node(std::uint64_t terrain, std::uint64_t revision, LodInput input, float distance) {
    // Caller's thread (SimulationThread). Shares waiting_/heap_/sequence_
    // with queue()'s chunk jobs, so the two kinds interleave by distance.
    const Key key{terrain, JobKind::Node, ChunkCoord{}, input.key};
    std::lock_guard<std::mutex> lock(mutex_);
    node_job_queued_ = true;
    const std::uint64_t sequence = next_sequence_++;
    Job job;
    job.terrain = terrain;
    job.kind = JobKind::Node;
    job.node = input.key;
    job.revision = revision;
    job.node_input = std::move(input);
    job.distance = distance;
    job.sequence = sequence;
    waiting_[key] = std::move(job);
    heap_.push_back(HeapEntry{distance, sequence, key});
    std::push_heap(heap_.begin(), heap_.end(), HeapOrder{});
    cv_.notify_all();
}

void TerrainMesher::collect(std::vector<MeshResult>& out) {
    // Caller's thread. A node job's result has nowhere to go through this
    // overload, so this asserts none was ever queued rather than risk
    // silently dropping one -- a caller that queues node jobs must switch to
    // collect(chunks, nodes) instead.
    std::lock_guard<std::mutex> lock(mutex_);
    assert(!node_job_queued_ && "collect(chunks only) used after queue_node(): call collect(chunks, nodes) instead");
    out.clear();
    out.swap(results_);
    out.erase(std::remove_if(out.begin(), out.end(), [](const MeshResult& r) { return r.failed; }), out.end());
}

void TerrainMesher::collect(std::vector<MeshResult>& chunks, std::vector<NodeResult>& nodes) {
    // Caller's thread.
    std::lock_guard<std::mutex> lock(mutex_);
    chunks.clear();
    chunks.swap(results_);
    nodes.clear();
    nodes.swap(node_results_);
}

bool TerrainMesher::idle() const {
    // Caller's thread. Both waiting_ and running_ are read under mutex_ so
    // this can't observe one updated and not the other.
    std::lock_guard<std::mutex> lock(mutex_);
    return waiting_.empty() && running_ == 0;
}

void TerrainMesher::wait_idle() {
    // Caller's thread (tests). wait(lock, pred) re-checks pred under mutex_
    // on every wake, so a notify_all that lands between our unlock-to-wait
    // and the next check is never missed: the predicate itself is the only
    // thing that decides whether to keep sleeping.
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return waiting_.empty() && running_ == 0; });
}

std::uint64_t TerrainMesher::failure_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_count_;
}

std::string TerrainMesher::last_failure() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_failure_;
}

void TerrainMesher::pause_for_test(bool paused) {
    // Caller's thread (tests only).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = paused;
    }
    cv_.notify_all();
}

bool TerrainMesher::take_job(Job& out) {
    // Worker thread, called with mutex_ already held. Pops heap entries
    // (nearest distance first) until it finds one that still matches the
    // live job for its key in waiting_ -- a mismatch (or a missing key)
    // means that entry was superseded or already taken, so it is dropped
    // (lazy deletion) and the loop tries the next one.
    while (!heap_.empty()) {
        std::pop_heap(heap_.begin(), heap_.end(), HeapOrder{});
        const HeapEntry entry = heap_.back();
        heap_.pop_back();
        auto it = waiting_.find(entry.key);
        if (it == waiting_.end() || it->second.sequence != entry.sequence) continue;
        out = std::move(it->second);
        waiting_.erase(it);
        ++running_;
        return true;
    }
    return false;
}

void TerrainMesher::worker_loop() {
    // Worker thread: take -> build (mesh+collider, or LOD node) -> push
    // result, forever until stopping_ is set.
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stopping_) return;
                if (!paused_ && take_job(job)) break;
                cv_.wait(lock);
            }
        }
        // The actual build happens with no lock held: this is the whole
        // point of the pool, and surface_nets/build_collider_/build_node (or
        // its test override) only touch this job's own immutable input.
        ChunkMesh mesh;
        std::shared_ptr<void> collider;
        LodResult node_result;
        bool ok = true;
        std::string failure_message;
        try {
            if (job.kind == JobKind::Chunk) {
                mesh = surface_nets(job.mesh_input);
                if (build_collider_) collider = build_collider_(mesh);
            } else {
                node_result = build_node_fn_ ? build_node_fn_(job.node_input) : build_node(job.node_input);
            }
        } catch (const std::exception& error) {
            // A throw must never cross back into worker_loop's caller (an
            // uncaught exception on a non-main thread is std::terminate): a
            // bad job comes back as a result flagged failed, with an empty
            // mesh (or LodResult), so the caller can stop waiting on it; the
            // chunk (or node) keeps whatever it had. running_ is still
            // decremented below, so the mesher never looks permanently busy.
            // The message is kept (not just discarded) so the owning thread
            // can report it once it notices failure_count_ rise.
            ok = false;
            failure_message = error.what();
        } catch (...) {
            ok = false;
            failure_message = "a non-std::exception";
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --running_;
            if (!ok) {
                mesh = ChunkMesh{};
                collider.reset();
                node_result = LodResult{};
                node_result.key = job.node;
                ++failure_count_;
                last_failure_ = std::move(failure_message);
            }
            if (job.kind == JobKind::Chunk) {
                results_.push_back(
                    MeshResult{job.terrain, job.coord, job.revision, std::move(mesh), std::move(collider), !ok});
            } else {
                node_results_.push_back(NodeResult{job.terrain, job.node, job.revision, std::move(node_result), !ok});
            }
        }
        cv_.notify_all();   // may have just made the mesher idle
    }
}

}  // namespace engine_core::terrain
