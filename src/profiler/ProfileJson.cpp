#include "profiler/ProfileJson.hpp"

#include "profiler/ProfileStats.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace profiler {

using engine_core::JsonValue;

namespace {

constexpr const char* kGroupNames[] = {"engine", "physics", "render", "script", "user", "gpu"};
constexpr std::size_t kTreeDepth = 6;
constexpr double kTreeMinMs = 0.05;

const char* group_name(Group group) { return kGroupNames[static_cast<int>(group)]; }

double round3(double value) { return std::round(value * 1000.0) / 1000.0; }

// Microseconds from base, to the nanosecond, so a frame of under a microsecond keeps its length.
double micros(std::uint64_t ns, std::uint64_t base) { return static_cast<double>(ns - base) / 1000.0; }

}  // namespace

std::string write_capture(const History& history, const std::string& place, const std::string& created_utc) {
    const std::uint64_t base = history.frames.empty() ? 0 : history.frames.front().start_ns;
    JsonValue root = JsonValue::object();
    root.set("format", JsonValue::string(kCaptureFormat));
    root.set("version", JsonValue::number(kCaptureVersion));
    root.set("place", JsonValue::string(place));
    root.set("created", JsonValue::string(created_utc));
    root.set("gpu_lag_frames", JsonValue::number(history.gpu_lag_frames));
    root.set("dropped", JsonValue::number(static_cast<double>(history.dropped)));
    root.set("budget_ms", JsonValue::number(kBudgetMs));
    root.set("over_budget_ms", JsonValue::number(kOverBudgetMs));
    JsonValue threads = JsonValue::array();
    for (const std::string& row : history.rows) {
        threads.items().push_back(JsonValue::string(row));
    }
    root.set("threads", std::move(threads));
    JsonValue scopes = JsonValue::array();
    for (const ScopeInfo& info : history.scopes) {
        JsonValue scope = JsonValue::object();
        scope.set("name", JsonValue::string(info.name));
        scope.set("group", JsonValue::string(group_name(info.group)));
        if (!info.key.empty()) {
            scope.set("script", JsonValue::string(info.key));
        }
        scopes.items().push_back(std::move(scope));
    }
    root.set("scopes", std::move(scopes));
    JsonValue causes = JsonValue::array();
    for (const std::string& cause : history.causes) {
        causes.items().push_back(JsonValue::string(cause));
    }
    root.set("causes", std::move(causes));
    JsonValue frames = JsonValue::array();
    JsonValue events = JsonValue::array();
    for (const Frame& frame : history.frames) {
        frames.items().push_back(
            JsonValue::array({JsonValue::number(micros(frame.start_ns, base)), JsonValue::number(micros(frame.end_ns, base))}));
        for (const ScopeRecord& record : frame.scopes) {
            const double start = micros(record.start_ns, base);
            const double end = micros(record.end_ns, base);
            events.items().push_back(JsonValue::array(
                {JsonValue::number(record.row), JsonValue::number(record.scope), JsonValue::number(record.depth),
                 JsonValue::number(start), JsonValue::number(std::max(0.0, end - start)),
                 record.cause == kNoCause ? JsonValue() : JsonValue::number(record.cause)}));
        }
    }
    root.set("frames", std::move(frames));
    root.set("events", std::move(events));
    return engine_core::compact_json(root);
}

namespace {

struct TreeNode {
    const ScopeRecord* record = nullptr;
    std::vector<TreeNode> children;
};

JsonValue tree_json(const TreeNode& node, const History& history, std::size_t depth) {
    const ScopeRecord& record = *node.record;
    JsonValue out = JsonValue::object();
    out.set("name", JsonValue::string(record.scope < history.scopes.size() ? history.scopes[record.scope].name : "?"));
    out.set("ms", JsonValue::number(round3(static_cast<double>(record.end_ns - record.start_ns) / 1e6)));
    if (record.cause != kNoCause && record.cause < history.causes.size()) {
        out.set("cause", JsonValue::string(history.causes[record.cause]));
    }
    JsonValue children = JsonValue::array();
    if (depth + 1 < kTreeDepth) {
        for (const TreeNode& child : node.children) {
            if (static_cast<double>(child.record->end_ns - child.record->start_ns) / 1e6 >= kTreeMinMs) {
                children.items().push_back(tree_json(child, history, depth + 1));
            }
        }
    }
    out.set("children", std::move(children));
    return out;
}

}  // namespace

