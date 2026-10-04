#include "ProfileStats.hpp"

#include <algorithm>
#include <unordered_map>

namespace profiler {

namespace {

std::uint64_t key_of(std::uint16_t row, ScopeId scope, CauseId cause) {
    return (static_cast<std::uint64_t>(row) << 48) | (static_cast<std::uint64_t>(cause) << 32) | scope;
}

struct Acc {
    std::size_t index = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t max_ns = 0;
    std::uint64_t calls = 0;
};

}  // namespace

double frame_ms(const Frame& frame) {
    return frame.end_ns > frame.start_ns ? static_cast<double>(frame.end_ns - frame.start_ns) / 1e6 : 0.0;
}

std::vector<ScopeStat> compute_stats(const History& history, std::size_t selected_frame) {
    std::vector<ScopeStat> out;
    if (history.frames.empty()) {
        return out;
    }
    std::unordered_map<std::uint64_t, Acc> accs;
    std::unordered_map<std::uint64_t, std::uint64_t> this_frame;
    double frames_ms = 0;
    for (std::size_t index = 0; index < history.frames.size(); ++index) {
        const Frame& frame = history.frames[index];
        frames_ms += frame_ms(frame);
        this_frame.clear();
        for (const ScopeRecord& record : frame.scopes) {
            const std::uint64_t key = key_of(record.row, record.scope, record.cause);
            auto [it, added] = accs.try_emplace(key);
            if (added) {
                it->second.index = out.size();
                ScopeStat stat;
                stat.row = record.row;
                stat.scope = record.scope;
                stat.cause = record.cause;
                out.push_back(stat);
            }
            const std::uint64_t ns = record.end_ns - record.start_ns;
            it->second.total_ns += ns;
            it->second.calls += 1;
            this_frame[key] += ns;
        }
        for (const auto& [key, ns] : this_frame) {
            Acc& acc = accs[key];
            acc.max_ns = std::max(acc.max_ns, ns);
            if (index == selected_frame) {
                out[acc.index].frame_ms = static_cast<double>(ns) / 1e6;
            }
        }
    }
    const double count = static_cast<double>(history.frames.size());
    const double avg_frame_ms = frames_ms / count;
    for (const auto& [key, acc] : accs) {
        ScopeStat& stat = out[acc.index];
        stat.max_ms = static_cast<double>(acc.max_ns) / 1e6;
        stat.avg_ms = static_cast<double>(acc.total_ns) / 1e6 / count;
        stat.calls_per_frame = static_cast<double>(acc.calls) / count;
        stat.share = avg_frame_ms > 0 ? stat.avg_ms / avg_frame_ms : 0.0;
    }
    return out;
}

std::string stat_label(const ScopeStat& stat, const History& history) {
    std::string label = stat.scope < history.scopes.size() ? history.scopes[stat.scope].name : std::string("?");
    if (stat.cause != kNoCause && stat.cause < history.causes.size()) {
        label += " \xC2\xB7 ";
        label += history.causes[stat.cause];
    }
    return label;
}

void sort_stats(std::vector<ScopeStat>& stats, const History& history, StatSort sort) {
    auto value = [sort](const ScopeStat& stat) {
        switch (sort) {
            case StatSort::Avg:
                return stat.avg_ms;
            case StatSort::Frame:
                return stat.frame_ms;
            case StatSort::Calls:
                return stat.calls_per_frame;
            case StatSort::Share:
                return stat.share;
            default:
                return stat.max_ms;
        }
    };
    std::stable_sort(stats.begin(), stats.end(), [&](const ScopeStat& a, const ScopeStat& b) {
        if (sort == StatSort::Name) {
            return stat_label(a, history) < stat_label(b, history);
        }
        if (sort == StatSort::Row) {
            if (a.row != b.row) {
                return a.row < b.row;
            }
            return a.max_ms > b.max_ms;
        }
        const double va = value(a);
        const double vb = value(b);
        if (va != vb) {
            return va > vb;
        }
        return stat_label(a, history) < stat_label(b, history);
    });
}

}  // namespace profiler
