#include "Profiler.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace profiler {

namespace detail {
std::atomic<int> g_enabled{0};
}

namespace {

enum class Kind : std::uint8_t { Begin, End, Frame };

struct Event {
    std::uint64_t ns = 0;
    ScopeId scope = 0;
    CauseId cause = kNoCause;
    Kind kind = Kind::Begin;
};

struct Open {
    ScopeId scope = 0;
    CauseId cause = kNoCause;
    std::uint64_t start_ns = 0;
};

// One writer (its thread) and one reader (collect, under the history lock).
struct Ring {
    std::uint16_t row = 0;
    std::unique_ptr<Event[]> events{new Event[kRingEvents]};
    std::atomic<std::uint64_t> write{0};
    std::atomic<std::uint64_t> read{0};
    std::atomic<std::uint64_t> dropped{0};
    // Reader only: the scopes begun and not yet ended.
    std::vector<Open> stack;

    void push(const Event& event) {
        const std::uint64_t at = write.load(std::memory_order_relaxed);
        if (at - read.load(std::memory_order_acquire) >= kRingEvents) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        events[at % kRingEvents] = event;
        write.store(at + 1, std::memory_order_release);
    }
};

struct GpuPending {
    ScopeRecord record;
    int lag = 0;
};

std::uint64_t steady_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::atomic<std::uint64_t (*)()> g_clock{&steady_ns};

thread_local Ring* t_ring = nullptr;
// The scopes this thread has recorded a begin for and not yet ended, with the
// ring each began on, so its end goes there too.
thread_local std::vector<std::pair<ScopeId, Ring*>> t_open;
// Rings this thread made for RowScope, by row name.
thread_local std::vector<std::pair<std::string, Ring*>> t_rows;

constexpr const char* kFixedRows[] = {"Sim", "Render", "Render draw", "UI", "GPU"};
static_assert(sizeof(kFixedRows) / sizeof(kFixedRows[0]) == kFixedRowCount, "fixed rows");
constexpr std::uint16_t kGpuRow = 4;

class Recorder {
public:
    Recorder() {
        for (const char* row : kFixedRows) {
            rows_.emplace_back(row);
        }
    }

    // Names: interning, rows, and rings. Taken alone, or inside history_mu_.
    std::mutex names_mu_;
    std::vector<ScopeInfo> scopes_;
    std::unordered_map<std::string, ScopeId> by_name_;
    std::unordered_map<std::string, ScopeId> by_key_;
    std::vector<std::string> causes_;
    std::unordered_map<std::string, CauseId> cause_ids_;
    std::vector<std::string> rows_;
    std::vector<std::unique_ptr<Ring>> rings_;
    std::uint64_t names_version_ = 1;

    std::mutex gpu_mu_;
    std::vector<GpuPending> gpu_;

    // The histories, and what collect keeps between calls.
    std::mutex history_mu_;
    History live_;
    History frozen_;
    History capture_;
    bool paused_ = false;
    bool showing_capture_ = false;
    std::uint64_t synced_version_ = 0;
    std::vector<ScopeRecord> pending_;
    bool have_boundary_ = false;
    std::uint64_t last_boundary_ = 0;
    std::vector<std::uint64_t> boundaries_;

    void sync_names_locked() {
        std::lock_guard<std::mutex> names(names_mu_);
        if (synced_version_ == names_version_) {
            return;
        }
        live_.scopes = scopes_;
        live_.causes = causes_;
        live_.rows = rows_;
        synced_version_ = names_version_;
    }

    // Empties every ring and what was read from them. history_mu_ held.
    void clear_locked() {
        {
            std::lock_guard<std::mutex> names(names_mu_);
            for (const std::unique_ptr<Ring>& ring : rings_) {
                ring->read.store(ring->write.load(std::memory_order_acquire), std::memory_order_release);
                ring->stack.clear();
                ring->dropped.store(0, std::memory_order_relaxed);
            }
        }
        {
            std::lock_guard<std::mutex> gpu(gpu_mu_);
            gpu_.clear();
        }
        live_.frames.clear();
        live_.dropped = 0;
        live_.gpu_lag_frames = 0;
        pending_.clear();
        boundaries_.clear();
        have_boundary_ = false;
        last_boundary_ = 0;
    }

    void place(const ScopeRecord& record) {
        std::deque<Frame>& frames = live_.frames;
        // The first frame whose start is after the record's; the one before holds it.
        auto after = std::upper_bound(frames.begin(), frames.end(), record.start_ns,
                                      [](std::uint64_t start, const Frame& frame) { return start < frame.start_ns; });
        if (after != frames.begin()) {
            Frame& frame = *(after - 1);
            if (record.start_ns < frame.end_ns) {
                frame.scopes.push_back(record);
                return;
            }
        }
        // In the frame still open: placed once a boundary closes it.
        if (have_boundary_ && record.start_ns >= last_boundary_) {
            pending_.push_back(record);
        }
    }

