#include "DockArrange.hpp"

#include "jadefx/scene/Scene.hpp"
#include "jadefx/scene/controls/SplitPane.hpp"
#include "jadefx/scene/layout/BorderPane.hpp"

#include <algorithm>
#include <memory>
#include <vector>

namespace ide {
namespace {

// Thickness is the divider's horizontal padding plus border, for both
// orientations. Nested dividers belong to an inner split. Their long side is
// the inner pane, so measuring getWidth/getHeight here would grow the window
// without bound.
double DividerThickness(const jadefx::SplitPane& split) {
    auto& readable = const_cast<jadefx::SplitPane&>(split);
    for (jadefx::Node* divider : readable.getElementsByClassName("split-pane-divider")) {
        if (divider == nullptr || divider->getParent() != &split) {
            continue;
        }
        const jadefx::ComputedStyle& style = divider->computedStyle();
        const double thickness = style.padding.left + style.padding.right + style.border.left + style.border.right;
        if (thickness > 0.5) {
            return thickness;
        }
    }
    return 8;
}

jadefx::Node* structuralParent(jadefx::Node* node) {
    jadefx::Node* parent = node != nullptr ? node->getParent() : nullptr;
    while (parent != nullptr && dynamic_cast<jadefx::SplitPane*>(parent) == nullptr &&
           dynamic_cast<jadefx::BorderPane*>(parent) == nullptr && dynamic_cast<jadefx::Scene*>(parent) == nullptr) {
        parent = parent->getParent();
    }
    return parent;
}

bool horizontalSide(DropSide side) { return side == DropSide::Left || side == DropSide::Right; }

bool leadingSide(DropSide side) { return side == DropSide::Left || side == DropSide::Top; }

double clampFraction(double fraction) {
    if (fraction < 0.12) {
        return 0.12;
    }
    if (fraction > 0.5) {
        return 0.5;
    }
    return fraction;
}

void setPositions(jadefx::SplitPane& split, const std::vector<double>& positions) {
    for (std::size_t i = 0; i < positions.size() && i < split.getDividers().size(); ++i) {
        split.setDividerPosition(static_cast<int>(i), positions[i]);
    }
}

bool replaceChild(jadefx::Node& parent, const std::shared_ptr<jadefx::Node>& child,
                  const std::shared_ptr<jadefx::Node>& replacement) {
    if (auto* split = dynamic_cast<jadefx::SplitPane*>(&parent)) {
        jadefx::ObservableList<std::shared_ptr<jadefx::Node>>& items = split->getItems();
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i] != child) {
                continue;
            }
            // Removing and inserting rebuilds the divider list. A new divider
            // starts at 0.5, which would resize the siblings. Keep their fractions
            // when the replacement leaves the same number of dividers.
            const std::vector<double> positions = split->getDividerPositions();
            items.removeAt(i);
            if (replacement) {
                items.insert(i, replacement);
            }
            if (split->getDividers().size() == positions.size()) {
                setPositions(*split, positions);
            }
            return true;
        }
        return false;
    }
    if (auto* scene = dynamic_cast<jadefx::Scene*>(&parent)) {
        if (scene->getRoot() != child.get()) {
            return false;
        }
        scene->setRoot(replacement);
        return true;
    }
    auto* border = dynamic_cast<jadefx::BorderPane*>(&parent);
    if (border == nullptr) {
        return false;
    }
    if (border->getCenter() == child.get()) {
        border->setCenter(replacement);
    } else if (border->getTop() == child.get()) {
        border->setTop(replacement);
    } else if (border->getBottom() == child.get()) {
        border->setBottom(replacement);
    } else if (border->getLeft() == child.get()) {
        border->setLeft(replacement);
    } else if (border->getRight() == child.get()) {
        border->setRight(replacement);
    } else {
        return false;
    }
    return true;
}

