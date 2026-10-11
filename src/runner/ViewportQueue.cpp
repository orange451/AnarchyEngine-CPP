#include "ViewportQueue.hpp"

#include "SnapshotPump.hpp"

#include <algorithm>
#include <cmath>

namespace runner {

int ViewportPixels(double points, float pixelsPerPoint) {
    // The small slack keeps a size that is whole but for rounding from going up a step.
    const double pixels = std::ceil(points * static_cast<double>(pixelsPerPoint) - 1e-6);
    if (!(pixels > 0.0)) {
        return 0;
    }
    const int rounded = (static_cast<int>(std::min(pixels, 1.0e6)) + 7) / 8 * 8;
    return std::min(rounded, kViewportMaxSide);
}

void ViewportQueue::setScene(engine_core::InstanceId pane,
                             std::shared_ptr<const engine_core::VisualViewport> scene) {
    entries_[pane].scene = std::move(scene);
}

void ViewportQueue::shown(engine_core::InstanceId pane, int width, int height) {
    Entry& entry = entries_[pane];
    entry.shown = true;
    entry.shownWidth = width;
    entry.shownHeight = height;
    entry.lastShown = frame_;
}

int ViewportQueue::renderPending(const Draw& draw) {
    due_.clear();
    for (auto& [pane, entry] : entries_) {
        if (!entry.shown || entry.scene == nullptr || entry.shownWidth <= 0 || entry.shownHeight <= 0) {
            continue;
        }
        const bool needed = !entry.complete || entry.scene->revision != entry.drawnRevision ||
                            entry.shownWidth != entry.drawnWidth || entry.shownHeight != entry.drawnHeight;
        if (!needed) {
            continue;
        }
        if (entry.dirtySince == 0) {
            entry.dirtySince = frame_;
        }
        due_.emplace_back(entry.dirtySince, pane);
    }
    std::sort(due_.begin(), due_.end());
    int drew = 0;
    long long pixels = 0;
    for (const auto& due : due_) {
        Entry& entry = entries_[due.second];
        const long long cost = static_cast<long long>(entry.shownWidth) * entry.shownHeight;
        if (drew > 0 && (drew >= kViewportPanesPerFrame || pixels + cost > kViewportPixelsPerFrame)) {
            break;
        }
        const Job job{due.second, entry.scene, entry.shownWidth, entry.shownHeight};
        const Result result = draw(job);
        ++drew;
        pixels += cost;
        if (result == Result::NotReady) {
            continue;
        }
        entry.hasImage = true;
        entry.drawnWidth = job.width;
        entry.drawnHeight = job.height;
        entry.drawnRevision = job.scene->revision;
        entry.complete = result == Result::Complete;
        if (entry.complete) {
            entry.dirtySince = 0;
        }
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        it->second.shown = false;
        if (frame_ - it->second.lastShown > static_cast<std::uint64_t>(kViewportForgetFrames)) {
            forgotten_.push_back(it->first);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    ++frame_;
    return drew;
}

void ViewportQueue::takeForgotten(std::vector<engine_core::InstanceId>& out) {
    out.swap(forgotten_);
    forgotten_.clear();
}

bool ViewportQueue::image(engine_core::InstanceId pane, int& width, int& height) const {
    const auto found = entries_.find(pane);
    if (found == entries_.end() || !found->second.hasImage) {
        return false;
    }
    width = found->second.drawnWidth;
    height = found->second.drawnHeight;
    return true;
}

void ViewportQueue::clear() {
    for (const auto& entry : entries_) {
        forgotten_.push_back(entry.first);
    }
    entries_.clear();
}

}  // namespace runner