    void drain(Ring& ring) {
        const std::uint64_t to = ring.write.load(std::memory_order_acquire);
        std::uint64_t from = ring.read.load(std::memory_order_relaxed);
        for (; from < to; ++from) {
            const Event event = ring.events[from % kRingEvents];
            switch (event.kind) {
                case Kind::Begin:
                    ring.stack.push_back({event.scope, event.cause, event.ns});
                    break;
                case Kind::End: {
                    // An end matches the nearest open scope with its id. One with no
                    // match, as after a drop or a begin made while off, is ignored.
                    std::size_t match = ring.stack.size();
                    while (match > 0 && ring.stack[match - 1].scope != event.scope) {
                        --match;
                    }
                    if (match == 0) {
                        break;
                    }
                    while (ring.stack.size() >= match) {
                        const Open open = ring.stack.back();
                        ring.stack.pop_back();
                        ScopeRecord record;
                        record.row = ring.row;
                        record.scope = open.scope;
                        record.cause = open.cause;
                        record.depth = static_cast<std::uint8_t>(std::min<std::size_t>(ring.stack.size(), 255));
                        record.start_ns = open.start_ns;
                        record.end_ns = std::max(event.ns, open.start_ns);
                        pending_.push_back(record);
                    }
                    break;
                }
                case Kind::Frame:
                    boundaries_.push_back(event.ns);
                    break;
            }
        }
        ring.read.store(to, std::memory_order_release);
    }

    void collect_locked() {
        std::vector<Ring*> rings;
        {
            std::lock_guard<std::mutex> names(names_mu_);
            for (const std::unique_ptr<Ring>& ring : rings_) {
                rings.push_back(ring.get());
            }
        }
        std::vector<ScopeRecord> records;
        records.swap(pending_);
        for (Ring* ring : rings) {
            drain(*ring);
        }
        records.insert(records.end(), pending_.begin(), pending_.end());
        pending_.clear();
        {
            std::lock_guard<std::mutex> gpu(gpu_mu_);
            for (const GpuPending& late : gpu_) {
                records.push_back(late.record);
                live_.gpu_lag_frames = late.lag;
            }
            gpu_.clear();
        }
        std::sort(boundaries_.begin(), boundaries_.end());
        for (std::uint64_t boundary : boundaries_) {
            if (have_boundary_ && boundary > last_boundary_) {
                Frame frame;
                frame.start_ns = last_boundary_;
                frame.end_ns = boundary;
                live_.frames.push_back(std::move(frame));
            }
            if (!have_boundary_ || boundary > last_boundary_) {
                last_boundary_ = boundary;
                have_boundary_ = true;
            }
        }
        boundaries_.clear();
        while (live_.frames.size() > kHistoryFrames) {
            live_.frames.pop_front();
        }
        for (const ScopeRecord& record : records) {
            place(record);
        }
        std::uint64_t dropped = 0;
        for (Ring* ring : rings) {
            dropped += ring->dropped.load(std::memory_order_relaxed);
        }
        live_.dropped = dropped;
        sync_names_locked();
    }
};

Recorder& recorder() {
    static Recorder* instance = new Recorder();
    return *instance;
}

std::string cut_name(std::string_view name) {
    if (name.size() <= kMaxNameBytes) {
        return std::string(name);
    }
    std::size_t size = kMaxNameBytes;
    // Back up off the middle of a UTF-8 sequence.
    while (size > 0 && (static_cast<unsigned char>(name[size]) & 0xC0) == 0x80) {
        --size;
    }
    return std::string(name.substr(0, size));
}

}  // namespace

void acquire() {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.history_mu_);
    if (detail::g_enabled.load() == 0) {
        r.clear_locked();
    }
    detail::g_enabled.fetch_add(1);
}

void release() {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.history_mu_);
    if (detail::g_enabled.load() > 0) {
        detail::g_enabled.fetch_sub(1);
    }
}

ScopeId intern(std::string_view name, Group group) {
    Recorder& r = recorder();
    const std::string cut = cut_name(name);
    std::string key(1, static_cast<char>('0' + static_cast<int>(group)));
    key += cut;
    std::lock_guard<std::mutex> lock(r.names_mu_);
    const auto found = r.by_name_.find(key);
    if (found != r.by_name_.end()) {
        return found->second;
    }
    const ScopeId id = static_cast<ScopeId>(r.scopes_.size());
    r.scopes_.push_back({cut, group, std::string()});
    r.by_name_.emplace(std::move(key), id);
    ++r.names_version_;
    return id;
}

ScopeId intern_keyed(std::string_view key, std::string_view name, Group group) {
    Recorder& r = recorder();
    const std::string cut = cut_name(name);
    std::string full(1, static_cast<char>('0' + static_cast<int>(group)));
    full += key;
    std::lock_guard<std::mutex> lock(r.names_mu_);
    const auto found = r.by_key_.find(full);
    if (found != r.by_key_.end()) {
        ScopeInfo& info = r.scopes_[found->second];
        if (info.name != cut) {
            info.name = cut;
            ++r.names_version_;
        }
        return found->second;
    }
    const ScopeId id = static_cast<ScopeId>(r.scopes_.size());
    r.scopes_.push_back({cut, group, std::string(key)});
    r.by_key_.emplace(std::move(full), id);
    ++r.names_version_;
    return id;
}

