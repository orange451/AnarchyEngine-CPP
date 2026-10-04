#include "ProfilerOverlay.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace runner {

namespace {

using jadefx::Color;

// One dark palette for the studio and games alike: it sits over the scene.
const Color kBackground = Color::rgb8(12, 15, 20, 242);
const Color kPanel = Color::rgb8(28, 35, 44, 255);
const Color kLine = Color::rgb8(42, 51, 63, 255);
const Color kText = Color::rgb8(217, 224, 232);
const Color kMuted = Color::rgb8(138, 150, 166);
const Color kBudget = Color::rgb8(242, 177, 52);
const Color kOver = Color::rgb8(229, 96, 79);
const Color kBar = Color::rgb8(60, 109, 158);
const Color kBarHover = Color::rgb8(96, 150, 204);
const Color kInk = Color::rgb8(13, 16, 21);
const Color kButton = Color::rgb8(36, 44, 55, 255);
const Color kButtonOn = Color::rgb8(58, 70, 86, 255);

Color group_color(profiler::Group group) {
    switch (group) {
        case profiler::Group::Physics:
            return Color::rgb8(59, 167, 160);
        case profiler::Group::Render:
            return Color::rgb8(217, 138, 61);
        case profiler::Group::Script:
            return Color::rgb8(155, 123, 224);
        case profiler::Group::User:
            return Color::rgb8(226, 195, 90);
        case profiler::Group::Gpu:
            return Color::rgb8(95, 179, 138);
        default:
            return Color::rgb8(79, 143, 214);
    }
}

Color dimmed(Color color) {
    color.a *= 0.3f;
    return color;
}

constexpr const char* kFamily = "Open Sans";
constexpr double kMargin = 8;
constexpr double kHeaderHeight = 24;
constexpr double kGraphTop = kHeaderHeight + 8;
constexpr double kGraphHeight = 58;
constexpr double kGraphGutter = 44;
constexpr double kLowerTop = kGraphTop + kGraphHeight + 16;
constexpr double kRulerHeight = 16;
constexpr double kRowGutter = 70;
constexpr double kLane = 15;
constexpr int kMaxLanes = 6;
constexpr double kTableRow = 16;
constexpr double kColumnWidths[] = {0, 64, 62, 62, 74, 64, 120};
constexpr const char* kColumnNames[] = {"Scope", "Thread", "Max ms", "Avg ms", "This frame", "Calls", "% of frame"};
constexpr profiler::StatSort kColumnSorts[] = {profiler::StatSort::Name,  profiler::StatSort::Row,
                                               profiler::StatSort::Max,   profiler::StatSort::Avg,
                                               profiler::StatSort::Frame, profiler::StatSort::Calls,
                                               profiler::StatSort::Share};

#ifdef __APPLE__
constexpr const char* kHints = "Cmd+F6 hide  \xC2\xB7  Cmd+P pause";
#else
constexpr const char* kHints = "Ctrl+F6 hide  \xC2\xB7  Ctrl+P pause";
#endif

double seconds_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string format(const char* pattern, double value) {
    char text[64];
    std::snprintf(text, sizeof text, pattern, value);
    return text;
}

// A tick step of 1, 2, or 5 times a power of ten, about every target ms.
double nice_step(double target) {
    const double power = std::pow(10.0, std::floor(std::log10(std::max(target, 1e-6))));
    for (double scale : {1.0, 2.0, 5.0, 10.0}) {
        if (power * scale >= target) {
            return power * scale;
        }
    }
    return power * 10.0;
}

}  // namespace

ProfilerUi& ProfilerUi::get() {
    static ProfilerUi* instance = new ProfilerUi();
    return *instance;
}

void ProfilerUi::setShown(bool shown) {
    if (shown == shown_) {
        return;
    }
    shown_ = shown;
    if (shown) {
        profiler::acquire();
        return;
    }
    // Shown again, it starts live: hiding ends a pause and closes a capture.
    if (profiler::showing_capture()) {
        profiler::close_capture();
    } else if (profiler::paused()) {
        profiler::set_paused(false);
    }
    selected = kNewest;
    profiler::release();
}

void ProfilerUi::togglePaused() {
    if (!shown_) {
        return;
    }
    if (profiler::showing_capture()) {
        profiler::close_capture();
        selected = kNewest;
        return;
    }
    const bool now = !profiler::paused();
    profiler::set_paused(now);
    if (!now) {
        selected = kNewest;
    }
}

ProfilerOverlay::ProfilerOverlay() {
    getClassList().add("profiler-overlay");
    // Right and middle drags pan the timeline.
    setReceivesAllButtons(true);
}

double ProfilerOverlay::heightFor(double viewHeight, double split) {
    const double wanted = kLowerTop + kMargin + std::clamp(split, 0.2, 0.8) * viewHeight;
    return std::max(0.0, std::min(wanted, viewHeight - 2 * kMargin));
}

