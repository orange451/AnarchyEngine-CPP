// Capture JSON, the page Save writes, and the get_profile report, from a history built by hand.

#include "PropertyBag.hpp"
#include "profiler/ProfileJson.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int gFailures = 0;

void expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++gFailures;
    }
}

constexpr std::uint64_t kBase = 5000000000ull;
constexpr std::uint64_t kMs = 1000000ull;

profiler::ScopeRecord record(std::uint16_t row, profiler::ScopeId scope, std::uint8_t depth, double start_ms,
                             double end_ms, profiler::CauseId cause = profiler::kNoCause) {
    profiler::ScopeRecord out;
    out.row = row;
    out.scope = scope;
    out.depth = depth;
    out.cause = cause;
    out.start_ns = kBase + static_cast<std::uint64_t>(start_ms * kMs);
    out.end_ns = kBase + static_cast<std::uint64_t>(end_ms * kMs);
    return out;
}

// Three frames: 16 ms, 20 ms (over budget, with EnemyAI and its pathfind), 16 ms.
profiler::History sample() {
    profiler::History history;
    history.rows = {"Sim", "Render", "UI", "GPU"};
    history.scopes = {{"Physics", profiler::Group::Physics, ""},
                      {"EnemyAI", profiler::Group::Script, "guid-9"},
                      {"pathfind", profiler::Group::User, ""},
                      {"Prepare", profiler::Group::Render, ""},
                      {"Shadows", profiler::Group::Gpu, ""},
                      {"Tiny", profiler::Group::Engine, ""}};
    history.causes = {"Heartbeat"};
    history.gpu_lag_frames = 2;
    history.dropped = 3;
    const double starts[] = {0, 16, 36};
    const double ends[] = {16, 36, 52};
    for (int index = 0; index < 3; ++index) {
        profiler::Frame frame;
        frame.start_ns = kBase + static_cast<std::uint64_t>(starts[index] * kMs);
        frame.end_ns = kBase + static_cast<std::uint64_t>(ends[index] * kMs);
        const double s = starts[index];
        frame.scopes.push_back(record(0, 0, 0, s + 1, s + 3));
        frame.scopes.push_back(record(1, 3, 0, s + 0.5, s + 1.5));
        if (index == 1) {
            frame.scopes.push_back(record(0, 1, 0, s + 4, s + 14, 0));
            frame.scopes.push_back(record(0, 2, 1, s + 5, s + 13));
            frame.scopes.push_back(record(0, 5, 1, s + 13.5, s + 13.51));
            frame.scopes.push_back(record(3, 4, 0, s + 2, s + 6));
        }
        history.frames.push_back(std::move(frame));
    }
    return history;
}

const engine_core::JsonValue* member(const engine_core::JsonValue& value, const char* key) { return value.find(key); }

// The JSON between the page's data block tags, as a browser's JSON.parse reads it.
std::string embedded_json(const std::string& page) {
    const std::string open = "<script id=\"capture\" type=\"application/json\">";
    const std::size_t from = page.find(open);
    if (from == std::string::npos) {
        return {};
    }
    const std::size_t start = from + open.size();
    const std::size_t end = page.find("</script>", start);
    return end == std::string::npos ? std::string() : page.substr(start, end - start);
}

void testCapture() {
    const profiler::History history = sample();
    const std::string text = profiler::write_capture(history, "MyGame", "2026-10-04T00:00:00Z");
    engine_core::JsonValue root;
    std::string error;
    expect(engine_core::parse_json(text, root, error), "a capture is JSON");
    expect(member(root, "format") != nullptr && member(root, "format")->as_string() == "anarchy-profile" &&
               member(root, "version")->as_number() == 1,
           "with its format and version");
    expect(member(root, "place")->as_string() == "MyGame" &&
               member(root, "created")->as_string() == "2026-10-04T00:00:00Z",
           "the place and when it was made");
    expect(member(root, "threads")->items().size() == 4 && member(root, "causes")->items().size() == 1,
           "every thread and cause");
    expect(member(root, "gpu_lag_frames")->as_number() == 2 && member(root, "dropped")->as_number() == 3,
           "the GPU lag and the drops");
    expect(member(root, "budget_ms")->as_number() == profiler::kBudgetMs &&
               member(root, "over_budget_ms")->as_number() == profiler::kOverBudgetMs,
           "the budget a frame is measured against");
    const auto& scopes = member(root, "scopes")->items();
    expect(scopes.size() == 6 && scopes[1].find("name")->as_string() == "EnemyAI" &&
               scopes[1].find("group")->as_string() == "script" && scopes[1].find("script")->as_string() == "guid-9",
           "scope names, groups, and Script keys");
    const auto& frames = member(root, "frames")->items();
    expect(frames.size() == 3 && frames[0].items()[0].as_number() == 0 && frames[1].items()[0].as_number() == 16000 &&
               frames[2].items()[1].as_number() == 52000,
           "frames in microseconds from the first frame's start");
    const auto& events = member(root, "events")->items();
    expect(events.size() == 10, "every scope of every frame");
    // Frame 1's EnemyAI: Sim, scope 1, depth 0, 20 ms in for 10 ms, resumed by Heartbeat.
    const auto& enemy = events[4].items();
    expect(enemy.size() == 6 && enemy[0].as_number() == 0 && enemy[1].as_number() == 1 && enemy[2].as_number() == 0 &&
               std::fabs(enemy[3].as_number() - 20000) < 0.01 && std::fabs(enemy[4].as_number() - 10000) < 0.01 &&
               enemy[5].as_number() == 0,
           "an event is its thread, scope, depth, start, length, and cause");
    expect(events[0].items()[5].is_null(), "a scope with no cause has null");
}