bool wrapNode(jadefx::Node& target, const std::shared_ptr<jadefx::Node>& incoming, DropSide side, double fraction,
              bool incomingResizable, const std::function<std::shared_ptr<jadefx::Node>(jadefx::Node*)>& share,
              const std::function<void(jadefx::Node& parent, const std::shared_ptr<jadefx::Node>& previous,
                                       const std::shared_ptr<jadefx::Node>& replacement)>& replaced) {
    if (!incoming || !share) {
        return false;
    }
    const std::shared_ptr<jadefx::Node> held = share(&target);
    jadefx::Node* parent = structuralParent(&target);
    if (!held || parent == nullptr) {
        return false;
    }
    const bool keep = jadefx::SplitPane::isResizableWithParent(*held);
    auto* parentSplit = dynamic_cast<jadefx::SplitPane*>(parent);
    const std::vector<double> parentPositions = parentSplit != nullptr ? parentSplit->getDividerPositions() : std::vector<double>();
    auto nested = std::make_shared<jadefx::SplitPane>();
    nested->setOrientation(horizontalSide(side) ? jadefx::Orientation::Horizontal : jadefx::Orientation::Vertical);
    if (!replaceChild(*parent, held, nested)) {
        return false;
    }
    const double cut = clampFraction(fraction);
    if (leadingSide(side)) {
        nested->getItems().add(incoming);
        nested->getItems().add(held);
        nested->setDividerPositions({cut});
    } else {
        nested->getItems().add(held);
        nested->getItems().add(incoming);
        nested->setDividerPositions({1.0 - cut});
    }
    jadefx::SplitPane::setResizableWithParent(*nested, keep);
    jadefx::SplitPane::setResizableWithParent(*incoming, incomingResizable);
    jadefx::SplitPane::setResizableWithParent(*held, true);
    if (parentSplit != nullptr && parentSplit->getDividers().size() == parentPositions.size()) {
        setPositions(*parentSplit, parentPositions);
    }
    if (replaced) {
        replaced(*parent, held, nested);
    }
    return true;
}

}  // namespace

Extent minimumExtent(const jadefx::Node* node) {
    Extent extent;
    if (node == nullptr) {
        return extent;
    }
    const auto* split = dynamic_cast<const jadefx::SplitPane*>(node);
    if (split == nullptr) {
        extent.width = node->getMinWidth();
        extent.height = node->getMinHeight();
        return extent;
    }
    const bool horizontal = split->getOrientation() == jadefx::Orientation::Horizontal;
    const double bars = DividerThickness(*split) * static_cast<double>(split->getItems().empty() ? 0 : split->getItems().size() - 1);
    for (const std::shared_ptr<jadefx::Node>& item : split->getItems().items()) {
        const Extent child = minimumExtent(item.get());
        if (horizontal) {
            extent.width += child.width;
            extent.height = std::max(extent.height, child.height);
        } else {
            extent.height += child.height;
            extent.width = std::max(extent.width, child.width);
        }
    }
    if (horizontal) {
        extent.width += bars;
    } else {
        extent.height += bars;
    }
    return extent;
}

void liftDegenerateSplits(
    const std::shared_ptr<jadefx::Node>& start, jadefx::Node* stop,
    const std::function<std::shared_ptr<jadefx::Node>(jadefx::Node*)>& share,
    const std::function<void(jadefx::Node& parent, const std::shared_ptr<jadefx::Node>& previous,
                             const std::shared_ptr<jadefx::Node>& replacement)>& replaced) {
    std::shared_ptr<jadefx::Node> node = start;
    while (node && node.get() != stop) {
        auto* split = dynamic_cast<jadefx::SplitPane*>(node.get());
        if (split == nullptr || split->getItems().size() >= 2) {
            return;
        }
        std::shared_ptr<jadefx::Node> replacement;
        if (split->getItems().size() == 1) {
            replacement = split->getItems()[0];
        }
        // Split children are parented to an internal host, not the split itself.
        jadefx::Node* parent = structuralParent(node.get());
        if (parent == nullptr) {
            return;
        }
        std::shared_ptr<jadefx::Node> parentHeld = parent == stop || !share ? std::shared_ptr<jadefx::Node>() : share(parent);
        const std::shared_ptr<jadefx::Node> previous = node;
        if (!replaceChild(*parent, node, replacement)) {
            return;
        }
        if (replaced) {
            replaced(*parent, previous, replacement);
        }
        if (parent == stop || !parentHeld) {
            return;
        }
        node = parentHeld;
    }
}

DockZone dockZone(Box dock, double header, double x, double y) {
    if (dock.width <= 0.0 || dock.height <= 0.0 || x < dock.x || y < dock.y || x >= dock.x + dock.width ||
        y >= dock.y + dock.height) {
        return DockZone::Outside;
    }
    if (header < 0.0) {
        header = 0.0;
    }
    if (header > dock.height) {
        header = dock.height;
    }
    if (header > 0.0 && y < dock.y + header) {
        return DockZone::Header;
    }
    const double width = dock.width;
    const double height = dock.height - header;
    if (width < 1.0 || height < 8.0) {
        return DockZone::Center;
    }
    const double nx = (x - dock.x) / width;
    const double ny = (y - (dock.y + header)) / height;
    const bool middle = nx >= 0.25 && nx < 0.75;
    if (middle && ny >= 0.22 && ny < 0.5) {
        return DockZone::Center;
    }
    if (middle && ny >= 0.5) {
        return DockZone::Bottom;
    }
    if (middle && ny < 0.22) {
        return DockZone::Top;
    }
    return nx < 0.5 ? DockZone::Left : DockZone::Right;
}

