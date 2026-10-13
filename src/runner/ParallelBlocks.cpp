#include "ParallelBlocks.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace runner {
namespace {

// Set while a thread runs a block, so a RunBlocks inside one runs inline
// instead of waiting on the pool it is part of.
thread_local bool tInBlock = false;

class WorkerPool {
public:
    WorkerPool() {
        // Two cores left for the engine and the rest of the process; the
        // thread that calls RunBlocks works as well.
        const unsigned hardware = std::thread::hardware_concurrency();
        const unsigned workers = hardware > 2 ? hardware - 2 : 0;
        threads_.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) {
            threads_.emplace_back([this] { work(); });
        }
    }

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (std::thread& thread : threads_) {
            thread.join();
        }
    }

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    unsigned workers() const { return static_cast<unsigned>(threads_.size()); }

    void run(std::size_t blocks, const std::function<void(std::size_t)>& body) {
        if (threads_.empty() || blocks <= 1 || tInBlock) {
            RunAll(blocks, body);
            return;
        }
        // One loop at a time: a second caller waits its turn.
        std::lock_guard<std::mutex> turn(turn_);
        Job job{&body, blocks};
        {
            std::lock_guard<std::mutex> guard(mutex_);
            current_ = &job;
            ++jobNumber_;
        }
        wake_.notify_all();
        Take(job);
        // No worker joins now; wait for the ones that did to finish their blocks.
        std::unique_lock<std::mutex> lock(mutex_);
        current_ = nullptr;
        done_.wait(lock, [this] { return holders_ == 0; });
    }

private:
    struct Job {
        const std::function<void(std::size_t)>* body = nullptr;
        std::size_t blocks = 0;
        std::atomic<std::size_t> next{0};

        Job(const std::function<void(std::size_t)>* run, std::size_t count) : body(run), blocks(count) {}
    };

    static void RunAll(std::size_t blocks, const std::function<void(std::size_t)>& body) {
        for (std::size_t block = 0; block < blocks; ++block) {
            body(block);
        }
    }

    // Runs blocks of job until none is left to take.
    static void Take(Job& job) {
        const bool was = tInBlock;
        tInBlock = true;
        for (std::size_t block = job.next.fetch_add(1); block < job.blocks; block = job.next.fetch_add(1)) {
            (*job.body)(block);
        }
        tInBlock = was;
    }

    void work() {
        std::uint64_t joined = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return stop_ || (current_ != nullptr && jobNumber_ != joined); });
            if (stop_) {
                return;
            }
            Job* job = current_;
            joined = jobNumber_;
            ++holders_;
            lock.unlock();
            Take(*job);
            lock.lock();
            if (--holders_ == 0) {
                done_.notify_all();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::mutex turn_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    // Under mutex_: the loop being run, its number, and how many workers hold it.
    Job* current_ = nullptr;
    std::uint64_t jobNumber_ = 0;
    int holders_ = 0;
    bool stop_ = false;
};

WorkerPool& Pool() {
    static WorkerPool pool;
    return pool;
}

}  // namespace

void RunBlocks(std::size_t blocks, const std::function<void(std::size_t)>& body) { Pool().run(blocks, body); }

unsigned BlockWorkers() { return Pool().workers(); }

}  // namespace runner
