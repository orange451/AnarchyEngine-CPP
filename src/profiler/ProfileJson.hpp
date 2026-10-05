#pragma once

#include "PropertyBag.hpp"
#include "profiler/Profiler.hpp"

#include <ctime>
#include <string>
#include <string_view>

// A history as JSON, and as the page the profiler's Save writes: that JSON
// with a viewer, readable in any browser. Also the report get_profile returns.
namespace profiler {

inline constexpr const char* kCaptureFormat = "anarchy-profile";
inline constexpr int kCaptureVersion = 1;

// One line of compact JSON. Times are microseconds from the first frame's start, to the nanosecond.
std::string write_capture(const History& history, const std::string& place, const std::string& created_utc);
// A whole HTML page: write_capture's JSON in a data block, and a viewer that
// shows it as the overlay does, with the frame graph, Timeline, and Scopes.
std::string write_capture_html(const History& history, const std::string& place, const std::string& created_utc);

struct ReportOptions {
    int top = 25;
    bool include_timeline = true;
};
engine_core::JsonValue build_report(const History& history, const ReportOptions& options);

// "profile-<yyyyMMdd-HHmmss>.html", in local time.
std::string capture_file_name(std::time_t when);
// "yyyy-MM-ddTHH:mm:ssZ".
std::string utc_stamp(std::time_t when);

}  // namespace profiler
