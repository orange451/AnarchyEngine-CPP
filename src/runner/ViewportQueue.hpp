#pragma once

#include "types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine_core {
struct VisualViewport;
}

namespace runner {

// Per host per frame: at most this many ViewportPane renders, and this many pixels.
constexpr int kViewportPanesPerFrame = 8;
constexpr long long kViewportPixelsPerFrame = 1 << 20;
// The largest side a pane renders at; a larger pane is scaled up.
constexpr int kViewportMaxSide = 1024;
// A pane not shown for this many frames is let go.
constexpr int kViewportForgetFrames = 60;

// points across at pixelsPerPoint, in device pixels: rounded up to a multiple
// of 8, at most kViewportMaxSide; 0 for none.
int ViewportPixels(double points, float pixelsPerPoint);

// Which ViewportPanes a host renders each frame, kept without GL so it can be
// tested: the newest scene of each pane (setScene, from layout), whether it was
// painted and at what pixel size (shown, from paint), and what was last drawn.
// renderPending runs once a frame, before the panes paint. Among the panes shown
// since the last call whose scene, size, or incomplete last draw needs a render,
// it draws those with no image yet first, then the oldest change, until
// kViewportPanesPerFrame panes or kViewportPixelsPerFrame pixels, and always at
// least one. An incomplete draw waits behind the others before it is tried again.
class ViewportQueue {
public:
    struct Job {
        engine_core::InstanceId pane = 0;
        std::shared_ptr<const engine_core::VisualViewport> scene;
        int width = 0;
        int height = 0;
    };
    // Complete: drawn as the scene says. Incomplete: drawn, but something was
    // still loading, so it is drawn again. NotReady: nothing drawn.
    enum class Result { Complete, Incomplete, NotReady };
    using Draw = std::function<Result(const Job&)>;

    void setScene(engine_core::InstanceId pane, std::shared_ptr<const engine_core::VisualViewport> scene);
    void shown(engine_core::InstanceId pane, int width, int height);
    // One frame: draws, with draw, the panes shown since the last call that need
    // it, by the budget, oldest change first. Returns how many it drew.
    int renderPending(const Draw& draw);
    // The panes not shown for kViewportForgetFrames frames, removed here, for
    // their owner to delete. out is replaced.
    void takeForgotten(std::vector<engine_core::InstanceId>& out);
    // Whether pane has an image to show, and the size it was drawn at.
    bool image(engine_core::InstanceId pane, int& width, int& height) const;
    // Forgets every pane, as when GL is released; each goes to takeForgotten.
    void clear();

private:
    struct Entry {
        std::shared_ptr<const engine_core::VisualViewport> scene;
        int shownWidth = 0;
        int shownHeight = 0;
        bool shown = false;
        std::uint64_t lastShown = 0;
        // What was last drawn, and whether that draw was complete.
        std::uint64_t drawnRevision = 0;
        int drawnWidth = 0;
        int drawnHeight = 0;
        bool hasImage = false;
        bool complete = false;
        // The frame it first needed a render since its last complete one; 0 when it needs none.
        std::uint64_t dirtySince = 0;
    };
    std::unordered_map<engine_core::InstanceId, Entry> entries_;
    std::vector<engine_core::InstanceId> forgotten_;
    // Due panes by priority: no image yet first, then the oldest change, then id.
    std::vector<std::tuple<bool, std::uint64_t, engine_core::InstanceId>> due_;
    std::uint64_t frame_ = 1;
};

}  // namespace runner