ProfilerOverlay::Rect ProfilerOverlay::tabRect(ProfilerUi::Tab tab) const {
    const double x = getAbsoluteX() + 74;
    const double y = getAbsoluteY() + 3;
    return tab == ProfilerUi::Tab::Timeline ? Rect{x, y, 68, 18} : Rect{x + 72, y, 60, 18};
}

ProfilerOverlay::Rect ProfilerOverlay::pauseRect() const {
    return {getAbsoluteX() + getWidth() - kMargin - 72, getAbsoluteY() + 3, 72, 18};
}

ProfilerOverlay::Rect ProfilerOverlay::saveRect() const {
    const Rect pause = pauseRect();
    return {pause.x - 6 - 48, pause.y, 48, 18};
}

ProfilerOverlay::Rect ProfilerOverlay::closeCaptureRect() const {
    const Rect save = saveRect();
    return {save.x - 6 - 96, save.y, 96, 18};
}

ProfilerOverlay::Rect ProfilerOverlay::graphRect() const {
    return {getAbsoluteX() + kGraphGutter, getAbsoluteY() + kGraphTop, std::max(0.0, getWidth() - kGraphGutter - kMargin),
            kGraphHeight};
}

ProfilerOverlay::Rect ProfilerOverlay::barRect(std::size_t index, std::size_t count) const {
    const Rect graph = graphRect();
    const double width = graph.w / static_cast<double>(profiler::kHistoryFrames);
    const double x = graph.x + graph.w - static_cast<double>(count - index) * width;
    return {x, graph.y, width, graph.h};
}

ProfilerOverlay::Rect ProfilerOverlay::lowerRect() const {
    return {getAbsoluteX() + kMargin, getAbsoluteY() + kLowerTop, std::max(0.0, getWidth() - 2 * kMargin),
            std::max(0.0, getHeight() - kLowerTop - kMargin)};
}

ProfilerOverlay::Rect ProfilerOverlay::splitterRect() const {
    const Rect lower = lowerRect();
    return {lower.x, lower.y - 7, lower.w, 6};
}

ProfilerOverlay::Rect ProfilerOverlay::columnRect(int column) const {
    const Rect lower = lowerRect();
    double right = lower.x + lower.w;
    for (int index = kColumns - 1; index > column; --index) {
        right -= kColumnWidths[index];
    }
    if (column == kScopeColumn) {
        return {lower.x, lower.y, std::max(0.0, right - lower.x), kTableRow + 2};
    }
    return {right - kColumnWidths[column], lower.y, kColumnWidths[column], kTableRow + 2};
}

double ProfilerOverlay::textWidth(const std::string& text, float size) const {
    return jadefx::Font(kFamily, size).measureWidth(text);
}

void ProfilerOverlay::label(jadefx::Painter& painter, double x, double y, const std::string& text, const Color& color,
                            float size) {
    painter.text(static_cast<float>(x), static_cast<float>(y), text, kFamily, size, color);
}

void ProfilerOverlay::button(jadefx::Painter& painter, const Rect& rect, const std::string& text, bool on) {
    const bool hover = hovering_ && rect.contains(mouse_x_, mouse_y_);
    painter.fillRect(static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.w),
                     static_cast<float>(rect.h), on ? kButtonOn : (hover ? kPanel : kButton));
    const double width = textWidth(text);
    label(painter, rect.x + (rect.w - width) * 0.5, rect.y + 2, text, on ? kText : kMuted);
}

std::size_t ProfilerOverlay::selectedIn(const profiler::History& history) const {
    const std::size_t selected = ProfilerUi::get().selected;
    if (history.frames.empty()) {
        return 0;
    }
    return selected < history.frames.size() ? selected : history.frames.size() - 1;
}

void ProfilerOverlay::timelineWindow(const profiler::History& history, double& start_ns, double& span_ns) const {
    span_ns = span_ns_;
    if (follow_ && !history.frames.empty()) {
        start_ns = static_cast<double>(history.frames.back().end_ns) - span_ns_;
    } else {
        start_ns = start_ns_;
    }
}

void ProfilerOverlay::pauseHere() {
    if (profiler::paused()) {
        follow_ = false;
        return;
    }
    double start = 0;
    double span = 0;
    profiler::with_view([&](const profiler::History& history) { timelineWindow(history, start, span); });
    profiler::set_paused(true);
    start_ns_ = start;
    follow_ = false;
}

void ProfilerOverlay::selectFrame(std::size_t index) {
    if (!profiler::paused()) {
        profiler::set_paused(true);
    }
    ProfilerUi::get().selected = index;
    profiler::with_view([&](const profiler::History& history) {
        if (index < history.frames.size()) {
            const profiler::Frame& frame = history.frames[index];
            const double middle = 0.5 * static_cast<double>(frame.start_ns + frame.end_ns);
            span_ns_ = std::max(span_ns_, static_cast<double>(frame.end_ns - frame.start_ns) * 1.25);
            start_ns_ = middle - span_ns_ * 0.5;
        }
    });
    follow_ = false;
}