CauseId intern_cause(std::string_view cause) {
    Recorder& r = recorder();
    const std::string cut = cut_name(cause);
    std::lock_guard<std::mutex> lock(r.names_mu_);
    const auto found = r.cause_ids_.find(cut);
    if (found != r.cause_ids_.end()) {
        return found->second;
    }
    if (r.causes_.size() >= kNoCause) {
        return kNoCause;
    }
    const CauseId id = static_cast<CauseId>(r.causes_.size());
    r.causes_.push_back(cut);
    r.cause_ids_.emplace(cut, id);
    ++r.names_version_;
    return id;
}

namespace {

// A new ring on the row named row, for the calling thread to write.
Ring* make_ring(const std::string& name) {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.names_mu_);
    auto ring = std::make_unique<Ring>();
    const auto existing = std::find(r.rows_.begin(), r.rows_.end(), name);
    if (existing != r.rows_.end()) {
        ring->row = static_cast<std::uint16_t>(existing - r.rows_.begin());
    } else {
        ring->row = static_cast<std::uint16_t>(r.rows_.size());
        r.rows_.push_back(name);
        ++r.names_version_;
    }
    Ring* made = ring.get();
    r.rings_.push_back(std::move(ring));
    return made;
}

}  // namespace

void register_thread(const char* row) { t_ring = make_ring(row != nullptr ? row : "Thread"); }

RowScope::RowScope(const char* row) : previous_(t_ring) {
    const std::string name = row != nullptr ? row : "Thread";
    for (const auto& [held, ring] : t_rows) {
        if (held == name) {
            t_ring = ring;
            return;
        }
    }
    t_ring = make_ring(name);
    t_rows.emplace_back(name, t_ring);
}

RowScope::~RowScope() { t_ring = static_cast<Ring*>(previous_); }

void begin(ScopeId scope, CauseId cause) {
    if (!enabled() || t_ring == nullptr) {
        return;
    }
    t_open.emplace_back(scope, t_ring);
    t_ring->push({now_ns(), scope, cause, Kind::Begin});
}

void end() {
    // Ends what this thread began, even after recording stopped, so a begin
    // never pairs with a later scope's end. An end with nothing begun, as for a
    // begin made while off, records nothing.
    if (t_open.empty()) {
        return;
    }
    const auto [scope, ring] = t_open.back();
    t_open.pop_back();
    ring->push({now_ns(), scope, kNoCause, Kind::End});
}

void frame_boundary() {
    if (!enabled() || t_ring == nullptr) {
        return;
    }
    t_ring->push({now_ns(), 0, kNoCause, Kind::Frame});
}

void gpu_scope(ScopeId scope, std::uint8_t depth, std::uint64_t start_ns, std::uint64_t end_ns, int lag_frames) {
    if (!enabled()) {
        return;
    }
    Recorder& r = recorder();
    GpuPending late;
    late.record.row = kGpuRow;
    late.record.scope = scope;
    late.record.depth = depth;
    late.record.start_ns = start_ns;
    late.record.end_ns = std::max(start_ns, end_ns);
    late.lag = lag_frames;
    std::lock_guard<std::mutex> lock(r.gpu_mu_);
    r.gpu_.push_back(late);
}

std::uint64_t now_ns() { return g_clock.load(std::memory_order_relaxed)(); }

void set_clock_for_testing(std::uint64_t (*clock)()) { g_clock.store(clock != nullptr ? clock : &steady_ns); }

void reset_for_testing() {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.history_mu_);
    r.clear_locked();
    r.paused_ = false;
    r.showing_capture_ = false;
    r.frozen_ = History();
    r.capture_ = History();
    detail::g_enabled.store(0);
}

void collect() {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.history_mu_);
    r.collect_locked();
}

void set_paused(bool paused) {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.history_mu_);
    if (paused && !r.paused_) {
        r.sync_names_locked();
        r.frozen_ = r.live_;
    }
    if (!paused) {
        r.showing_capture_ = false;
        r.capture_ = History();
        r.frozen_ = History();
    }
    r.paused_ = paused;
}

bool paused() {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.history_mu_);
    return r.paused_;
}

void show_capture(History capture) {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.history_mu_);
    r.capture_ = std::move(capture);
    r.showing_capture_ = true;
    r.paused_ = true;
}

void close_capture() {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.history_mu_);
    r.capture_ = History();
    r.showing_capture_ = false;
    r.frozen_ = History();
    r.paused_ = false;
}

bool showing_capture() {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.history_mu_);
    return r.showing_capture_;
}

void with_view(const std::function<void(const History&)>& fn) {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.history_mu_);
    r.sync_names_locked();
    if (r.showing_capture_) {
        fn(r.capture_);
    } else if (r.paused_) {
        fn(r.frozen_);
    } else {
        fn(r.live_);
    }
}

void with_live(const std::function<void(const History&)>& fn) {
    Recorder& r = recorder();
    std::lock_guard<std::mutex> lock(r.history_mu_);
    r.sync_names_locked();
    fn(r.live_);
}

}  // namespace profiler
