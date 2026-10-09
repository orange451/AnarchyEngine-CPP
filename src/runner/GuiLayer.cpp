#include "GuiLayer.hpp"

#include "AssetInstances.hpp"
#include "BillboardMath.hpp"
#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "Gui.hpp"
#include "SceneService.hpp"
#include "ScriptRuntime.hpp"
#include "TextureCache.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace runner {
const char* GuiLayer::defaultStylesheet() {
    return R"CSS(
/* The game UI's starting point: no outlines, backgrounds, or padding.
   Hover, press, and focus feedback stay, colored by the variables below. */
:root {
    --text-color: #000000;
    --border-color: transparent;
    --surface-color: transparent;
    --accent-color: #1a73e8;
    --outline-color: var(--accent-color);
    --wash-color: rgba(0, 0, 0, 0.04);
    --text-selection-color: rgba(26, 115, 232, 0.3);
    color: var(--text-color);
}
button, textfield {
    padding: 0;
}
)CSS";
}

GuiLayer::GuiLayer(engine_core::Engine& engine, GuiInput input)
    : engine_(engine), game_(engine.datamodel()), input_(std::make_shared<GuiInput>(std::move(input))),
      tree_(std::make_unique<GuiTree>(engine, input_, [this] { return game_.resources_root(); })) {
    setMinSize(0, 0);
    setPickOnBounds(false);
}

GuiLayer::~GuiLayer() = default;

jadefx::Node* GuiLayer::nodeFor(engine_core::InstanceId id) const {
    return tree_->nodeFor(id);
}

void GuiLayer::sync() {
    {
        // The simulation may be inside a step. Skip this frame rather than wait.
        engine_core::DataModelLock lock(game_, engine_core::DataModelLock::Read, std::chrono::milliseconds(1));
        if (!lock.owns()) {
            return;
        }
        syncTree();
    }
    tree_->updateImages();
}

void GuiLayer::syncTree() {
    tree_->beginPass();
    std::vector<std::shared_ptr<jadefx::Node>> screens;
    std::string css;
    if (const engine_core::InstanceId service = game_.scene_service("Gui"); service != 0) {
        collectScreens(service, screens);
        // The CSS instances under the service style every ScreenGui.
        for (engine_core::InstanceId child = game_.first_child(service); child != 0; child = game_.next_sibling(child)) {
            if (const auto* sheet = dynamic_cast<const engine_core::Css*>(game_.instance(child))) {
                css += sheet->source();
                css += '\n';
            }
        }
    }
    if (css != css_) {
        setStylesheet(css);
        css_ = std::move(css);
    }
    std::vector<engine_core::InstanceId> boardIds;
    std::vector<std::shared_ptr<jadefx::Node>> boardNodes;
    collectBillboards(boardIds, boardNodes);
    // Each billboard keeps last frame's placement until placeBillboards runs.
    std::vector<Placement> placements;
    placements.reserve(boardNodes.size());
    for (std::size_t i = 0; i < boardNodes.size(); ++i) {
        Placement next;
        for (const Placement& was : placements_) {
            if (was.id == boardIds[i] && was.node == boardNodes[i]) {
                next = was;
                break;
            }
        }
        next.id = boardIds[i];
        next.node = boardNodes[i];
        placements.push_back(std::move(next));
    }
    placements_ = std::move(placements);
    screens_ = std::move(screens);
    restack();
    // The screens and billboards it places may have changed.
    markLayoutDirty(LayoutDirt::Arrange);
    // What is no longer drawn lets go of its children and goes.
    tree_->endPass();
}

void GuiLayer::collectScreens(engine_core::InstanceId id, std::vector<std::shared_ptr<jadefx::Node>>& out) {
    for (engine_core::InstanceId child = game_.first_child(id); child != 0; child = game_.next_sibling(child)) {
        const engine_core::DataModel* object = game_.instance(child);
        if (const auto* screen = dynamic_cast<const engine_core::ScreenGui*>(object)) {
            out.push_back(tree_->build(child, *screen));
        } else if (dynamic_cast<const engine_core::Folder*>(object) != nullptr) {
            collectScreens(child, out);
        }
    }
}

void GuiLayer::collectBillboards(std::vector<engine_core::InstanceId>& ids,
                                 std::vector<std::shared_ptr<jadefx::Node>>& nodes) {
    std::vector<engine_core::InstanceId> found;
    game_.billboards(found);
    std::sort(found.begin(), found.end());
    for (engine_core::InstanceId id : found) {
        const auto* board = dynamic_cast<const engine_core::BillboardGui*>(game_.instance(id));
        if (board != nullptr && board->drawn()) {
            ids.push_back(id);
            nodes.push_back(tree_->build(id, *board));
        }
    }
}