void ProfilerOverlay::refreshStats(const profiler::History& history) {
    const std::size_t selected = selectedIn(history);
    const bool paused = !follow_;
    const double now = seconds_now();
    const bool stale = stats_frames_ != history.frames.size() || stats_selected_ != selected ||
                       stats_sort_ != sort_ || stats_paused_ != paused;
    if (!stale || (!paused && stats_sort_ == sort_ && now - stats_at_ < 0.25)) {
        return;
    }
    stats_ = profiler::compute_stats(history, selected);
    profiler::sort_stats(stats_, history, sort_);
    stats_frames_ = history.frames.size();
    stats_selected_ = selected;
    stats_sort_ = sort_;
    stats_paused_ = paused;
    stats_at_ = now;
}

void ProfilerOverlay::renderContent(jadefx::UiRenderer& renderer, float) {
    static const profiler::ScopeId kDraw = profiler::intern("Profiler overlay", profiler::Group::Engine);
    profiler::Scope timing(kDraw);
    jadefx::Painter painter(renderer);
    const float x = static_cast<float>(getAbsoluteX());
    const float y = static_cast<float>(getAbsoluteY());
    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());
    if (w <= 0 || h <= 0) {
        return;
    }
    if (!profiler::paused()) {
        follow_ = true;
        ProfilerUi::get().selected = ProfilerUi::kNewest;
    }
    const bool capture = profiler::showing_capture();
    painter.pushClip(x, y, w, h);
    painter.fillRect(x, y, w, h, kBackground);
    blocks_.clear();
    profiler::with_view([&](const profiler::History& history) {
        drawHeader(painter, history);
        if (capture) {
            button(painter, closeCaptureRect(), "Close capture", false);
        }
        drawGraph(painter, history);
        if (ProfilerUi::get().tab == ProfilerUi::Tab::Timeline) {
            drawTimeline(painter, history);
            drawTooltip(painter, history);
        } else {
            refreshStats(history);
            drawTable(painter, history);
        }
    });
    painter.popClip();
}

void ProfilerOverlay::drawHeader(jadefx::Painter& painter, const profiler::History& history) {
    const double x = getAbsoluteX();
    const double y = getAbsoluteY();
    painter.fillRect(static_cast<float>(x), static_cast<float>(y + kHeaderHeight - 1), static_cast<float>(getWidth()), 1,
                     kLine);
    label(painter, x + kMargin, y + 4, "Profiler", kText, 12.f);
    const ProfilerUi& ui = ProfilerUi::get();
    button(painter, tabRect(ProfilerUi::Tab::Timeline), "Timeline", ui.tab == ProfilerUi::Tab::Timeline);
    button(painter, tabRect(ProfilerUi::Tab::Scopes), "Scopes", ui.tab == ProfilerUi::Tab::Scopes);
    const bool paused = !follow_;
    button(painter, pauseRect(), paused ? "Resume" : "Pause", paused);
    if (paused && ui.save) {
        button(painter, saveRect(), "Save", false);
    }
    double left = tabRect(ProfilerUi::Tab::Scopes).x + tabRect(ProfilerUi::Tab::Scopes).w + 12;
    std::string info;
    if (!history.frames.empty()) {
        const std::size_t selected = selectedIn(history);
        const std::size_t back = history.frames.size() - 1 - selected;
        info = (back == 0 ? std::string("newest frame") : "frame \xE2\x88\x92" + std::to_string(back)) + " \xC2\xB7 " +
               format("%.2f ms", profiler::frame_ms(history.frames[selected]));
    } else {
        info = "waiting for frames";
    }
    if (!history.capture_name.empty()) {
        info = "capture: " + history.capture_name + "   " + info;
    } else if (paused) {
        info = "paused \xC2\xB7 " + info;
    }
    label(painter, left, y + 5, info, paused ? kBudget : kMuted);
    left += textWidth(info) + 14;
    if (history.dropped > 0) {
        const std::string dropped = std::to_string(history.dropped) + " events dropped";
        label(painter, left, y + 5, dropped, kOver);
        left += textWidth(dropped) + 14;
    }
    const double right = (history.capture_name.empty() ? (paused && ui.save ? saveRect().x : pauseRect().x)
                                                         : closeCaptureRect().x) -
                         12;
    const double hints = textWidth(kHints);
    if (right - hints > left) {
        label(painter, right - hints, y + 5, kHints, kMuted);
    }
}

