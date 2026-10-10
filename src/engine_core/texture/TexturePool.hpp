#pragma once

// TexturePool: the worker threads every texture load shares (terrain layers
// and mesh textures alike), running jobs by priority so what makes terrain
// stop being blank always runs first. Jobs never touch GL; they hand their
// results to whoever uploads them.

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace engine_core::texture {

// Lower runs first; FIFO within one. Every terrain stage comes before the
// mesh stage it matches, and every small read before any full bake.
enum class JobPriority : int {
    TerrainPreview = 0,
    TerrainSmallLevels = 1,
    MeshSmallLevels = 2,
    TerrainBake = 3,
    TerrainLargeLevels = 4,
    MeshBake = 5,
    MeshLargeLevels = 6,
};

class TexturePool {
public:
    // threads 0: max(1, hardware_concurrency - 2), leaving the simulation and
    // render threads a core each.
    explicit TexturePool(int threads = 0);
    // Finishes the jobs running now and drops the ones still queued.
    ~TexturePool();

    TexturePool(const TexturePool&) = delete;
    TexturePool& operator=(const TexturePool&) = delete;

    // Any thread.
    void submit(JobPriority priority, std::function<void()> job);
    // Blocks until nothing is queued or running. Tests.
    void wait_idle();
    int thread_count() const { return static_cast<int>(threads_.size()); }
    // Heard on submit's thread with each job's priority, before it is queued. Tests.
    void set_on_submit(std::function<void(JobPriority)> on_submit);

    // The process's pool, made on first use.
    static TexturePool& shared();

private:
    struct Job {
        int priority = 0;
        std::uint64_t sequence = 0;
        std::function<void()> run;
        bool operator<(const Job& other) const {
            // std::priority_queue puts the largest on top: invert both keys.
            if (priority != other.priority) return priority > other.priority;
            return sequence > other.sequence;
        }
    };

    void work();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::priority_queue<Job> queue_;
    std::uint64_t next_sequence_ = 0;
    int running_ = 0;
    bool stopping_ = false;
    std::function<void(JobPriority)> on_submit_;
    std::vector<std::thread> threads_;
};

}  // namespace engine_core::texture
