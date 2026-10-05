#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The frame profiler's recorder. Each thread that records registers once and
// gets a ring of its own, so recording never takes a lock: a scope is two clock
// reads and two writes into that ring. collect() drains every ring into a
// history of the last kHistoryFrames frames, a frame being the time from one
// frame_boundary() to the next (the render thread marks one at each Prepare).
// Other threads' scopes go into the frame in which they began.
//
// Nothing records until someone asks: acquire() turns recording on and
// release() turns it off once every acquire is matched. While off, a scope
// costs one relaxed atomic load.
namespace profiler {

enum class Group : std::uint8_t { Engine, Physics, Render, Script, User, Gpu };

using ScopeId = std::uint32_t;
using CauseId = std::uint16_t;
inline constexpr CauseId kNoCause = 0xffff;
inline constexpr std::size_t kRingEvents = 65536;
inline constexpr std::size_t kHistoryFrames = 300;
inline constexpr std::size_t kMaxNameBytes = 64;
// Sim, Render, Render draw, UI, and GPU: the rows every history has, in order.
inline constexpr std::size_t kFixedRowCount = 5;
// A 60 Hz frame's budget, and the length past which a frame counts as over it:
// a little slack, so vsync's jitter around 16.7 ms is not called slow.
inline constexpr double kBudgetMs = 16.6;
inline constexpr double kOverBudgetMs = 17.5;

struct ScopeInfo {
    std::string name;
    Group group = Group::Engine;
    // A Script scope's GUID, which keeps its id through a rename. Empty otherwise.
    std::string key;
};

struct ScopeRecord {
    std::uint16_t row = 0;
    ScopeId scope = 0;
    CauseId cause = kNoCause;
    std::uint8_t depth = 0;
    std::uint64_t start_ns = 0;
    std::uint64_t end_ns = 0;
};

struct Frame {
    std::uint64_t start_ns = 0;
    std::uint64_t end_ns = 0;
    std::vector<ScopeRecord> scopes;
};

struct History {
    // Sim, Render, Render draw, UI, and GPU always, then other threads in the order they registered.
    std::vector<std::string> rows;
    // Oldest first, at most kHistoryFrames.
    std::deque<Frame> frames;
    // By ScopeId and CauseId.
    std::vector<ScopeInfo> scopes;
    std::vector<std::string> causes;
    // Events the rings had no room for since recording last started.
    std::uint64_t dropped = 0;
    // How many frames late the GPU row's latest results arrived.
    int gpu_lag_frames = 0;
};

namespace detail {
extern std::atomic<int> g_enabled;
}

inline bool enabled() { return detail::g_enabled.load(std::memory_order_relaxed) > 0; }

// Any thread. Recording runs while acquires outnumber releases. The first
// acquire starts a new history.
void acquire();
void release();

// Any thread. A name is cut to kMaxNameBytes, on a UTF-8 boundary.
ScopeId intern(std::string_view name, Group group);
// One id per key, such as a Script's GUID. A new name renames it.
ScopeId intern_keyed(std::string_view key, std::string_view name, Group group);
CauseId intern_cause(std::string_view cause);

// Once per thread, before it records. Several threads may share a row, one after another.
void register_thread(const char* row);
void begin(ScopeId scope, CauseId cause = kNoCause);
void end();
// The render thread, at the start of each Prepare.
void frame_boundary();
// A GPU pass that ran from start_ns to end_ns on this process's clock, read
// lag_frames frames after it was issued.
void gpu_scope(ScopeId scope, std::uint8_t depth, std::uint64_t start_ns, std::uint64_t end_ns, int lag_frames);

// Whether the renderer times each pass on the GPU, rather than the whole 3D
// draw once a frame. Off by default: on macOS's OpenGL every timed pass stalls
// the CPU and inflates what it measures, so per-pass detail costs the frame.
bool gpu_detail();
void set_gpu_detail(bool detail);

std::uint64_t now_ns();
void set_clock_for_testing(std::uint64_t (*clock)());
// Empties the rings and the history and turns recording off. Ids stay.
void reset_for_testing();

// Any thread; calls are serialized. Drains the rings into the live history.
void collect();

// Pausing freezes what with_view shows; recording goes on into the live history.
void set_paused(bool paused);
bool paused();

// fn sees the history under the profiler's lock, so it must not call back into
// the profiler. with_view gives the frozen history while paused, and the live
// one otherwise.
void with_view(const std::function<void(const History&)>& fn);
void with_live(const std::function<void(const History&)>& fn);

// Until destroyed, this thread's scopes go on another row, as the UI thread's 3D
// draw goes on "Render draw" to sit in the Render section. Scopes begun before
// it still end on their own row.
class RowScope {
public:
    explicit RowScope(const char* row);
    ~RowScope();
    RowScope(const RowScope&) = delete;
    RowScope& operator=(const RowScope&) = delete;

private:
    void* previous_;
};

// Records from construction to destruction, when recording was on at construction.
class Scope {
public:
    explicit Scope(ScopeId scope, CauseId cause = kNoCause) : active_(enabled()) {
        if (active_) {
            begin(scope, cause);
        }
    }
    ~Scope() {
        if (active_) {
            end();
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool active_;
};

}  // namespace profiler

#define PROFILER_JOIN2(a, b) a##b
#define PROFILER_JOIN(a, b) PROFILER_JOIN2(a, b)
// Times the rest of the enclosing block. The name is interned once per call site.
#define PROFILE_SCOPE(name, group)                                                                         \
    static const ::profiler::ScopeId PROFILER_JOIN(profiler_id_, __LINE__) = ::profiler::intern(name, group); \
    ::profiler::Scope PROFILER_JOIN(profiler_scope_, __LINE__)(PROFILER_JOIN(profiler_id_, __LINE__))