void GuiLayer::placeBillboards(const std::vector<engine_core::VisualBillboard>& rows, const BillboardView& view) {
    // Billboards move with the camera, so they are placed again.
    markLayoutDirty(LayoutDirt::Arrange);
    for (Placement& placement : placements_) {
        placement.placed = false;
        const engine_core::VisualBillboard* row = nullptr;
        for (const engine_core::VisualBillboard& candidate : rows) {
            if (candidate.id == placement.id) {
                row = &candidate;
                break;
            }
        }
        if (row != nullptr) {
            const BillboardPlacement where =
                PlaceBillboard(view.view, view.fovYDegrees, static_cast<float>(view.paneWidth),
                               static_cast<float>(view.paneHeight), row->anchor);
            placement.placed = where.visible;
            placement.alwaysOnTop = row->always_on_top;
            placement.x = view.paneX + where.x;
            placement.y = view.paneY + where.y;
            placement.pixelsPerUnit = where.pixelsPerUnit;
            placement.distance = where.distance;
            placement.drawn = PlacedBillboard{where.depth, row->always_on_top};
        }
        const bool shown = tree_->visible(placement.id);
        const bool userTransparent = tree_->mouseTransparent(placement.id);
        // The scene under the cursor is nearer: the mouse goes past this billboard.
        const bool behindScene = !placement.alwaysOnTop && cursorDepth_ && *cursorDepth_ < placement.drawn.depth;
        placement.node->setVisible(shown && placement.placed);
        placement.node->setMouseTransparent(!placement.placed || behindScene || userTransparent);
    }
    // The layer lays out after this in the same pass, from these placements.
    restack();
}

void GuiLayer::setCursorDepth(std::optional<float> depth) { cursorDepth_ = depth; }

void GuiLayer::restack() {
    std::vector<const Placement*> sorted;
    sorted.reserve(placements_.size());
    for (const Placement& placement : placements_) {
        sorted.push_back(&placement);
    }
    // Depth tested first, then on top; each far to near, ties by id so the order holds still.
    std::sort(sorted.begin(), sorted.end(), [](const Placement* a, const Placement* b) {
        if (a->alwaysOnTop != b->alwaysOnTop) {
            return !a->alwaysOnTop;
        }
        if (a->distance != b->distance) {
            return a->distance > b->distance;
        }
        return a->id < b->id;
    });
    order_.clear();
    order_.reserve(sorted.size() + screens_.size());
    for (const Placement* placement : sorted) {
        order_.push_back(placement->node);
    }
    for (const auto& screen : screens_) {
        order_.push_back(screen);
    }
    // A reorder alone leaves the list be: visitChildren paints and picks in order_.
    std::vector<jadefx::Node*> members;
    members.reserve(order_.size());
    for (const auto& node : order_) {
        members.push_back(node.get());
    }
    std::sort(members.begin(), members.end());
    if (members != members_) {
        // Only the nodes that leave or arrive are taken out or put in, so the
        // rest keep their focus and presses.
        getChildren().removeIf([&](const std::shared_ptr<jadefx::Node>& child) {
            return !std::binary_search(members.begin(), members.end(), child.get());
        });
        for (const auto& node : order_) {
            if (!std::binary_search(members_.begin(), members_.end(), node.get())) {
                getChildren().add(node);
            }
        }
        members_ = std::move(members);
    }
}

std::vector<jadefx::Node*> GuiLayer::paintOrder() const {
    std::vector<jadefx::Node*> order;
    order.reserve(order_.size());
    for (const auto& node : order_) {
        order.push_back(node.get());
    }
    return order;
}

void GuiLayer::visitChildren(const std::function<void(jadefx::Node*)>& visitor) {
    std::size_t visited = 0;
    for (const auto& node : order_) {
        if (node->getParent() == this) {
            visitor(node.get());
            ++visited;
        }
    }
    if (visited == getChildren().size()) {
        return;
    }
    for (const auto& child : getChildren().items()) {
        if (!child) {
            continue;
        }
        const bool ordered = std::find(order_.begin(), order_.end(), child) != order_.end();
        if (!ordered) {
            visitor(child.get());
        }
    }
}

void GuiLayer::renderChildren(jadefx::UiRenderer& renderer, float opacity) {
    std::vector<jadefx::Node*> children;
    visitChildren([&](jadefx::Node* child) { children.push_back(child); });
    jadefx::Painter painter(renderer);
    for (jadefx::Node* child : children) {
        const PlacedBillboard* board = placedFor(child);
        const bool hide = board != nullptr && !board->alwaysOnTop && sceneDepth_.texture != 0;
        if (hide) {
            painter.setOccluder(sceneDepth_.texture, sceneDepth_.x, sceneDepth_.y, sceneDepth_.width,
                                sceneDepth_.height, board->depth);
        }
        child->render(renderer, opacity);
        if (hide) {
            painter.clearOccluder();
        }
    }
}

const GuiLayer::PlacedBillboard* GuiLayer::placedFor(const jadefx::Node* node) const {
    for (const Placement& placement : placements_) {
        if (placement.node.get() == node && placement.placed) {
            return &placement.drawn;
        }
    }
    return nullptr;
}

void GuiLayer::layoutChildren() {
    for (const auto& screen : screens_) {
        screen->performLayout(contentLeft(), contentTop(), contentWidth(), contentHeight());
    }
    // A billboard's available size is one world unit at its distance, so a
    // percentage on it is world units; its content and Size work as anywhere.
    // performLayout takes a place relative to this layer, and placements are
    // in window points.
    const double left = getAbsoluteX();
    const double top = getAbsoluteY();
    for (const Placement& placement : placements_) {
        if (!placement.placed) {
            continue;
        }
        const double unit = std::min(placement.pixelsPerUnit, 1.0e6);
        const double width = placement.node->measuredWidth(unit);
        const double height = placement.node->measuredHeight(width, unit);
        placement.node->performLayout(placement.x - left - width / 2.0, placement.y - top - height / 2.0, width,
                                      height);
    }
}

}  // namespace runner
