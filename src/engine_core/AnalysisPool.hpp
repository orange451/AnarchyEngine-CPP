#pragma once

#include "StackThread.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace engine_core {

// Threads that run script analysis tasks: parsing and linting a batch, and the
// type checks Luau's checkQueuedModules hands out. post() queues and returns at
// once, as checkQueuedModules wants of its executeTasks; run_all() queues and
// waits. Each thread has a stack as deep as Luau needs.
//
// A task must not throw. Luau's own task reports itself done only by returning.
// run_all must not be called from one of the pool's threads.
class AnalysisPool {
public:
    // At least one thread, whatever `threads` says.
    AnalysisPool(unsigned threads, std::size_t stack_bytes);
    // Drops tasks that have not started and joins the threads.
    ~AnalysisPool();
    AnalysisPool(const AnalysisPool&) = delete;
    AnalysisPool& operator=(const AnalysisPool&) = delete;

    unsigned size() const { return static_cast<unsigned>(threads_.size()); }
    void post(std::vector<std::function<void()>> tasks);
    void run_all(std::vector<std::function<void()>> tasks);

    // One fewer than the hardware threads, so the UI and simulation keep a core,
    // and at least one.
    static unsigned default_size();

private:
    void work();

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stop_ = false;
    std::vector<StackThread> threads_;
};

}  // namespace engine_core
