#pragma once

#include "profiler/ProfileStats.hpp"
#include "profiler/Profiler.hpp"

#include "jadefx/jadefx.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace runner {

class GameView;

// What every Scene View's profiler shows, one for the process. Showing it
// starts recording (profiler::acquire) and hiding it stops (release); the
// history it recorded stays until it is shown again. Hiding ends a pause and
// closes a capture, so it always comes back live. Only owner draws it: the
// Scene View last clicked or focused.
class ProfilerUi {
public:
    enum class Tab { Timeline, Scopes };
    static constexpr std::size_t kNewest = static_cast<std::size_t>(-1);

    static ProfilerUi& get();

    bool shown() const { return shown_; }
    void setShown(bool shown);
    void toggleShown() { setShown(!shown_); }
    // Pauses or resumes. Resuming a capture closes it and returns to live.
    void togglePaused();

    // The view that draws it. A view that leaves its window lets go; the next
    // one laid out while it is shown takes it.
    GameView* owner = nullptr;
    Tab tab = Tab::Timeline;
    // The lower half's share of the view's height, 0.2 to 0.8.
    double split = 0.45;
    // The selected frame, an index into the paused history; kNewest when live.
    std::size_t selected = kNewest;
    // Writes the paused history to a file. Null hides Save.
    std::function<void()> save;
    // The tab or split changed, for preferences.
    std::function<void()> changed;

private:
    bool shown_ = false;
};

// The profiler drawn over a Scene View: a header (the tabs, pause, Save), the
// frame graph, and below them the Timeline (a flame chart per thread) or the
// Scopes table. It draws with a fixed dark palette, since it sits over the
// scene in the studio and in a game alike. All hit testing uses the layout's
// geometry, so it works before the first paint.
class ProfilerOverlay : public jadefx::Region {
public:
    struct Rect {
        double x = 0;
        double y = 0;
        double w = 0;
        double h = 0;
        bool contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    };
    // The table's columns, left to right.
    enum Column { kScopeColumn, kThreadColumn, kMaxColumn, kAvgColumn, kFrameColumn, kCallsColumn, kShareColumn,
                  kColumns };

    ProfilerOverlay();
    const char* getElementType() const override { return "profiler"; }

    // A timeline block's fill, opaque, as drawn: bright, or blended most of the way
    // into the background when another scope is highlighted. Fixed, not themed.
    static jadefx::Color blockColor(profiler::Group group, bool dimmed);
    // Dark or light text, whichever reads better on fill.
    static jadefx::Color labelColor(const jadefx::Color& fill);

    // How tall it is over a view of this height.
    static double heightFor(double viewHeight, double split);

    Rect tabRect(ProfilerUi::Tab tab) const;
    Rect pauseRect() const;
    Rect saveRect() const;
    Rect closeCaptureRect() const;
    // Switches the GPU row between the whole 3D draw once a frame and each pass.
    Rect gpuDetailRect() const;
    Rect graphRect() const;
    // Frame index's bar among count frames.
    Rect barRect(std::size_t index, std::size_t count) const;
    Rect lowerRect() const;
    Rect columnRect(int column) const;
    // The lower half's top edge, which a drag moves.
    Rect splitterRect() const;

    profiler::StatSort sort() const { return sort_; }
    // The widest block of this scope in the last painted Timeline. False when none is drawn.
    bool blockRect(profiler::ScopeId scope, Rect& out) const;

protected:
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void handleMousePressed(const jadefx::MouseEvent& event) override;
    void handleMouseReleased(const jadefx::MouseEvent& event) override;
    void handleMouseDragged(const jadefx::MouseEvent& event) override;
    void handleMouseMoved(const jadefx::MouseEvent& event) override;
    void handleScroll(jadefx::ScrollEvent& event) override;
    void handleHoverChanged() override;

private:
    struct Block {
        Rect rect;
        profiler::ScopeRecord record;
        std::uint64_t frame_start = 0;
    };
    enum class Drag { None, Pan, Split };

    void drawHeader(jadefx::Painter& painter, const profiler::History& history);
    void drawGraph(jadefx::Painter& painter, const profiler::History& history);
    void drawTimeline(jadefx::Painter& painter, const profiler::History& history);
    void drawTable(jadefx::Painter& painter, const profiler::History& history);
    void drawTooltip(jadefx::Painter& painter, const profiler::History& history);
    void button(jadefx::Painter& painter, const Rect& rect, const std::string& text, bool on);
    void label(jadefx::Painter& painter, double x, double y, const std::string& text, const jadefx::Color& color,
               float size = 11.f);
    double textWidth(const std::string& text, float size = 11.f) const;

    // The selected frame in history, or the newest.
    std::size_t selectedIn(const profiler::History& history) const;
    // Live: the newest frames. Paused: where the view was put.
    void timelineWindow(const profiler::History& history, double& start_ns, double& span_ns) const;
    void selectFrame(std::size_t index);
    void refreshStats(const profiler::History& history);
    void pauseHere();

    profiler::StatSort sort_ = profiler::StatSort::Max;
    // The timeline's width in ns, and its left edge while paused.
    double span_ns_ = 50e6;
    double start_ns_ = 0;
    bool follow_ = true;
    // The Scopes row last clicked, drawn bright in the Timeline.
    bool highlight_ = false;
    profiler::ScopeId highlight_scope_ = 0;
    profiler::CauseId highlight_cause_ = profiler::kNoCause;
    std::size_t table_scroll_ = 0;

    // From the last paint, for hovering.
    std::vector<Block> blocks_;
    std::vector<profiler::ScopeStat> stats_;
    std::size_t stats_frames_ = 0;
    std::size_t stats_selected_ = 0;
    profiler::StatSort stats_sort_ = profiler::StatSort::Max;
    double stats_at_ = -1;
    bool stats_paused_ = false;

    double mouse_x_ = -1;
    double mouse_y_ = -1;
    bool hovering_ = false;
    Drag drag_ = Drag::None;
    double drag_x_ = 0;
    double drag_y_ = 0;
    double drag_start_ns_ = 0;
    double drag_split_ = 0;
};

}  // namespace runner
