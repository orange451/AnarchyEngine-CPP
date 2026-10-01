#pragma once

#include "AssetBrowser.hpp"
#include "ChangeFlag.hpp"
#include "IdeExplorer.hpp"
#include "IdePane.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ide {

// What the Assets pane needs from the studio: the explorer's actions, and the
// view it last showed, to keep in preferences.json.
struct AssetsHost {
    ExplorerHost actions;
    std::function<std::string()> saved_view;
    std::function<void(const std::string&)> save_view;
    // Puts a GameObject in Workspace linked to the Prefab prefab, and selects it.
    std::function<void(engine_core::InstanceId prefab)> add_as_game_object;
};

// A Finder-like browser of the Assets tree. The toolbar holds back and forward,
// a crumb per level from Assets down, the Icons, List, and Columns toggle, and a
// search field. The sidebar lists the five categories. The center shows the
// folder in the chosen view, and the status line counts its items. A click
// selects through the world's selection, which the explorers and Properties share.
// A slow second click or Enter renames in place. A double-click on a Prefab runs
// Edit on it. Right-clicks offer Rename, Cut, Paste, and Delete on items, a
// Prefab's also Edit and Add as GameObject, and New Folder, New <kind>, and
// Paste on empty space.
// Items drag onto Folders, categories, and crumbs; a refused move is a notice.
class IdeAssets : public IdePane {
public:
    IdeAssets(engine_core::DataModel& world, AssetsHost host);
    ~IdeAssets() override;

    AssetBrowser& browser() { return browser_; }
    AssetView view() const { return view_; }
    // Shows view, checks its toggle, and saves it through the host.
    void setView(AssetView view);
    // Opens id when it can be opened; for tests and the path bar.
    bool openFolder(engine_core::InstanceId id);
    // The widget showing id in the current view, or null. For tests.
    jadefx::Node* itemNode(engine_core::InstanceId id) const;
    // Starts renaming id in place. For tests and the slow second click.
    void beginRename(engine_core::InstanceId id);
    // The field a search is typed in. A non-empty search lists the matches under the folder.
    jadefx::TextField& searchField() const { return *search_field_; }
    // Moves ids into target, or says why the first refused one cannot go. What a drop does.
    bool dropInto(const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId target);

protected:
    void layoutChildren() override;
    void handleKey(jadefx::KeyEvent& event) override;

private:
    // Rebuilds the crumbs, the sidebar's mark, and the center from the browser.
    // Callers hold the world's read lock.
    void rebuild();
    void rebuild_crumbs();
    void rebuild_icons(const std::vector<AssetRow>& rows);
    void rebuild_list(const std::vector<AssetRow>& rows);
    void rebuild_columns(const std::vector<std::pair<engine_core::InstanceId, std::string>>& crumbs);
    // The Columns view's last column: a single selected asset's icon, name,
    // class, and saved properties. Callers hold the world's read lock.
    void rebuild_preview();
    // The image a Texture's Path names under the resources folder, decoded
    // once per version of the file, or null when there is none to show.
    std::shared_ptr<jadefx::Image> texture_image(const std::string& path);
    // A size box holding row's icon: a Texture's file, fit and centered, or the class's icon.
    std::shared_ptr<jadefx::Node> asset_icon(const AssetRow& row, double size);
    // A flat list of the search's matches, with where each is from the folder.
    void rebuild_search(const std::vector<AssetRow>& rows);
    // The sidebar and scrolling the view has.
    void fit_view();
    bool searching() const { return !browser_.search().empty(); }
    // Folders, sidebar categories, and crumbs take dragged instances.
    void accept_drops(jadefx::Node& node, engine_core::InstanceId target);
    void show_item_menu(const AssetRow& row, double x, double y);
    void show_empty_menu(double x, double y);
    void new_item(const std::string& class_name);
    // The pending insert finished: select what it made and rename it, or say why nothing was made.
    // Callers hold the world's read lock.
    void finish_insert();
    // The selected instances this view shows.
    std::vector<engine_core::InstanceId> shown_selection() const;
    // name is id's Name, read by the caller under the world's lock.
    void begin_rename(engine_core::InstanceId id, const std::string& name);
    void finish_rename(bool apply);
    // Lays the field over the renamed item's name, or drops the rename when the item is gone or the field lost focus.
    void place_rename();
    // Starts a slow click's rename once no double-click can follow it.
    void poll_clicks();
    double now() const;
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
    // The rows Shift+click ranges over: the folder's, or in Columns its last column's.
    std::vector<AssetRow> rows_;
    // How many items the folder shown holds, for the status line.
    std::size_t count_ = 0;
    std::shared_ptr<jadefx::VBox> preview_;
    // The files texture_image decoded: each one's time then, what it gave, and
    // whether the last rebuild used it. A rebuild drops the ones it did not use.
    struct TextureImage {
        std::filesystem::file_time_type stamp{};
        std::shared_ptr<jadefx::Image> image;
        bool used = false;
    };
    std::unordered_map<std::string, TextureImage> texture_images_;

    // What the widgets were last built from. dirty_ asks for a rebuild, as a sort or a disclosure does.
    bool built_ = false;
    bool dirty_ = false;
    bool scroll_right_ = false;
    // Set by a disclosure click, so the row it bubbles to next does not select.
    bool disclosed_ = false;

    std::shared_ptr<jadefx::Menu> menu_;
    std::shared_ptr<InsertResult> pending_insert_;
    // Hidden until a rename. Kept as a child so it is styled every frame.
    std::shared_ptr<jadefx::TextField> rename_field_;
    bool renaming_ = false;
    engine_core::InstanceId rename_id_ = 0;
    std::string rename_from_;
    // The last plain click on an item, and a slow second click waiting out the double-click window.
    engine_core::InstanceId click_id_ = 0;
    double click_at_ = 0;
    bool slow_pending_ = false;
    engine_core::InstanceId slow_id_ = 0;
    double slow_at_ = 0;
    AssetView built_view_ = AssetView::Icons;
    std::uint64_t selection_seen_ = ~std::uint64_t{0};
    std::vector<engine_core::InstanceId> selected_;
    // Set when a property of a shown instance changes, as a Path edit or its
    // undo does, which moves no tree revision; the next frame rebuilds.
    ChangeFlag edited_;
    std::uint64_t watch_ = 0;
    std::vector<engine_core::InstanceId> watched_;
    // The last item clicked, where Shift+click extends from.
    engine_core::InstanceId anchor_ = 0;
};

}  // namespace ide