void ProfilerOverlay::drawGraph(jadefx::Painter& painter, const profiler::History& history) {
    const Rect graph = graphRect();
    const std::size_t count = history.frames.size();
    double longest = 33.3;
    for (const profiler::Frame& frame : history.frames) {
        longest = std::max(longest, profiler::frame_ms(frame));
    }
    const double scale = std::ceil(longest / 5.0) * 5.0;
    painter.fillRect(static_cast<float>(graph.x), static_cast<float>(graph.y), static_cast<float>(graph.w),
                     static_cast<float>(graph.h), Color::rgb8(255, 255, 255, 8));
    const std::size_t selected = follow_ ? count : selectedIn(history);
    for (std::size_t index = 0; index < count; ++index) {
        const double ms = profiler::frame_ms(history.frames[index]);
        const Rect bar = barRect(index, count);
        const double height = std::min(1.0, ms / scale) * graph.h;
        const bool hover = hovering_ && bar.contains(mouse_x_, mouse_y_);
        Color color = ms > profiler::kOverBudgetMs ? kOver : kBar;
        if (hover) {
            color = kBarHover;
        }
        const float bar_width = static_cast<float>(std::max(1.0, bar.w - (bar.w > 2.5 ? 1.0 : 0.0)));
        painter.fillRect(static_cast<float>(bar.x), static_cast<float>(graph.y + graph.h - height), bar_width,
                         static_cast<float>(height), color);
        if (index == selected) {
            painter.fillRect(static_cast<float>(bar.x) - 1, static_cast<float>(graph.y), bar_width + 2, 2, kText);
            painter.fillRect(static_cast<float>(bar.x) - 1, static_cast<float>(graph.y + graph.h), bar_width + 2, 2,
                             kText);
        }
    }
    // The 16.6 ms budget, dashed.
    const double budget_y = graph.y + graph.h - std::min(1.0, profiler::kBudgetMs / scale) * graph.h;
    for (double dash = graph.x; dash < graph.x + graph.w; dash += 8) {
        painter.fillRect(static_cast<float>(dash), static_cast<float>(budget_y), 4, 1, kBudget);
    }
    const double gutter = getAbsoluteX() + 6;
    label(painter, gutter, budget_y - 7, format("%.1f", profiler::kBudgetMs), kBudget, 10.f);
    label(painter, gutter, graph.y - 2, format("%.0f ms", scale), kMuted, 10.f);
    label(painter, gutter, graph.y + graph.h - 10, "0", kMuted, 10.f);
    // The hovered bar's time, under the graph.
    for (std::size_t index = 0; hovering_ && index < count; ++index) {
        if (barRect(index, count).contains(mouse_x_, mouse_y_)) {
            const std::size_t back = count - 1 - index;
            const std::string text = (back == 0 ? std::string("newest") : "\xE2\x88\x92" + std::to_string(back)) +
                                     " \xC2\xB7 " + format("%.2f ms", profiler::frame_ms(history.frames[index]));
            const double width = textWidth(text, 10.f);
            const double tx = std::clamp(mouse_x_ - width * 0.5, graph.x, graph.x + graph.w - width);
            label(painter, tx, graph.y + graph.h + 2, text, kText, 10.f);
        }
    }
}