Box dockPreview(Box dock, double header, DockZone zone) {
    if (header < 0.0) {
        header = 0.0;
    }
    if (header > dock.height) {
        header = dock.height;
    }
    Box content;
    content.x = dock.x;
    content.y = dock.y + header;
    content.width = dock.width;
    content.height = std::max(0.0, dock.height - header);
    Box half = dock;
    switch (zone) {
        case DockZone::Center:
            return content.height > 0.0 ? content : dock;
        case DockZone::Left:
            half.width = dock.width * 0.5;
            return half;
        case DockZone::Right:
            half.x = dock.x + dock.width * 0.5;
            half.width = dock.width * 0.5;
            return half;
        case DockZone::Top:
            half.height = dock.height * 0.5;
            return half;
        case DockZone::Bottom:
            half.y = dock.y + dock.height * 0.5;
            half.height = dock.height * 0.5;
            return half;
        case DockZone::Header:
        case DockZone::Outside:
            return {};
    }
    return {};
}

DockZone screenEdge(Box work, double margin, double x, double y) {
    if (margin <= 0.0 || work.width <= 0.0 || work.height <= 0.0 || x < work.x || y < work.y ||
        x >= work.x + work.width || y >= work.y + work.height) {
        return DockZone::Outside;
    }
    const double left = x - work.x;
    const double right = work.x + work.width - x;
    const double top = y - work.y;
    const double bottom = work.y + work.height - y;
    double best = margin + 1.0;
    DockZone zone = DockZone::Outside;
    auto take = [&](double distance, DockZone candidate) {
        if (distance <= margin && distance < best) {
            best = distance;
            zone = candidate;
        }
    };
    take(left, DockZone::Left);
    take(right, DockZone::Right);
    take(top, DockZone::Top);
    take(bottom, DockZone::Bottom);
    return zone;
}

Box edgePreview(Box work, DockZone edge, double depth) {
    if (depth < 1.0) {
        depth = 1.0;
    }
    Box band = work;
    switch (edge) {
        case DockZone::Left:
            band.width = std::min(depth, work.width);
            return band;
        case DockZone::Right:
            band.width = std::min(depth, work.width);
            band.x = work.x + work.width - band.width;
            return band;
        case DockZone::Top:
            band.height = std::min(depth, work.height);
            return band;
        case DockZone::Bottom:
            band.height = std::min(depth, work.height);
            band.y = work.y + work.height - band.height;
            return band;
        case DockZone::Header:
        case DockZone::Center:
        case DockZone::Outside:
            return {};
    }
    return {};
}

bool splitBeside(jadefx::Node& target, const std::shared_ptr<jadefx::Node>& incoming, DropSide side,
                 const std::function<std::shared_ptr<jadefx::Node>(jadefx::Node*)>& share,
                 const std::function<void(jadefx::Node& parent, const std::shared_ptr<jadefx::Node>& previous,
                                          const std::shared_ptr<jadefx::Node>& replacement)>& replaced) {
    // Always wrap the target. Inserting into the parent split would resize the
    // neighboring sections instead of dividing this one.
    return wrapNode(target, incoming, side, 0.5, true, share, replaced);
}

bool splitEdge(jadefx::Node& area, const std::shared_ptr<jadefx::Node>& incoming, DropSide side, double fraction,
               const std::function<std::shared_ptr<jadefx::Node>(jadefx::Node*)>& share,
               const std::function<void(jadefx::Node& parent, const std::shared_ptr<jadefx::Node>& previous,
                                        const std::shared_ptr<jadefx::Node>& replacement)>& replaced) {
    if (!incoming) {
        return false;
    }
    const double cut = clampFraction(fraction);
    const bool horizontal = horizontalSide(side);
    auto* split = dynamic_cast<jadefx::SplitPane*>(&area);
    if (split != nullptr && split->getItems().size() >= 1 &&
        (split->getOrientation() == jadefx::Orientation::Horizontal) == horizontal) {
        const std::vector<double> positions = split->getDividerPositions();
        std::vector<double> next;
        if (leadingSide(side)) {
            split->getItems().insert(0, incoming);
            next.push_back(cut);
            for (double position : positions) {
                next.push_back(cut + (1.0 - cut) * position);
            }
        } else {
            split->getItems().add(incoming);
            for (double position : positions) {
                next.push_back(position * (1.0 - cut));
            }
            next.push_back(1.0 - cut);
        }
        setPositions(*split, next);
        jadefx::SplitPane::setResizableWithParent(*incoming, false);
        return true;
    }
    return wrapNode(area, incoming, side, cut, false, share, replaced);
}

}  // namespace ide
