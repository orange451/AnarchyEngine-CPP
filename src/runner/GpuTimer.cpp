#include "GpuTimer.hpp"

#include "gl.hpp"

#include <algorithm>

namespace runner {

namespace {

// One query a pass: a dozen passes a frame for several frames in flight.
constexpr int kQueries = 128;
// A pass the GPU has not finished this many frames later is given up on.
constexpr std::uint64_t kGiveUpFrames = 16;

}  // namespace

bool GpuTimer::init() {
    if (!pool_.empty()) {
        return true;
    }
    if (!GlTimerQueries()) {
        return false;
    }
    pool_.resize(kQueries);
    glGenQueries(kQueries, pool_.data());
    free_ = pool_;
    return true;
}

void GpuTimer::begin(profiler::ScopeId scope, bool timed) {
    Open open;
    open.scope = scope;
    // One elapsed query at a time: a pass inside a timed one goes untimed.
    const bool nested = std::any_of(open_.begin(), open_.end(), [](const Open& outer) { return outer.query != 0; });
    if (timed && available() && profiler::enabled() && !nested && !free_.empty()) {
        open.query = free_.back();
        free_.pop_back();
        open.issued_ns = profiler::now_ns();
        glBeginQuery(GL_TIME_ELAPSED, open.query);
    }
    open_.push_back(open);
}

void GpuTimer::end() {
    if (open_.empty()) {
        return;
    }
    const Open open = open_.back();
    open_.pop_back();
    if (open.query == 0) {
        return;
    }
    glEndQuery(GL_TIME_ELAPSED);
    Pending pending;
    pending.scope = open.scope;
    pending.query = open.query;
    pending.issued_ns = open.issued_ns;
    pending.frame = frame_;
    pending_.push_back(pending);
}

void GpuTimer::frame() {
    ++frame_;
    // Queries finish in order, so the first one not ready ends the read.
    while (available() && !pending_.empty()) {
        const Pending& pending = pending_.front();
        GLint ready = 0;
        glGetQueryObjectiv(pending.query, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready == 0 && frame_ - pending.frame < kGiveUpFrames) {
            break;
        }
        if (ready != 0) {
            GLuint64 elapsed = 0;
            glGetQueryObjectui64v(pending.query, GL_QUERY_RESULT, &elapsed);
            const std::uint64_t start = std::max(pending.issued_ns, gpu_cursor_ns_);
            gpu_cursor_ns_ = start + elapsed;
            profiler::gpu_scope(pending.scope, 0, start, gpu_cursor_ns_, static_cast<int>(frame_ - pending.frame));
        }
        free_.push_back(pending.query);
        pending_.pop_front();
    }
}

void GpuTimer::shutdown() {
    if (!pool_.empty()) {
        glDeleteQueries(static_cast<GLsizei>(pool_.size()), pool_.data());
    }
    pool_.clear();
    free_.clear();
    open_.clear();
    pending_.clear();
}

}  // namespace runner