void ProfilerOverlay::drawTimeline(jadefx::Painter& painter, const profiler::History& history) {
    const Rect lower = lowerRect();
    if (lower.h < 20 || lower.w < kRowGutter + 20) {
        return;
    }
    double start = 0;
    double span = 0;
    timelineWindow(history, start, span);
    const double end = start + span;
    const Rect plot{lower.x + kRowGutter, lower.y + kRulerHeight, lower.w - kRowGutter, lower.h - kRulerHeight};
    auto to_x = [&](double ns) { return plot.x + (ns - start) / span * plot.w; };
    painter.fillRect(static_cast<float>(lower.x), static_cast<float>(lower.y + kRulerHeight - 1),
                     static_cast<float>(lower.w), 1, kLine);
    // The ruler: ms from the first frame shown.
    const auto first_frame = std::upper_bound(history.frames.begin(), history.frames.end(), start,
                                              [](double at, const profiler::Frame& frame) { return at < frame.end_ns; });
    const double origin = first_frame != history.frames.end() ? static_cast<double>(first_frame->start_ns) : start;
    const double step_ms = nice_step(span / 1e6 / 8.0);
    const double first_tick = std::ceil((start - origin) / 1e6 / step_ms) * step_ms;
    for (double tick = first_tick; origin + tick * 1e6 <= end; tick += step_ms) {
        const double tx = to_x(origin + tick * 1e6);
        if (tx < plot.x) {
            continue;
        }
        painter.fillRect(static_cast<float>(tx), static_cast<float>(lower.y + kRulerHeight - 4), 1, 3, kMuted);
        label(painter, tx + 2, lower.y, format("%g ms", tick), kMuted, 10.f);
    }
    // Frame starts, dashed down the plot, with each frame's length.
    painter.pushClip(static_cast<float>(plot.x), static_cast<float>(lower.y), static_cast<float>(plot.w),
                     static_cast<float>(lower.h));
    const std::size_t selected = selectedIn(history);
    for (auto it = first_frame; it != history.frames.end() && it->start_ns < end; ++it) {
        const double fx = to_x(static_cast<double>(it->start_ns));
        for (double dash = plot.y; dash < plot.y + plot.h; dash += 6) {
            painter.fillRect(static_cast<float>(fx), static_cast<float>(dash), 1, 3, Color::rgb8(242, 177, 52, 110));
        }
        const std::size_t index = static_cast<std::size_t>(it - history.frames.begin());
        const double width = to_x(static_cast<double>(it->end_ns)) - fx;
        if (!follow_ && index == selected) {
            painter.fillRect(static_cast<float>(fx), static_cast<float>(plot.y), static_cast<float>(width),
                             static_cast<float>(plot.h), Color::rgb8(255, 255, 255, 10));
        }
        if (width > 60) {
            label(painter, fx + 3, plot.y + 1, format("%.1f ms", profiler::frame_ms(*it)),
                  profiler::frame_ms(*it) > profiler::kOverBudgetMs ? kOver : kMuted, 10.f);
        }
    }
    painter.popClip();
    // One band per thread, as tall as its deepest scope in view.
    std::vector<int> lanes(history.rows.size(), 0);
    for (auto it = first_frame; it != history.frames.end() && it->start_ns < end; ++it) {
        for (const profiler::ScopeRecord& record : it->scopes) {
            if (record.row < lanes.size() && record.end_ns >= start && record.start_ns <= end) {
                lanes[record.row] = std::max(lanes[record.row], std::min<int>(record.depth + 1, kMaxLanes));
            }
        }
    }
    double row_y = plot.y + 14;
    std::vector<double> row_top(history.rows.size(), -1);
    for (std::size_t row = 0; row < history.rows.size(); ++row) {
        // Extra threads show only when they did something in view.
        if (row >= 4 && lanes[row] == 0) {
            continue;
        }
        const double height = std::max(1, lanes[row]) * kLane + 6;
        if (row_y + height > lower.y + lower.h) {
            break;
        }
        row_top[row] = row_y;
        std::string name = history.rows[row];
        if (name == "GPU" && history.gpu_lag_frames > 0) {
            name += " (\xE2\x88\x92" + std::to_string(history.gpu_lag_frames) + ")";
        }
        label(painter, lower.x, row_y + 1, name, kMuted, 11.f);
        painter.fillRect(static_cast<float>(lower.x), static_cast<float>(row_y + height - 2),
                         static_cast<float>(lower.w), 1, Color::rgb8(42, 51, 63, 120));
        row_y += height;
    }
    painter.pushClip(static_cast<float>(plot.x), static_cast<float>(plot.y), static_cast<float>(plot.w),
                     static_cast<float>(plot.h));
    const jadefx::Font font(kFamily, 10.f);
    for (auto it = first_frame; it != history.frames.end() && it->start_ns < end; ++it) {
        for (const profiler::ScopeRecord& record : it->scopes) {
            if (record.row >= row_top.size() || row_top[record.row] < 0 || record.depth >= kMaxLanes ||
                record.end_ns < start || record.start_ns > end) {
                continue;
            }
            const double x0 = std::max(plot.x, to_x(static_cast<double>(record.start_ns)));
            const double x1 = std::min(plot.x + plot.w, to_x(static_cast<double>(record.end_ns)));
            if (x1 - x0 < 0.25) {
                continue;
            }
            const double by = row_top[record.row] + record.depth * kLane;
            const double bw = std::max(1.0, x1 - x0);
            const profiler::Group group =
                record.scope < history.scopes.size() ? history.scopes[record.scope].group : profiler::Group::Engine;
            Color color = group_color(group);
            const bool lit = highlight_ && record.scope == highlight_scope_ && record.cause == highlight_cause_;
            if (highlight_ && !lit) {
                color = dimmed(color);
            }
            painter.fillRect(static_cast<float>(x0), static_cast<float>(by), static_cast<float>(bw), kLane - 1, color);
            if (lit) {
                painter.fillRect(static_cast<float>(x0), static_cast<float>(by), static_cast<float>(bw), 1, kText);
                painter.fillRect(static_cast<float>(x0), static_cast<float>(by + kLane - 2), static_cast<float>(bw), 1,
                                 kText);
            }
            if (bw > 26 && record.scope < history.scopes.size()) {
                std::string text = history.scopes[record.scope].name;
                if (record.cause != profiler::kNoCause && record.cause < history.causes.size()) {
                    text += " \xC2\xB7 " + history.causes[record.cause];
                }
                const std::size_t fits = static_cast<std::size_t>((bw - 6) / 5.6);
                if (text.size() > fits) {
                    text = fits > 2 ? text.substr(0, fits - 1) + "\xE2\x80\xA6" : std::string();
                }
                if (!text.empty()) {
                    painter.text(static_cast<float>(x0 + 3), static_cast<float>(by + 1), text, kFamily, 10.f,
                                 highlight_ && !lit ? dimmed(kInk) : kInk);
                }
            }
            blocks_.push_back({{x0, by, bw, kLane - 1}, record, it->start_ns});
        }
    }
    painter.popClip();
}

