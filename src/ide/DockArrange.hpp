#pragma once

#include <functional>
#include <memory>

namespace jadefx {
class Node;
}

namespace ide {

struct Extent {
    double width = 0;
    double height = 0;
};

// A rectangle in window points.
struct Box {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

// Where a pointer sits inside one dock. Header is the tab strip.
// The content below it is split into a merge band and four edge bands.
// Top is the upper portion, bottom is the lower half, and the left and right
// bands are the outer quarters. The preview for a merge is the whole content
// area. The preview for an edge is that half of the dock.
enum class DockZone { Outside, Header, Center, Left, Right, Top, Bottom };

enum class DropSide { Left, Right, Top, Bottom };

// Minimum of a split is the sum of its children along the split, and the
// largest child across it. A tab dock's minimum is whatever it has set.
Extent minimumExtent(const jadefx::Node* node);

DockZone dockZone(Box dock, double header, double x, double y);
Box dockPreview(Box dock, double header, DockZone zone);

// The nearest work-area edge within `margin`. Corners belong to the left or right.
DockZone screenEdge(Box work, double margin, double x, double y);
Box edgePreview(Box work, DockZone edge, double depth);

// A split with one child is replaced by that child. An empty split is removed.
// The walk stops at `stop` without removing it. A scene root is replaced too.
void liftDegenerateSplits(const std::shared_ptr<jadefx::Node>& start, jadefx::Node* stop,
                          const std::function<std::shared_ptr<jadefx::Node>(jadefx::Node*)>& share,
                          const std::function<void(jadefx::Node& parent, const std::shared_ptr<jadefx::Node>& previous,
                                                   const std::shared_ptr<jadefx::Node>& replacement)>& replaced);

// Wraps `target` in a new split so `incoming` occupies the given side.
// The new half is half of the target. The target's outer resize flag moves to the wrapper.
bool splitBeside(jadefx::Node& target, const std::shared_ptr<jadefx::Node>& incoming, DropSide side,
                 const std::function<std::shared_ptr<jadefx::Node>(jadefx::Node*)>& share,
                 const std::function<void(jadefx::Node& parent, const std::shared_ptr<jadefx::Node>& previous,
                                          const std::shared_ptr<jadefx::Node>& replacement)>& replaced);

// Adds `incoming` on a side of the whole area. A matching split gains a child.
// Anything else is wrapped. `fraction` is the incoming share of that area.
// The incoming child keeps its size when the parent split grows.
bool splitEdge(jadefx::Node& area, const std::shared_ptr<jadefx::Node>& incoming, DropSide side, double fraction,
               const std::function<std::shared_ptr<jadefx::Node>(jadefx::Node*)>& share,
               const std::function<void(jadefx::Node& parent, const std::shared_ptr<jadefx::Node>& previous,
                                        const std::shared_ptr<jadefx::Node>& replacement)>& replaced);

}  // namespace ide
