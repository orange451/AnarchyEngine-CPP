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

bool group_from(const std::string& name, Group& out) {
    for (int index = 0; index < 6; ++index) {
        if (name == kGroupNames[index]) {
            out = static_cast<Group>(index);
            return true;
        }
    }
    return false;
}

double round3(double value) { return std::round(value * 1000.0) / 1000.0; }

double micros(std::uint64_t ns, std::uint64_t base) {
    return std::round(static_cast<double>(ns - base) / 1000.0);
}

bool integer_in(const JsonValue& value, double min, double max, double& out) {
    if (!value.is_number()) {
        return false;
    }
    out = value.as_number();
    return std::isfinite(out) && out == std::floor(out) && out >= min && out <= max;
}

bool number_at_least(const JsonValue& value, double min, double& out) {
    if (!value.is_number()) {
        return false;
    }
    out = value.as_number();
    return std::isfinite(out) && out >= min;
}

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

bool read_capture(std::string_view text, History& out, std::string& error) {
    JsonValue root;
    if (!engine_core::parse_json(text, root, error)) {
        error = "This is not a profile capture: " + error;
        return false;
    }
    if (!root.is_object()) {
        error = "This is not a profile capture.";
        return false;
    }
    const JsonValue* format = root.find("format");
    if (format == nullptr || !format->is_string() || format->as_string() != kCaptureFormat) {
        error = "This is not a profile capture.";
        return false;
    }
    const JsonValue* version = root.find("version");
    double version_number = 0;
    if (version == nullptr || !integer_in(*version, 1, 1e9, version_number)) {
        error = "The capture has no version.";
        return false;
    }
    if (version_number != kCaptureVersion) {
        error = "The capture was made by a newer studio (version " + std::to_string(static_cast<int>(version_number)) +
                ").";
        return false;
    }
    History history;
    const JsonValue* threads = root.find("threads");
    if (threads == nullptr || !threads->is_array() || threads->items().empty()) {
        error = "The capture lists no threads.";
        return false;
    }
    for (const JsonValue& thread : threads->items()) {
        if (!thread.is_string()) {
            error = "A thread's name is not text.";
            return false;
        }
        history.rows.push_back(thread.as_string());
    }
    const JsonValue* scopes = root.find("scopes");
    if (scopes == nullptr || !scopes->is_array()) {
        error = "The capture lists no scopes.";
        return false;
    }
    for (const JsonValue& scope : scopes->items()) {
        const JsonValue* name = scope.find("name");
        const JsonValue* group = scope.find("group");
        ScopeInfo info;
        if (name == nullptr || !name->is_string() || group == nullptr || !group->is_string() ||
            !group_from(group->as_string(), info.group)) {
            error = "A scope has no name or no known group.";
            return false;
        }
        info.name = name->as_string();
        if (const JsonValue* key = scope.find("script"); key != nullptr && key->is_string()) {
            info.key = key->as_string();
        }
        history.scopes.push_back(std::move(info));
    }
    if (const JsonValue* causes = root.find("causes"); causes != nullptr) {
        if (!causes->is_array()) {
            error = "The capture's causes are not a list.";
            return false;
        }
        for (const JsonValue& cause : causes->items()) {
            if (!cause.is_string()) {
                error = "A cause is not text.";
                return false;
            }
            history.causes.push_back(cause.as_string());
        }
    }
    double number = 0;
    if (const JsonValue* lag = root.find("gpu_lag_frames"); lag != nullptr && integer_in(*lag, 0, 1000, number)) {
        history.gpu_lag_frames = static_cast<int>(number);
    }
    if (const JsonValue* dropped = root.find("dropped"); dropped != nullptr && integer_in(*dropped, 0, 1e18, number)) {
        history.dropped = static_cast<std::uint64_t>(number);
    }
    const JsonValue* frames = root.find("frames");
    if (frames == nullptr || !frames->is_array()) {
        error = "The capture has no frames.";
        return false;
    }
    if (frames->items().size() > kHistoryFrames * 10) {
        error = "The capture has more frames than a profile keeps.";
        return false;
    }
    double previous_end = 0;
    for (const JsonValue& pair : frames->items()) {
        double start = 0;
        double end = 0;
        if (!pair.is_array() || pair.items().size() != 2 || !number_at_least(pair.items()[0], 0, start) ||
            !number_at_least(pair.items()[1], 0, end) || end <= start || start < previous_end) {
            error = "The capture's frames are out of order or empty.";
            return false;
        }
        previous_end = end;
        Frame frame;
        frame.start_ns = static_cast<std::uint64_t>(start * 1000.0);
        frame.end_ns = static_cast<std::uint64_t>(end * 1000.0);
        history.frames.push_back(std::move(frame));
    }
    const JsonValue* events = root.find("events");
    if (events == nullptr || !events->is_array()) {
        error = "The capture has no events.";
        return false;
    }
    for (const JsonValue& event : events->items()) {
        double row = 0;
        double scope = 0;
        double depth = 0;
        double start = 0;
        double duration = 0;
        if (!event.is_array() || event.items().size() != 6) {
            error = "An event is not six values.";
            return false;
        }
        const auto& v = event.items();
        if (!integer_in(v[0], 0, static_cast<double>(history.rows.size()) - 1, row)) {
            error = "An event names a thread the capture does not list.";
            return false;
        }
        if (history.scopes.empty() || !integer_in(v[1], 0, static_cast<double>(history.scopes.size()) - 1, scope)) {
            error = "An event names a scope the capture does not list.";
            return false;
        }
        if (!integer_in(v[2], 0, 255, depth) || !number_at_least(v[3], 0, start)) {
            error = "An event has no depth or start.";
            return false;
        }
        if (!number_at_least(v[4], 0, duration)) {
            error = "An event has a negative length.";
            return false;
        }
        ScopeRecord record;
        record.row = static_cast<std::uint16_t>(row);
        record.scope = static_cast<ScopeId>(scope);
        record.depth = static_cast<std::uint8_t>(depth);
        record.start_ns = static_cast<std::uint64_t>(start * 1000.0);
        record.end_ns = record.start_ns + static_cast<std::uint64_t>(duration * 1000.0);
        if (!v[5].is_null()) {
            double cause = 0;
            if (history.causes.empty() ||
                !integer_in(v[5], 0, static_cast<double>(history.causes.size()) - 1, cause)) {
                error = "An event names a cause the capture does not list.";
                return false;
            }
            record.cause = static_cast<CauseId>(cause);
        }
        auto after = std::upper_bound(history.frames.begin(), history.frames.end(), record.start_ns,
                                      [](std::uint64_t at, const Frame& frame) { return at < frame.start_ns; });
        if (after == history.frames.begin() || record.start_ns >= (after - 1)->end_ns) {
            error = "An event starts outside every frame.";
            return false;
        }
        (after - 1)->scopes.push_back(record);
    }
    out = std::move(history);
    return true;
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
    if (!history.capture_name.empty()) {
        out.set("capture", JsonValue::string(history.capture_name));
    }
    std::vector<double> lengths;
    std::size_t slowest = 0;
    int over = 0;
    double total = 0;
    for (std::size_t index = 0; index < history.frames.size(); ++index) {
        const double ms = frame_ms(history.frames[index]);
        lengths.push_back(ms);
        total += ms;
        over += ms > kBudgetMs ? 1 : 0;
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
    std::strftime(text, sizeof text, "profile-%Y%m%d-%H%M%S.aprof.json", &local);
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