void ProfilerOverlay::drawTooltip(jadefx::Painter& painter, const profiler::History& history) {
    if (!hovering_ || drag_ != Drag::None) {
        return;
    }
    const Block* hit = nullptr;
    for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
        if (it->rect.contains(mouse_x_, mouse_y_)) {
            hit = &*it;
            break;
        }
    }
    if (hit == nullptr) {
        return;
    }
    const profiler::ScopeRecord& record = hit->record;
    std::vector<std::pair<std::string, Color>> lines;
    lines.emplace_back(record.scope < history.scopes.size() ? history.scopes[record.scope].name : "?", kText);
    lines.emplace_back(format("%.3f ms", static_cast<double>(record.end_ns - record.start_ns) / 1e6) +
                           format("   starts %.2f ms into its frame",
                                  (static_cast<double>(record.start_ns) - static_cast<double>(hit->frame_start)) / 1e6),
                       kText);
    std::string where = record.row < history.rows.size() ? history.rows[record.row] : "?";
    if (record.cause != profiler::kNoCause && record.cause < history.causes.size()) {
        where += " \xC2\xB7 resumed by " + history.causes[record.cause];
    }
    lines.emplace_back(where, kMuted);
    double width = 0;
    for (const auto& line : lines) {
        width = std::max(width, textWidth(line.first));
    }
    width += 16;
    const double height = 8 + 15.0 * lines.size();
    double tx = mouse_x_ + 14;
    double ty = mouse_y_ + 16;
    if (tx + width > getAbsoluteX() + getWidth()) {
        tx = mouse_x_ - width - 8;
    }
    if (ty + height > getAbsoluteY() + getHeight()) {
        ty = mouse_y_ - height - 6;
    }
    painter.fillRect(static_cast<float>(tx), static_cast<float>(ty), static_cast<float>(width),
                     static_cast<float>(height), Color::rgb8(8, 10, 14, 245));
    painter.fillRect(static_cast<float>(tx), static_cast<float>(ty), 3, static_cast<float>(height),
                     group_color(record.scope < history.scopes.size() ? history.scopes[record.scope].group
                                                                      : profiler::Group::Engine));
    for (std::size_t index = 0; index < lines.size(); ++index) {
        label(painter, tx + 9, ty + 4 + 15.0 * index, lines[index].first, lines[index].second);
    }
}

