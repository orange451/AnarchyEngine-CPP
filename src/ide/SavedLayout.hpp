#pragma once

#include "PropertyBag.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jadefx {
class Node;
class SplitPane;
}

namespace ide {

class IdeDock;
class IdePane;

// How the pages in the studio's docks are named in layout.json, and docked again from it.
struct LayoutHost {
    // The name a page is saved under. Empty leaves its tab out of the file.
    std::function<std::string(const IdePane& page)> name_of;
    // Docks the page saved under name in dock. False when there is none to
    // dock, such as a name this studio does not know or one already docked.
    std::function<bool(IdeDock& dock, const std::string& name)> dock_page;
    // Told each split a load makes and the divider positions it set, its saved shares. Optional.
    std::function<void(const std::shared_ptr<jadefx::SplitPane>& split, const std::vector<double>& dividers)>
        loaded_split;
};

// The main window's place and size, in screen points.
struct WindowPlace {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

// The main window as layout.json keeps it: {"x", "y", "width", "height",
// "maximized"}. Maximized, it keeps normal, the place and size it last had
// when it was not, so un-maximizing after a restart goes back to them; with
// none, only now's place, which picks the display it maximizes on.
engine_core::JsonValue save_window_place(bool maximized, const WindowPlace& now,
                                         const std::optional<WindowPlace>& normal);

// A dock is {"tabs": [names], "selected": name}. A split is {"split":
// "horizontal" or "vertical", "items": [...], "sizes": [...]}, where sizes are
// the items' shares of it. "fixed": true marks an item that keeps its size
// when the split around it resizes.
// Null when nothing under node is saved. A split left with one item saves as that item.
engine_core::JsonValue save_layout_node(const jadefx::Node& node, const LayoutHost& host);

// Builds new docks and splits from what save_layout_node wrote, docking the
// named pages as it goes. A dock none of whose tabs could be docked is left
// out, and a split left with one item becomes that item. Null when nothing could be built.
std::shared_ptr<jadefx::Node> load_layout_node(const engine_core::JsonValue& value, const LayoutHost& host);

// Every tab name in what save_layout_node wrote, in order.
void layout_tab_names(const engine_core::JsonValue& value, std::vector<std::string>& out);

}  // namespace ide
