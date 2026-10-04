#pragma once

#include "Profiler.hpp"

#include <cstddef>
#include <vector>

namespace profiler {

// One row of the Scopes table: a scope on a thread, and what resumed it when it
// is a Script's. Times are a frame's total for the scope, in milliseconds.
struct ScopeStat {
    std::uint16_t row = 0;
    ScopeId scope = 0;
    CauseId cause = kNoCause;
    // The most any one frame spent in it, and the average over every frame.
    double max_ms = 0;
    double avg_ms = 0;
    // The selected frame's total.
    double frame_ms = 0;
    double calls_per_frame = 0;
    // avg_ms over the average frame's length.
    double share = 0;
};

enum class StatSort { Max, Avg, Frame, Calls, Share, Name, Row };

// Over every frame of history. selected_frame past the end reads as none.
std::vector<ScopeStat> compute_stats(const History& history, std::size_t selected_frame);
// Numbers sort largest first; Name and Row sort in order. Ties keep a stable order by name.
void sort_stats(std::vector<ScopeStat>& stats, const History& history, StatSort sort);
double frame_ms(const Frame& frame);
// The name a row of the table shows: the scope's, then " · " and the cause when it has one.
std::string stat_label(const ScopeStat& stat, const History& history);

}  // namespace profiler
