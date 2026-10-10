#include "SavedLayout.hpp"

#include "IdeDock.hpp"
#include "IdePane.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace ide {
namespace {

using engine_core::JsonValue;

// A file written by hand could nest without end.
constexpr int kMaxDepth = 32;

// Shares that are all zero, or missing, split the space evenly.
std::vector<double> Normalized(std::vector<double> shares) {
    double total = 0;
    for (double share : shares) {
        total += share;
    }
    for (double& share : shares) {
        share = total > 1e-9 ? share / total : 1.0 / static_cast<double>(shares.size());
    }
    return shares;
}

JsonValue SaveNode(const jadefx::Node& node, const LayoutHost& host) {
    if (const auto* dock = dynamic_cast<const IdeDock*>(&node)) {
        if (dock->tabs() == nullptr) {
            return {};
        }
        JsonValue tabs = JsonValue::array();
        std::string selected;
        for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
            const auto* page = tab ? dynamic_cast<const IdePane*>(tab->getContent()) : nullptr;
            const std::string name = page != nullptr && host.name_of ? host.name_of(*page) : std::string();
            if (name.empty()) {
                continue;
            }
            tabs.items().push_back(JsonValue::string(name));
            if (tab->isSelected()) {
                selected = name;
            }
        }
        if (tabs.items().empty()) {
            return {};
        }
        JsonValue out = JsonValue::object();
        out.set("tabs", std::move(tabs));
        if (!selected.empty()) {
            out.set("selected", JsonValue::string(selected));
        }
        return out;
    }
    const auto* split = dynamic_cast<const jadefx::SplitPane*>(&node);
    if (split == nullptr) {
        return {};
    }
    const std::vector<std::shared_ptr<jadefx::Node>>& items = split->getItems().items();
    const std::vector<double> dividers = split->getDividerPositions();
    JsonValue saved = JsonValue::array();
    std::vector<double> shares;
    double edge = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        // Each item runs from the divider before it to the one after.
        const double next = i < dividers.size() ? std::clamp(dividers[i], edge, 1.0) : 1.0;
        const double share = next - edge;
        edge = next;
        if (!items[i]) {
            continue;
        }
        JsonValue child = SaveNode(*items[i], host);
        if (child.is_null()) {
            continue;
        }
        child.erase("fixed");
        if (!jadefx::SplitPane::isResizableWithParent(*items[i])) {
            child.set("fixed", JsonValue::boolean(true));
        }
        saved.items().push_back(std::move(child));
        shares.push_back(share);
    }
    if (saved.items().empty()) {
        return {};
    }
    if (saved.items().size() == 1) {
        // It takes this split's place, and the parent decides whether that is fixed.
        JsonValue only = std::move(saved.items()[0]);
        only.erase("fixed");
        return only;
    }
    JsonValue sizes = JsonValue::array();
    for (double share : Normalized(std::move(shares))) {
        // Four places is under a point on any screen, and keeps the file readable.
        sizes.items().push_back(JsonValue::number(std::round(share * 10000.0) / 10000.0));
    }
    JsonValue out = JsonValue::object();
    out.set("split", JsonValue::string(split->getOrientation() == jadefx::Orientation::Vertical ? "vertical"
                                                                                                : "horizontal"));
    out.set("items", std::move(saved));
    out.set("sizes", std::move(sizes));
    return out;
}

std::shared_ptr<jadefx::Node> LoadNode(const JsonValue& value, const LayoutHost& host, int depth) {
    if (!value.is_object() || depth > kMaxDepth) {
        return nullptr;
    }
    if (const JsonValue* tabs = value.find("tabs")) {
        if (!tabs->is_array() || !host.dock_page) {
            return nullptr;
        }
        auto dock = jadefx::make<IdeDock>();
        for (const JsonValue& name : tabs->items()) {
            if (name.is_string()) {
                host.dock_page(*dock, name.as_string());
            }
        }
        if (dock->empty()) {
            return nullptr;
        }
        const JsonValue* selected = value.find("selected");
        if (selected != nullptr && selected->is_string() && host.name_of) {
            for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
                const auto* page = tab ? dynamic_cast<const IdePane*>(tab->getContent()) : nullptr;
                if (page != nullptr && host.name_of(*page) == selected->as_string()) {
                    dock->select(page);
                }
            }
        }
        return dock;
    }
    const JsonValue* kind = value.find("split");
    const JsonValue* items = value.find("items");
    if (kind == nullptr || !kind->is_string() || items == nullptr || !items->is_array()) {
        return nullptr;
    }
    const JsonValue* sizes = value.find("sizes");
    auto split = jadefx::make<jadefx::SplitPane>();
    split->setOrientation(kind->as_string() == "vertical" ? jadefx::Orientation::Vertical
                                                          : jadefx::Orientation::Horizontal);
    std::vector<double> shares;
    for (std::size_t i = 0; i < items->items().size(); ++i) {
        const JsonValue& item = items->items()[i];
        std::shared_ptr<jadefx::Node> child = LoadNode(item, host, depth + 1);
        if (!child) {
            continue;
        }
        const JsonValue* fixed = item.find("fixed");
        jadefx::SplitPane::setResizableWithParent(*child, !(fixed != nullptr && fixed->as_bool()));
        split->getItems().add(child);
        double share = 0;
        if (sizes != nullptr && sizes->is_array() && i < sizes->items().size()) {
            const double read = sizes->items()[i].as_number();
            share = std::isfinite(read) ? std::max(0.0, read) : 0.0;
        }
        shares.push_back(share);
    }
    if (split->getItems().empty()) {
        return nullptr;
    }
    if (split->getItems().size() == 1) {
        return split->getItems()[0];
    }
    double edge = 0;
    const std::vector<double> normalized = Normalized(std::move(shares));
    std::vector<double> dividers;
    for (std::size_t i = 0; i + 1 < normalized.size(); ++i) {
        edge += normalized[i];
        split->setDividerPosition(static_cast<int>(i), edge);
        dividers.push_back(edge);
    }
    if (host.loaded_split) {
        host.loaded_split(split, dividers);
    }
    return split;
}

}  // namespace

JsonValue save_window_place(bool maximized, const WindowPlace& now, const std::optional<WindowPlace>& normal) {
    JsonValue window = JsonValue::object();
    const WindowPlace& kept = maximized && normal ? *normal : now;
    window.set("x", JsonValue::number(kept.x));
    window.set("y", JsonValue::number(kept.y));
    if (!maximized || normal) {
        window.set("width", JsonValue::number(kept.width));
        window.set("height", JsonValue::number(kept.height));
    }
    window.set("maximized", JsonValue::boolean(maximized));
    return window;
}

JsonValue save_layout_node(const jadefx::Node& node, const LayoutHost& host) {
    JsonValue saved = SaveNode(node, host);
    // The top has no split around it.
    saved.erase("fixed");
    return saved;
}

std::shared_ptr<jadefx::Node> load_layout_node(const JsonValue& value, const LayoutHost& host) {
    return LoadNode(value, host, 0);
}

void layout_tab_names(const JsonValue& value, std::vector<std::string>& out) {
    if (const JsonValue* tabs = value.find("tabs"); tabs != nullptr && tabs->is_array()) {
        for (const JsonValue& name : tabs->items()) {
            if (name.is_string()) {
                out.push_back(name.as_string());
            }
        }
    }
    if (const JsonValue* items = value.find("items"); items != nullptr && items->is_array()) {
        for (const JsonValue& item : items->items()) {
            layout_tab_names(item, out);
        }
    }
}

}  // namespace ide