void ProfilerOverlay::drawTable(jadefx::Painter& painter, const profiler::History& history) {
    const Rect lower = lowerRect();
    if (lower.h < kTableRow * 2) {
        return;
    }
    painter.fillRect(static_cast<float>(lower.x), static_cast<float>(lower.y), static_cast<float>(lower.w),
                     static_cast<float>(kTableRow + 2), kPanel);
    for (int column = 0; column < kColumns; ++column) {
        const Rect cell = columnRect(column);
        const std::string name = kColumnNames[column];
        const bool sorted = kColumnSorts[column] == sort_;
        const Color color = sorted ? kBudget : kMuted;
        const double nx = column <= kThreadColumn || column == kShareColumn ? cell.x + 6
                                                                            : cell.x + cell.w - 6 - textWidth(name);
        label(painter, nx, cell.y + 2, name, color);
        if (sorted) {
            // A small down arrow under the sorted column's name.
            const double ax = nx + textWidth(name) * 0.5;
            for (int step = 0; step < 3; ++step) {
                painter.fillRect(static_cast<float>(ax - 3 + step), static_cast<float>(cell.y + cell.h - 4 + step),
                                 static_cast<float>(6 - 2 * step), 1, kBudget);
            }
        }
    }
    const std::size_t visible = static_cast<std::size_t>(std::max(0.0, (lower.h - kTableRow - 2) / kTableRow));
    table_scroll_ = std::min(table_scroll_, stats_.size() > visible ? stats_.size() - visible : 0);
    for (std::size_t line = 0; line < visible && table_scroll_ + line < stats_.size(); ++line) {
        const profiler::ScopeStat& stat = stats_[table_scroll_ + line];
        const double ry = lower.y + kTableRow + 2 + line * kTableRow;
        const bool hover = hovering_ && Rect{lower.x, ry, lower.w, kTableRow}.contains(mouse_x_, mouse_y_);
        if (hover) {
            painter.fillRect(static_cast<float>(lower.x), static_cast<float>(ry), static_cast<float>(lower.w),
                             static_cast<float>(kTableRow), Color::rgb8(255, 255, 255, 16));
        } else if (line % 2 == 1) {
            painter.fillRect(static_cast<float>(lower.x), static_cast<float>(ry), static_cast<float>(lower.w),
                             static_cast<float>(kTableRow), Color::rgb8(255, 255, 255, 6));
        }
        const profiler::Group group =
            stat.scope < history.scopes.size() ? history.scopes[stat.scope].group : profiler::Group::Engine;
        const Rect scope = columnRect(kScopeColumn);
        painter.fillRect(static_cast<float>(scope.x + 6), static_cast<float>(ry + 4), 8, 8, group_color(group));
        std::string name = profiler::stat_label(stat, history);
        const std::size_t fits = static_cast<std::size_t>(std::max(0.0, (scope.w - 26) / 6.0));
        if (name.size() > fits && fits > 2) {
            name = name.substr(0, fits - 1) + "\xE2\x80\xA6";
        }
        label(painter, scope.x + 20, ry + 1, name, kText);
        label(painter, columnRect(kThreadColumn).x + 6, ry + 1,
              stat.row < history.rows.size() ? history.rows[stat.row] : "?", kMuted);
        auto number = [&](int column, const std::string& text, const Color& color) {
            const Rect cell = columnRect(column);
            label(painter, cell.x + cell.w - 6 - textWidth(text), ry + 1, text, color);
        };
        number(kMaxColumn, format("%.2f", stat.max_ms), stat.max_ms > 8.0 ? kOver : kText);
        number(kAvgColumn, format("%.2f", stat.avg_ms), kText);
        number(kFrameColumn, format("%.2f", stat.frame_ms), kText);
        number(kCallsColumn, format("%.1f", stat.calls_per_frame), kMuted);
        const Rect share = columnRect(kShareColumn);
        const double bar = std::clamp(stat.share, 0.0, 1.0) * (share.w - 52);
        painter.fillRect(static_cast<float>(share.x + 6), static_cast<float>(ry + 6),
                         static_cast<float>(share.w - 52), 4, kButton);
        painter.fillRect(static_cast<float>(share.x + 6), static_cast<float>(ry + 6), static_cast<float>(bar), 4,
                         group_color(group));
        const std::string percent = format("%.0f%%", stat.share * 100.0);
        label(painter, share.x + share.w - 6 - textWidth(percent), ry + 1, percent, kMuted);
    }
    if (stats_.empty()) {
        label(painter, lower.x + 6, lower.y + kTableRow + 6, "No scopes yet.", kMuted);
    }
}

bool ProfilerOverlay::blockRect(profiler::ScopeId scope, Rect& out) const {
    bool found = false;
    for (const Block& block : blocks_) {
        if (block.record.scope == scope && (!found || block.rect.w > out.w)) {
            out = block.rect;
            found = true;
        }
    }
    return found;
}

