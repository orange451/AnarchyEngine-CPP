#include "GpuTimer.hpp"

#include "gl.hpp"

namespace runner {

namespace {

// Two queries a pass: a dozen passes a frame for several frames in flight.
constexpr int kQueries = 256;
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

unsigned GpuTimer::take() {
    if (free_.empty()) {
        return 0;
    }
    const unsigned query = free_.back();
    free_.pop_back();
    return query;
}

void GpuTimer::begin(profiler::ScopeId scope) {
    Open open;
    open.scope = scope;
    if (available() && profiler::enabled() && free_.size() >= 2) {
        open.query = take();
        open.timed = true;
        glQueryCounter(open.query, GL_TIMESTAMP);
    }
    open_.push_back(open);
}

void GpuTimer::end() {
    if (open_.empty()) {
        return;
    }
    const Open open = open_.back();
    open_.pop_back();
    if (!open.timed) {
        return;
    }
    const unsigned query = take();
    if (query == 0) {
        free_.push_back(open.query);
        return;
    }
    glQueryCounter(query, GL_TIMESTAMP);
    Pending pending;
    pending.scope = open.scope;
    pending.depth = static_cast<std::uint8_t>(open_.size());
    pending.begin = open.query;
    pending.end = query;
    pending.frame = frame_;
    pending_.push_back(pending);
}

void GpuTimer::frame() {
    ++frame_;
    if (!available() || pending_.empty()) {
        return;
    }
    // The GPU clock against this process's, once a frame.
    GLint64 gpu_now = 0;
    glGetInteger64v(GL_TIMESTAMP, &gpu_now);
    const std::int64_t offset =
        static_cast<std::int64_t>(profiler::now_ns()) - static_cast<std::int64_t>(gpu_now);
    // Queries finish in order, so the first one not ready ends the read.
    while (!pending_.empty()) {
        const Pending& pending = pending_.front();
        GLint ready = 0;
        glGetQueryObjectiv(pending.end, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready == 0 && frame_ - pending.frame < kGiveUpFrames) {
            break;
        }
        if (ready != 0) {
            GLuint64 start = 0;
            GLuint64 stop = 0;
            glGetQueryObjectui64v(pending.begin, GL_QUERY_RESULT, &start);
            glGetQueryObjectui64v(pending.end, GL_QUERY_RESULT, &stop);
            const std::int64_t from = static_cast<std::int64_t>(start) + offset;
            const std::int64_t to = static_cast<std::int64_t>(stop) + offset;
            if (from > 0 && to >= from) {
                profiler::gpu_scope(pending.scope, pending.depth, static_cast<std::uint64_t>(from),
                                    static_cast<std::uint64_t>(to), static_cast<int>(frame_ - pending.frame));
            }
        }
        free_.push_back(pending.begin);
        free_.push_back(pending.end);
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