void testPage() {
    const profiler::History history = sample();
    const std::string page = profiler::write_capture_html(history, "MyGame", "2026-10-04T00:00:00Z");
    expect(page.rfind("<!doctype html>", 0) == 0, "the page is HTML");
    expect(page.find("<canvas id=\"timeline\">") != std::string::npos && page.find("id=\"tab-scopes\"") != std::string::npos,
           "with the Timeline and the Scopes");
    expect(page.find("src=") == std::string::npos && page.find("href=") == std::string::npos &&
               page.find("@import") == std::string::npos,
           "and nothing loaded from elsewhere, so it reads offline");
    expect(embedded_json(page) == profiler::write_capture(history, "MyGame", "2026-10-04T00:00:00Z"),
           "the page holds the capture's JSON");

    // A place named to end the data block early, or to make a browser read past its end, stays inside it.
    const std::string name = "<!--<script></script><script>alert(1)</script>";
    const std::string hostile = profiler::write_capture_html(history, name, "");
    const std::string json = embedded_json(hostile);
    engine_core::JsonValue root;
    std::string error;
    expect(engine_core::parse_json(json, root, error) && member(root, "place")->as_string() == name,
           "a place name with </script> stays in the data block");
    expect(json.find('<') == std::string::npos && hostile.find("<script>alert") == std::string::npos &&
               hostile.find("<!--") == std::string::npos,
           "and never becomes a script or a comment");
}

void testReport() {
    const profiler::History history = sample();
    profiler::ReportOptions options;
    options.top = 3;
    const engine_core::JsonValue report = profiler::build_report(history, options);
    const engine_core::JsonValue* frames = member(report, "frames");
    expect(frames != nullptr && frames->as_number() == 3, "the report counts the frames");
    const engine_core::JsonValue* times = member(report, "frame_ms");
    expect(times != nullptr && std::abs(member(*times, "max")->as_number() - 20.0) < 1e-6 &&
               std::abs(member(*times, "avg")->as_number() - 52.0 / 3.0) < 1e-3 &&
               std::abs(member(*times, "p95")->as_number() - 20.0) < 1e-6,
           "avg, p95, and max frame times");
    expect(member(report, "over_budget") != nullptr && member(report, "over_budget")->as_number() == 1,
           "one frame over 16.6 ms");
    expect(member(report, "gpu_lag_frames") != nullptr && member(report, "gpu_lag_frames")->as_number() == 2,
           "the GPU lag is reported");
    const engine_core::JsonValue* top = member(report, "scopes");
    expect(top != nullptr && top->items().size() == 3, "top limits the scope rows");
    if (top != nullptr && !top->items().empty()) {
        const engine_core::JsonValue& first = top->items()[0];
        expect(member(first, "name")->as_string() == "EnemyAI" && member(first, "cause")->as_string() == "Heartbeat" &&
                   member(first, "thread")->as_string() == "Sim" && member(first, "max_ms")->as_number() == 10,
               "the slowest scope first, with its cause and thread");
    }
    const engine_core::JsonValue* slowest = member(report, "slowest_frame");
    expect(slowest != nullptr && member(*slowest, "ms")->as_number() == 20 && member(*slowest, "index")->as_number() == 1,
           "the slowest frame is named");
    bool nested = false;
    bool pruned = true;
    if (slowest != nullptr) {
        for (const engine_core::JsonValue& thread : member(*slowest, "threads")->items()) {
            if (member(thread, "thread")->as_string() != "Sim") {
                continue;
            }
            for (const engine_core::JsonValue& scope : member(thread, "scopes")->items()) {
                if (member(scope, "name")->as_string() != "EnemyAI") {
                    continue;
                }
                for (const engine_core::JsonValue& child : member(scope, "children")->items()) {
                    nested = nested || member(child, "name")->as_string() == "pathfind";
                    pruned = pruned && member(child, "name")->as_string() != "Tiny";
                }
            }
        }
    }
    expect(nested, "the slowest frame's tree nests pathfind under EnemyAI");
    expect(pruned, "scopes under 0.05 ms are left out of the tree");
    options.include_timeline = false;
    expect(profiler::build_report(history, options).find("slowest_frame") == nullptr,
           "include_timeline false leaves the tree out");
    expect(member(profiler::build_report(profiler::History(), options), "frames")->as_number() == 0,
           "an empty history reports zero frames");
}

void testFileName() {
    std::tm when{};
    when.tm_year = 126;
    when.tm_mon = 9;
    when.tm_mday = 4;
    when.tm_hour = 2;
    when.tm_min = 15;
    when.tm_sec = 3;
    when.tm_isdst = -1;
    expect(profiler::capture_file_name(std::mktime(&when)) == "profile-20261004-021503.html",
           "profiles are named by local time");
}

}  // namespace

int RunProfileJsonTests() {
    testCapture();
    testPage();
    testReport();
    testFileName();
    return gFailures;
}
