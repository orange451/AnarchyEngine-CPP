#pragma once

#include "profiler/Profiler.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace runner {

// Times GPU passes for the profiler's GPU row. begin and end bracket a pass with
// a GL_TIME_ELAPSED query; frame, once per drawn frame, reads the queries the GPU
// has finished. It never waits on the GPU, so results arrive a frame or more
// late. Elapsed time rather than timestamps, since macOS's OpenGL answers every
// GL_TIMESTAMP query with 0: each pass is placed on the row after the moment the
// CPU issued it and after the pass before it, for as long as the GPU took.
// Elapsed queries cannot nest, so a pass begun inside a timed one is not timed.
// Nothing is issued while the profiler is not recording, or when the context
// has no timer queries. Every call needs the context that init ran on.
class GpuTimer {
public:
    GpuTimer() = default;
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    // False, and every call a no-op, when the context has no timer queries.
    bool init();
    bool available() const { return !pool_.empty(); }
    void begin(profiler::ScopeId scope);
    void end();
    void frame();
    // Deletes the queries. The context that init ran on is current.
    void shutdown();

private:
    struct Open {
        profiler::ScopeId scope = 0;
        unsigned query = 0;
        std::uint64_t issued_ns = 0;
    };
    struct Pending {
        profiler::ScopeId scope = 0;
        unsigned query = 0;
        std::uint64_t issued_ns = 0;
        std::uint64_t frame = 0;
    };

    std::vector<unsigned> pool_;
    std::vector<unsigned> free_;
    std::vector<Open> open_;
    std::deque<Pending> pending_;
    std::uint64_t frame_ = 0;
    // Where the last placed pass ended, so passes never overlap on the row.
    std::uint64_t gpu_cursor_ns_ = 0;
};

}  // namespace runner
