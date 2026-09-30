#pragma once

#include "AssetBrowser.hpp"
#include "IdeExplorer.hpp"
#include "IdePane.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ide {

// What the Assets pane needs from the studio: the explorer's actions, and the
// view it last showed, to keep in preferences.json.
struct AssetsHost {
    ExplorerHost actions;
    std::function<std::string()> saved_view;
    std::function<void(const std::string&)> save_view;
};

// A Finder-like browser of the Assets tree. The toolbar holds back and forward,
// a crumb per level from Assets down, the Icons, List, and Columns toggle, and a
// search field. The sidebar lists the five categories. The center shows the
// folder in the chosen view, and the status line counts its items. A click
// selects through the world's selection, which the explorers and Properties share.
class IdeAssets : public IdePane {
public:
    IdeAssets(engine_core::DataModel& world, AssetsHost host);

    AssetBrowser& browser() { return browser_; }
    AssetView view() const { return view_; }
    // Shows view, checks its toggle, and saves it through the host.
    void setView(AssetView view);
    // Opens id when it can be opened; for tests and the path bar.
    bool openFolder(engine_core::InstanceId id);
    // The widget showing id in the current view, or null. For tests.
    jadefx::Node* itemNode(engine_core::InstanceId id) const;

protected:
    void layoutChildren() override;
    void handleKey(jadefx::KeyEvent& event) override;

private:
    // Rebuilds the crumbs, the sidebar's mark, and the center from the browser.
    // Callers hold the world's read lock.
    void rebuild();
    void rebuild_crumbs();
    void rebuild_icons(const std::vector<AssetRow>& rows);
    // Gives node the item's class and handlers, and remembers it for id.
    void add_item(const std::shared_ptr<jadefx::Node>& node, const AssetRow& row);
    // Marks the selected items, and says what is selected in the status line.
    void show_selection();
    void clicked(const AssetRow& row, const jadefx::MouseEvent& event);
    void place_clear();

    engine_core::DataModel& world_;
    AssetsHost host_;
    AssetBrowser browser_;
    AssetView view_ = AssetView::Icons;

    std::shared_ptr<jadefx::BorderPane> column_;
    std::shared_ptr<jadefx::Button> back_;
    std::shared_ptr<jadefx::Button> forward_;
    std::shared_ptr<jadefx::HBox> crumbs_;
    jadefx::ToggleGroup view_group_;
    std::shared_ptr<jadefx::ToggleButton> view_buttons_[3];
    std::shared_ptr<jadefx::TextField> search_field_;
    std::shared_ptr<jadefx::Label> search_clear_;
    std::shared_ptr<jadefx::VBox> sidebar_;
    std::unordered_map<engine_core::InstanceId, std::shared_ptr<jadefx::Node>> sidebar_rows_;
    std::shared_ptr<jadefx::ScrollPane> scroll_;
    std::shared_ptr<jadefx::Label> status_;

    // The item widgets of the current view, and their ids in the order shown.
    std::unordered_map<engine_core::InstanceId, std::shared_ptr<jadefx::Node>> items_;
    std::vector<AssetRow> rows_;

    // What the widgets were last built from.
    bool built_ = false;
    AssetView built_view_ = AssetView::Icons;
    std::uint64_t selection_seen_ = ~std::uint64_t{0};
    std::vector<engine_core::InstanceId> selected_;
    // The last item clicked, where Shift+click extends from.
    engine_core::InstanceId anchor_ = 0;
};

}  // namespace ide
