#pragma once

#include "PropertyBag.hpp"
#include "profiler/Profiler.hpp"

#include <ctime>
#include <string>
#include <string_view>

// A history as JSON: the capture files the overlay saves and the studio opens
// (*.aprof.json), and the report get_profile returns.
namespace profiler {

inline constexpr const char* kCaptureFormat = "anarchy-profile";
inline constexpr int kCaptureVersion = 1;

// One line of compact JSON. Times are microseconds from the first frame's start, to the nanosecond.
std::string write_capture(const History& history, const std::string& place, const std::string& created_utc);
// False, with why, for any file that is not a whole version-1 capture; out is then untouched.
bool read_capture(std::string_view text, History& out, std::string& error);

struct ReportOptions {
    int top = 25;
    bool include_timeline = true;
};
engine_core::JsonValue build_report(const History& history, const ReportOptions& options);

// "profile-<yyyyMMdd-HHmmss>.aprof.json", in local time.
std::string capture_file_name(std::time_t when);
// "yyyy-MM-ddTHH:mm:ssZ".
std::string utc_stamp(std::time_t when);

}  // namespace profiler