void ProfilerOverlay::handleMousePressed(const jadefx::MouseEvent& event) {
    mouse_x_ = event.x;
    mouse_y_ = event.y;
    ProfilerUi& ui = ProfilerUi::get();
    const Rect lower = lowerRect();
    if (event.button != 0) {
        if (lower.contains(event.x, event.y) && ui.tab == ProfilerUi::Tab::Timeline) {
            pauseHere();
            drag_ = Drag::Pan;
            drag_x_ = event.x;
            drag_start_ns_ = start_ns_;
        }
        return;
    }
    if (tabRect(ProfilerUi::Tab::Timeline).contains(event.x, event.y)) {
        ui.tab = ProfilerUi::Tab::Timeline;
        if (ui.changed) {
            ui.changed();
        }
        return;
    }
    if (tabRect(ProfilerUi::Tab::Scopes).contains(event.x, event.y)) {
        ui.tab = ProfilerUi::Tab::Scopes;
        if (ui.changed) {
            ui.changed();
        }
        return;
    }
    if (pauseRect().contains(event.x, event.y)) {
        if (follow_) {
            pauseHere();
        } else {
            ui.togglePaused();
            follow_ = !profiler::paused();
            if (follow_) {
                ui.selected = ProfilerUi::kNewest;
            }
        }
        return;
    }
    const bool capture = profiler::showing_capture();
    if (!follow_ && ui.save && saveRect().contains(event.x, event.y)) {
        ui.save();
        return;
    }
    if (capture && closeCaptureRect().contains(event.x, event.y)) {
        profiler::close_capture();
        follow_ = true;
        ui.selected = ProfilerUi::kNewest;
        return;
    }
    if (graphRect().contains(event.x, event.y)) {
        std::size_t count = 0;
        profiler::with_view([&](const profiler::History& history) { count = history.frames.size(); });
        for (std::size_t index = 0; index < count; ++index) {
            if (barRect(index, count).contains(event.x, event.y)) {
                selectFrame(index);
                return;
            }
        }
        return;
    }
    if (splitterRect().contains(event.x, event.y)) {
        drag_ = Drag::Split;
        drag_y_ = event.y;
        drag_split_ = ui.split;
        return;
    }
    if (!lower.contains(event.x, event.y)) {
        return;
    }
    if (ui.tab == ProfilerUi::Tab::Scopes) {
        for (int column = 0; column < kColumns; ++column) {
            if (columnRect(column).contains(event.x, event.y)) {
                sort_ = kColumnSorts[column];
                return;
            }
        }
        const std::size_t line = static_cast<std::size_t>((event.y - lower.y - kTableRow - 2) / kTableRow);
        if (event.y >= lower.y + kTableRow + 2 && table_scroll_ + line < stats_.size()) {
            const profiler::ScopeStat& stat = stats_[table_scroll_ + line];
            highlight_ = true;
            highlight_scope_ = stat.scope;
            highlight_cause_ = stat.cause;
            ui.tab = ProfilerUi::Tab::Timeline;
            if (ui.changed) {
                ui.changed();
            }
        }
        return;
    }
    // The timeline: a double click fits the scope under the pointer; a drag pans.
    if (event.clickCount >= 2) {
        for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {
            if (it->rect.contains(event.x, event.y)) {
                pauseHere();
                const double length = static_cast<double>(it->record.end_ns - it->record.start_ns);
                span_ns_ = std::max(length * 1.2, 20e3);
                start_ns_ = static_cast<double>(it->record.start_ns) - length * 0.1;
                return;
            }
        }
        highlight_ = false;
        return;
    }
    drag_ = Drag::Pan;
    drag_x_ = event.x;
    drag_start_ns_ = -1;
}

void ProfilerOverlay::handleMouseDragged(const jadefx::MouseEvent& event) {
    mouse_x_ = event.x;
    mouse_y_ = event.y;
    if (drag_ == Drag::Split) {
        const double view = getParent() != nullptr ? getParent()->getHeight() : getHeight();
        if (view > 0) {
            ProfilerUi::get().split = std::clamp(drag_split_ + (event.y - drag_y_) / view, 0.2, 0.8);
        }
        return;
    }
    if (drag_ != Drag::Pan) {
        return;
    }
    if (drag_start_ns_ < 0) {
        // The first move of a left drag: only now is it a pan, not a click.
        if (std::abs(event.x - drag_x_) < 3) {
            return;
        }
        pauseHere();
        drag_start_ns_ = start_ns_;
    }
    const Rect lower = lowerRect();
    const double width = std::max(1.0, lower.w - kRowGutter);
    start_ns_ = drag_start_ns_ - (event.x - drag_x_) / width * span_ns_;
}

void ProfilerOverlay::handleMouseReleased(const jadefx::MouseEvent& event) {
    mouse_x_ = event.x;
    mouse_y_ = event.y;
    if (drag_ == Drag::Split) {
        ProfilerUi& ui = ProfilerUi::get();
        if (ui.changed) {
            ui.changed();
        }
    }
    drag_ = Drag::None;
}

void ProfilerOverlay::handleMouseMoved(const jadefx::MouseEvent& event) {
    mouse_x_ = event.x;
    mouse_y_ = event.y;
    hovering_ = true;
}

void ProfilerOverlay::handleHoverChanged() {
    hovering_ = isHovered();
}

void ProfilerOverlay::handleScroll(jadefx::ScrollEvent& event) {
    event.consume();
    const Rect lower = lowerRect();
    if (!lower.contains(event.x, event.y)) {
        return;
    }
    if (ProfilerUi::get().tab == ProfilerUi::Tab::Scopes) {
        const double rows = -event.deltaY * 3.0;
        if (rows < 0) {
            table_scroll_ = table_scroll_ > static_cast<std::size_t>(-rows) ? table_scroll_ - static_cast<std::size_t>(-rows) : 0;
        } else {
            table_scroll_ += static_cast<std::size_t>(rows);
        }
        return;
    }
    // Zoom about the pointer; live, about the newest edge.
    const double factor = event.deltaY > 0 ? 1.0 / 1.25 : 1.25;
    const double next = std::clamp(span_ns_ * factor, 20e3, 2e9);
    if (follow_) {
        span_ns_ = next;
        return;
    }
    const double width = std::max(1.0, lower.w - kRowGutter);
    const double at = std::clamp((event.x - lower.x - kRowGutter) / width, 0.0, 1.0);
    const double pointer_ns = start_ns_ + at * span_ns_;
    span_ns_ = next;
    start_ns_ = pointer_ns - at * span_ns_;
}

}  // namespace runner