JsonValue build_report(const History& history, const ReportOptions& options) {
    JsonValue out = JsonValue::object();
    out.set("frames", JsonValue::number(static_cast<double>(history.frames.size())));
    out.set("budget_ms", JsonValue::number(kBudgetMs));
    out.set("gpu_lag_frames", JsonValue::number(history.gpu_lag_frames));
    out.set("dropped_events", JsonValue::number(static_cast<double>(history.dropped)));
    std::vector<double> lengths;
    std::size_t slowest = 0;
    int over = 0;
    double total = 0;
    for (std::size_t index = 0; index < history.frames.size(); ++index) {
        const double ms = frame_ms(history.frames[index]);
        lengths.push_back(ms);
        total += ms;
        over += ms > kOverBudgetMs ? 1 : 0;
        if (ms > lengths[slowest]) {
            slowest = index;
        }
    }
    JsonValue times = JsonValue::object();
    if (!lengths.empty()) {
        std::vector<double> sorted = lengths;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t p95 = std::min(sorted.size() - 1,
                                         static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(sorted.size()))) - 1);
        times.set("avg", JsonValue::number(round3(total / static_cast<double>(lengths.size()))));
        times.set("p95", JsonValue::number(round3(sorted[p95])));
        times.set("max", JsonValue::number(round3(sorted.back())));
    }
    out.set("frame_ms", std::move(times));
    out.set("over_budget", JsonValue::number(over));
    std::vector<ScopeStat> stats = compute_stats(history, slowest);
    sort_stats(stats, history, StatSort::Max);
    JsonValue rows = JsonValue::array();
    for (std::size_t index = 0; index < stats.size() && static_cast<int>(index) < std::max(options.top, 0); ++index) {
        const ScopeStat& stat = stats[index];
        JsonValue row = JsonValue::object();
        const ScopeInfo* info = stat.scope < history.scopes.size() ? &history.scopes[stat.scope] : nullptr;
        row.set("name", JsonValue::string(info != nullptr ? info->name : "?"));
        row.set("group", JsonValue::string(info != nullptr ? group_name(info->group) : "engine"));
        row.set("thread", JsonValue::string(stat.row < history.rows.size() ? history.rows[stat.row] : "?"));
        if (stat.cause != kNoCause && stat.cause < history.causes.size()) {
            row.set("cause", JsonValue::string(history.causes[stat.cause]));
        }
        row.set("max_ms", JsonValue::number(round3(stat.max_ms)));
        row.set("avg_ms", JsonValue::number(round3(stat.avg_ms)));
        row.set("calls_per_frame", JsonValue::number(round3(stat.calls_per_frame)));
        row.set("share", JsonValue::number(round3(stat.share)));
        rows.items().push_back(std::move(row));
    }
    out.set("scopes", std::move(rows));
    if (options.include_timeline && !history.frames.empty()) {
        const Frame& frame = history.frames[slowest];
        JsonValue slow = JsonValue::object();
        slow.set("index", JsonValue::number(static_cast<double>(slowest)));
        slow.set("ms", JsonValue::number(round3(frame_ms(frame))));
        JsonValue threads = JsonValue::array();
        for (std::size_t row = 0; row < history.rows.size(); ++row) {
            std::vector<const ScopeRecord*> mine;
            for (const ScopeRecord& record : frame.scopes) {
                if (record.row == row) {
                    mine.push_back(&record);
                }
            }
            if (mine.empty()) {
                continue;
            }
            std::sort(mine.begin(), mine.end(), [](const ScopeRecord* a, const ScopeRecord* b) {
                return a->start_ns != b->start_ns ? a->start_ns < b->start_ns : a->depth < b->depth;
            });
            // Rebuild nesting from depth: each record's parent is the nearest open one a level up.
            std::vector<TreeNode> roots;
            std::vector<std::vector<TreeNode>*> levels;
            for (const ScopeRecord* record : mine) {
                std::size_t depth = std::min<std::size_t>(record->depth, levels.size());
                levels.resize(depth);
                std::vector<TreeNode>& into = depth == 0 ? roots : levels[depth - 1]->back().children;
                into.push_back(TreeNode{record, {}});
                levels.push_back(&into);
            }
            JsonValue thread = JsonValue::object();
            thread.set("thread", JsonValue::string(history.rows[row]));
            JsonValue list = JsonValue::array();
            for (const TreeNode& node : roots) {
                if (static_cast<double>(node.record->end_ns - node.record->start_ns) / 1e6 >= kTreeMinMs) {
                    list.items().push_back(tree_json(node, history, 0));
                }
            }
            thread.set("scopes", std::move(list));
            threads.items().push_back(std::move(thread));
        }
        slow.set("threads", std::move(threads));
        out.set("slowest_frame", std::move(slow));
    }
    return out;
}

std::string capture_file_name(std::time_t when) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &when);
#else
    localtime_r(&when, &local);
#endif
    char text[64];
    std::strftime(text, sizeof text, "profile-%Y%m%d-%H%M%S.html", &local);
    return text;
}

std::string utc_stamp(std::time_t when) {
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &when);
#else
    gmtime_r(&when, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

}  // namespace profiler
