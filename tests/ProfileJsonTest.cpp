// Capture files and the get_profile report, from a history built by hand.

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

void testRoundTrip() {
    const profiler::History before = sample();
    const std::string text = profiler::write_capture(before, "MyGame", "2026-10-04T00:00:00Z");
    profiler::History after;
    std::string error;
    expect(profiler::read_capture(text, after, error), "a written capture reads back");
    expect(error.empty(), "with no error");
    expect(after.rows == before.rows, "rows survive");
    expect(after.causes == before.causes, "causes survive");
    expect(after.gpu_lag_frames == 2 && after.dropped == 3, "GPU lag and drops survive");
    bool scopes_same = after.scopes.size() == before.scopes.size();
    for (std::size_t index = 0; scopes_same && index < before.scopes.size(); ++index) {
        scopes_same = after.scopes[index].name == before.scopes[index].name &&
                      after.scopes[index].group == before.scopes[index].group &&
                      after.scopes[index].key == before.scopes[index].key;
    }
    expect(scopes_same, "scope names, groups, and Script keys survive");
    bool frames_same = after.frames.size() == before.frames.size();
    for (std::size_t index = 0; frames_same && index < before.frames.size(); ++index) {
        const profiler::Frame& a = before.frames[index];
        const profiler::Frame& b = after.frames[index];
        frames_same = b.scopes.size() == a.scopes.size() &&
                      std::llabs(static_cast<long long>(b.end_ns - b.start_ns) -
                                 static_cast<long long>(a.end_ns - a.start_ns)) <= 1000;
        for (std::size_t s = 0; frames_same && s < a.scopes.size(); ++s) {
            const profiler::ScopeRecord& x = a.scopes[s];
            const profiler::ScopeRecord& y = b.scopes[s];
            frames_same = x.row == y.row && x.scope == y.scope && x.depth == y.depth && x.cause == y.cause &&
                          std::llabs(static_cast<long long>(y.start_ns - b.start_ns) -
                                     static_cast<long long>(x.start_ns - a.start_ns)) <= 1000 &&
                          std::llabs(static_cast<long long>(y.end_ns - y.start_ns) -
                                     static_cast<long long>(x.end_ns - x.start_ns)) <= 1000;
        }
    }
    expect(frames_same, "frames and their scopes survive to the microsecond");
}

void expectRefused(const std::string& text, const char* label) {
    profiler::History out;
    out.capture_name = "untouched";
    std::string error;
    const bool read = profiler::read_capture(text, out, error);
    expect(!read && !error.empty(), label);
    expect(out.capture_name == "untouched" && out.frames.empty(), "a refused file changes nothing");
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at != std::string::npos) {
        text.replace(at, from.size(), to);
    }
    return text;
}

void testRefusals() {
    const std::string good = profiler::write_capture(sample(), "MyGame", "2026-10-04T00:00:00Z");
    expectRefused(replaced(good, "\"anarchy-profile\"", "\"other-profile\""), "a file of another format is refused");
    expectRefused(replaced(good, "\"version\":1", "\"version\":2"), "a newer version is refused");
    expectRefused(good.substr(0, good.size() / 2), "a truncated file is refused");
    expectRefused("{\"format\":\"anarchy-profile\",\"version\":1,\"threads\":[\"Sim\"],\"scopes\":[{\"name\":\"A\","
                  "\"group\":\"engine\"}],\"causes\":[],\"frames\":[[0,16000]],\"events\":[[4,0,0,10,5,null]]}",
                  "an event on a thread out of range is refused");
    expectRefused("{\"format\":\"anarchy-profile\",\"version\":1,\"threads\":[\"Sim\"],\"scopes\":[{\"name\":\"A\","
                  "\"group\":\"engine\"}],\"causes\":[],\"frames\":[[0,16000]],\"events\":[[0,7,0,10,5,null]]}",
                  "an event naming a scope out of range is refused");
    expectRefused("{\"format\":\"anarchy-profile\",\"version\":1,\"threads\":[\"Sim\"],\"scopes\":[{\"name\":\"A\","
                  "\"group\":\"engine\"}],\"causes\":[],\"frames\":[[0,16000]],\"events\":[[0,0,0,10,-5,null]]}",
                  "a negative duration is refused");
    expectRefused("{\"format\":\"anarchy-profile\",\"version\":1,\"threads\":[\"Sim\"],\"scopes\":[{\"name\":\"A\","
                  "\"group\":\"engine\"}],\"causes\":[],\"frames\":[[0,16000],[8000,9000]],\"events\":[]}",
                  "frames out of order are refused");
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
    expect(profiler::capture_file_name(std::mktime(&when)) == "profile-20261004-021503.aprof.json",
           "captures are named by local time");
}

}  // namespace

int RunProfileJsonTests() {
    testRoundTrip();
    testRefusals();
    testReport();
    testFileName();
    return gFailures;
}
