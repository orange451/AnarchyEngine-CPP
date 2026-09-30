#include "IdeAssets.hpp"
#include "LockWaits.hpp"

#include "DataModelLock.hpp"
#include "FindBar.hpp"
#include "IdeIcons.hpp"
#include "LuaApi.hpp"
#include "PropertySheet.hpp"
#include "SelectionService.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace ide {
namespace {

// A tile in the Icons view, and the icon it shows.
constexpr double kTileWidth = 72;
constexpr double kTileIcon = 40;
constexpr double kSidebarWidth = 150;
// The × in the search field's right end, and its gap from the field's edge.
constexpr double kClearSize = 18;
constexpr double kClearInset = 3;

// Rows and selection take the explorer's colors: JadeFX's row hover and
// selection, and the explorer's buttons.
constexpr const char* kAssetsRules = R"CSS(
.assets-pane {
    background-color: var(--ide-panel-color);
}
.assets-toolbar {
    padding: 5px 8px;
    spacing: 4px;
    border-width: 0 0 1px 0;
    border-style: solid;
    border-color: var(--ide-search-divider-color);
}
.assets-nav, .assets-crumb, .assets-view-toggle {
    background-color: rgba(0, 0, 0, 0);
    border-width: 1px;
    border-style: solid;
    border-color: rgba(0, 0, 0, 0);
    border-radius: 3px;
    color: var(--ide-text-color);
}
.assets-nav {
    font-size: 16px;
    padding: 0 7px;
}
.assets-crumb {
    padding: 1px 4px;
}
.assets-crumb.current {
    font-weight: bold;
}
.assets-view-toggle {
    padding: 3px 5px;
}
.assets-view-toggle image-view {
    image-color: var(--ide-find-button-text-color);
}
.assets-nav:hover, .assets-crumb:hover, .assets-view-toggle:hover {
    background-color: var(--ide-find-button-hover-color);
}
.assets-view-toggle:selected {
    background-color: var(--ide-find-button-checked-color);
    border-color: var(--ide-find-button-checked-border-color);
}
.assets-nav:disabled {
    background-color: rgba(0, 0, 0, 0);
    opacity: 0.35;
}
.assets-crumb-gap {
    color: var(--ide-muted-text-color);
}
.assets-search {
    padding: 3px 26px 3px 6px;
}
.assets-search-clear {
    color: var(--ide-explorer-button-color);
}
.assets-search-clear:disabled {
    color: var(--ide-explorer-button-disabled-color);
}
.assets-sidebar {
    background-color: var(--ide-ribbon-color);
    border-width: 0 1px 0 0;
    border-style: solid;
    border-color: var(--ide-search-divider-color);
    padding: 8px 6px;
    spacing: 1px;
}
.assets-sidebar-heading {
    color: var(--ide-muted-text-color);
    font-size: 11px;
    font-weight: bold;
    padding: 0 6px 4px 6px;
}
.assets-sidebar-row {
    padding: 3px 6px;
    spacing: 6px;
    border-radius: 4px;
    color: var(--ide-text-color);
}
.assets-sidebar-row:hover {
    background-color: var(--row-hover-color);
}
.assets-sidebar-row.selected {
    background-color: var(--selection-color);
}
.assets-icons {
    padding: 10px;
}
.assets-item {
    border-radius: 4px;
    color: var(--ide-text-color);
}
.assets-item:hover {
    background-color: var(--row-hover-color);
}
.assets-item.selected {
    background-color: var(--selection-color);
}
.assets-item.selected:hover {
    background-color: var(--selection-hover-color);
}
.assets-tile {
    padding: 6px 3px 4px 3px;
    spacing: 4px;
}
.assets-tile .assets-name {
    font-size: 12px;
}
.assets-list-header {
    padding: 0 4px;
    border-width: 0 0 1px 0;
    border-style: solid;
    border-color: var(--ide-search-divider-color);
}
.assets-sort {
    background-color: rgba(0, 0, 0, 0);
    border-width: 1px;
    border-style: solid;
    border-color: rgba(0, 0, 0, 0);
    border-radius: 0;
    color: var(--ide-muted-text-color);
    font-size: 12px;
    padding: 3px 0;
}
.assets-sort:hover {
    background-color: var(--ide-find-button-hover-color);
}
.assets-sort.active {
    color: var(--ide-text-color);
    font-weight: bold;
}
.assets-list {
    padding: 2px 4px;
}
.assets-list-row {
    padding: 0 4px;
}
.assets-cell {
    font-size: 13px;
}
.assets-cell.muted, .assets-disclosure, .assets-opens {
    color: var(--ide-muted-text-color);
}
.assets-disclosure image-view, .assets-sort image-view {
    image-color: currentColor;
}
.assets-column {
    border-width: 0 1px 0 0;
    border-style: solid;
    border-color: var(--ide-search-divider-color);
}
.assets-column-list {
    padding: 4px;
}
.assets-column-row {
    padding: 0 6px;
}
.assets-on-path {
    background-color: var(--row-hover-color);
}
.assets-preview {
    padding: 14px 12px;
    spacing: 3px;
}
.assets-preview-name {
    font-size: 14px;
    font-weight: bold;
}
.assets-preview-class {
    color: var(--ide-muted-text-color);
    font-size: 12px;
    padding: 0 0 10px 0;
}
.assets-preview-key {
    color: var(--ide-muted-text-color);
    font-size: 11px;
    padding: 6px 0 0 0;
}
.assets-preview-value {
    font-size: 13px;
}
.assets-empty {
    color: var(--ide-muted-text-color);
    padding: 16px;
}
.assets-status {
    color: var(--ide-search-status-color);
    font-size: 12px;
    padding: 3px 10px;
    border-width: 1px 0 0 0;
    border-style: solid;
    border-color: var(--ide-search-divider-color);
}
)CSS";

// The toggle's buttons, in AssetView's order. vbox.png draws stacked rows, a
// list; hbox.png draws bars side by side, columns.
struct ViewButton {
    AssetView view;
    const char* icon;
    const char* tip;
};
constexpr ViewButton kViewButtons[] = {
    {AssetView::Icons, "Grid.png", "Icons"},
    {AssetView::List, "vbox.png", "List"},
    {AssetView::Columns, "hbox.png", "Columns"},
};

std::shared_ptr<jadefx::Label> text_label(const std::string& text, const char* style_class) {
    auto label = jadefx::make<jadefx::Label>(text);
    if (style_class != nullptr) {
        label->getClassList().add(style_class);
    }
    label->setMouseTransparent(true);
    return label;
}

// The class's icon at size, not taking clicks, or an empty box of that size.
std::shared_ptr<jadefx::Node> sized_icon(const std::string& class_name, double size) {
    if (std::shared_ptr<jadefx::ImageView> icon = icon_view(class_name)) {
        icon->setPrefSize(size, size);
        icon->setMinSize(size, size);
        icon->setMaxSize(size, size);
        icon->setMouseTransparent(true);
        return icon;
    }
    auto box = jadefx::make<jadefx::Pane>();
    box->setPrefSize(size, size);
    box->setMouseTransparent(true);
    return box;
}

// Takes the room left in a line, which pushes what follows it to the right edge.
std::shared_ptr<jadefx::Pane> spacer() {
    auto pane = jadefx::make<jadefx::Pane>();
    pane->setStyle("width: 100%;");
    pane->setMouseTransparent(true);
    return pane;
}

// A row whose cells take fractions of its width, as the List view's columns.
class FractionRow : public jadefx::HBox {
public:
    explicit FractionRow(std::vector<double> fractions) : fractions_(std::move(fractions)) {
        setAlignment(jadefx::Pos::CenterLeft);
        setStyle("width: 100%;");
    }

protected:
    double preferredContentWidth(double innerAvailable) const override {
        return innerAvailable > 0 ? innerAvailable : HBox::preferredContentWidth(innerAvailable);
    }

    void layoutChildren() override {
        const std::vector<std::shared_ptr<jadefx::Node>>& children = getChildren().items();
        const double width = contentWidth();
        const double height = contentHeight();
        double x = contentLeft();
        for (std::size_t index = 0; index < children.size(); ++index) {
            const double cell = index < fractions_.size() ? width * fractions_[index] : 0;
            if (jadefx::Node* child = children[index].get()) {
                const double child_height = std::min(child->measuredHeight(cell, height), height);
                child->performLayout(x, contentTop() + (height - child_height) * 0.5, cell, child_height);
            }
            x += cell;
        }
    }

private:
    std::vector<double> fractions_;
};

// A line of cells at their own widths, except one that takes what is left, so
// a long name ends in an ellipsis instead of pushing the rest out.
class StretchRow : public jadefx::HBox {
public:
    explicit StretchRow(std::size_t stretch) : stretch_(stretch) {
        setAlignment(jadefx::Pos::CenterLeft);
        setSpacing(5);
    }

protected:
    double preferredContentWidth(double innerAvailable) const override {
        const double wanted = HBox::preferredContentWidth(innerAvailable);
        return innerAvailable > 0 ? std::min(wanted, innerAvailable) : wanted;
    }

    void layoutChildren() override {
        const std::vector<std::shared_ptr<jadefx::Node>>& children = getChildren().items();
        const double gap = getSpacing();
        const double height = contentHeight();
        std::vector<double> widths(children.size(), 0);
        double used = 0;
        for (std::size_t index = 0; index < children.size(); ++index) {
            if (index != stretch_ && children[index]) {
                widths[index] = children[index]->measuredWidth(contentWidth());
                used += widths[index];
            }
            if (index > 0) {
                used += gap;
            }
        }
        if (stretch_ < children.size()) {
            widths[stretch_] = std::max(0.0, contentWidth() - used);
        }
        double x = contentLeft();
        for (std::size_t index = 0; index < children.size(); ++index) {
            if (jadefx::Node* child = children[index].get()) {
                const double child_height = std::min(child->measuredHeight(widths[index], height), height);
                child->performLayout(x, contentTop() + (height - child_height) * 0.5, widths[index], child_height);
            }
            x += widths[index] + gap;
        }
    }

private:
    std::size_t stretch_;
};

// The List view's columns: Name, Kind, and Path.
const std::vector<double> kListColumns = {0.45, 0.20, 0.35};
constexpr double kRowHeight = 22;
constexpr double kIndent = 16;
constexpr double kDisclosureWidth = 16;
constexpr double kColumnWidth = 180;
constexpr double kPreviewWidth = 220;
constexpr double kPreviewIcon = 64;

void fix_width(jadefx::Node& node, double width) {
    node.setPrefWidth(width);
    node.setMinSize(width, 0);
    node.setMaxSize(width, 100000);
}

void set_class(jadefx::Node& node, const char* name, bool on) {
    jadefx::ObservableList<std::string>& classes = node.getClassList();
    const bool has = std::find(classes.begin(), classes.end(), std::string(name)) != classes.end();
    if (on && !has) {
        classes.add(name);
    } else if (!on && has) {
        classes.removeIf([name](const std::string& item) { return item == name; });
    }
}

}  // namespace

IdeAssets::IdeAssets(engine_core::DataModel& world, AssetsHost host)
    : IdePane("Assets", true), world_(world), host_(std::move(host)), browser_(world) {
    setPrefWidth(9999999);
    setMinSize(240, 120);
    getClassList().add("assets-pane");
    setStylesheet(std::string(kFindStylesheet) + kAssetsRules);
    if (host_.saved_view) {
        AssetView saved = AssetView::Icons;
        if (asset_view_from(host_.saved_view(), saved)) {
            view_ = saved;
        }
    }

    back_ = jadefx::make<jadefx::Button>("‹");
    forward_ = jadefx::make<jadefx::Button>("›");
    for (const auto& button : {back_, forward_}) {
        button->getClassList().add("assets-nav");
    }
    back_->getClassList().add("assets-back");
    forward_->getClassList().add("assets-forward");
    jadefx::Tooltip::install(back_.get(), jadefx::make<jadefx::Tooltip>("Back"));
    jadefx::Tooltip::install(forward_.get(), jadefx::make<jadefx::Tooltip>("Forward"));
    back_->setOnAction([this](jadefx::ActionEvent&) {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
        if (lock.owns()) {
            browser_.back();
        }
    });
    forward_->setOnAction([this](jadefx::ActionEvent&) {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
        if (lock.owns()) {
            browser_.forward();
        }
    });

    crumbs_ = jadefx::make<jadefx::HBox>();
    crumbs_->getClassList().add("assets-crumbs");
    crumbs_->setAlignment(jadefx::Pos::CenterLeft);
    crumbs_->setSpacing(1);

    auto toggles = jadefx::make<jadefx::HBox>();
    toggles->setSpacing(2);
    toggles->setAlignment(jadefx::Pos::CenterLeft);
    for (std::size_t index = 0; index < std::size(kViewButtons); ++index) {
        const ViewButton& entry = kViewButtons[index];
        auto button = jadefx::make<jadefx::ToggleButton>();
        button->getClassList().add("assets-view-toggle");
        button->getClassList().add(std::string("assets-view-") + asset_view_name(entry.view));
        if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(entry.icon)) {
            button->setGraphic(std::move(icon));
        } else {
            button->setText(entry.tip);
        }
        jadefx::Tooltip::install(button.get(), jadefx::make<jadefx::Tooltip>(entry.tip));
        button->setToggleGroup(&view_group_);
        const AssetView view = entry.view;
        button->setOnAction([this, view](jadefx::ActionEvent&) { setView(view); });
        toggles->getChildren().add(button);
        view_buttons_[index] = std::move(button);
    }
    view_buttons_[static_cast<std::size_t>(view_)]->setSelected(true);

    search_field_ = jadefx::make<jadefx::TextField>();
    search_field_->getClassList().add("search-field");
    search_field_->getClassList().add("assets-search");
    search_field_->setPromptText("Search");
    search_field_->setPrefWidth(170);
    search_field_->setMinSize(80, 0);

    auto toolbar = jadefx::make<jadefx::HBox>();
    toolbar->getClassList().add("assets-toolbar");
    toolbar->setAlignment(jadefx::Pos::CenterLeft);
    toolbar->setStyle("width: 100%;");
    toolbar->getChildren().add(back_);
    toolbar->getChildren().add(forward_);
    toolbar->getChildren().add(crumbs_);
    toolbar->getChildren().add(spacer());
    toolbar->getChildren().add(toggles);
    toolbar->getChildren().add(search_field_);

    sidebar_ = jadefx::make<jadefx::VBox>();
    sidebar_->getClassList().add("assets-sidebar");
    sidebar_->setPrefWidth(kSidebarWidth);
    sidebar_->setMinSize(kSidebarWidth, 0);
    sidebar_->setMaxSize(kSidebarWidth, 100000);
    sidebar_->getChildren().add(text_label("Assets", "assets-sidebar-heading"));

    scroll_ = jadefx::make<jadefx::ScrollPane>();
    scroll_->getClassList().add("assets-center");

    status_ = text_label("", "assets-status");
    status_->setStyle("width: 100%;");
    status_->setAlignment(jadefx::Pos::CenterLeft);

    column_ = jadefx::make<jadefx::BorderPane>();
    Fill(*column_);
    column_->setTop(toolbar);
    column_->setCenter(scroll_);
    column_->setBottom(status_);
    getChildren().add(column_);

    // After the column, so it draws over the field and is hit first.
    search_clear_ = jadefx::make<jadefx::Label>("×");
    search_clear_->getClassList().add("assets-search-clear");
    search_clear_->setAlignment(jadefx::Pos::Center);
    search_clear_->setFont(jadefx::Font("Open Sans", 16.f));
    search_clear_->setDisable(true);
    getChildren().add(search_clear_);
    fit_view();
}

void IdeAssets::fit_view() {
    const bool columns = view_ == AssetView::Columns;
    // Columns has no sidebar: its first column lists the categories.
    column_->setLeft(columns ? nullptr : sidebar_);
    // Columns run off to the right and each scrolls on its own; the others wrap
    // or stretch to the width and scroll down.
    scroll_->setFitToWidth(!columns);
    scroll_->setFitToHeight(columns);
    scroll_->setHbarPolicy(columns ? jadefx::ScrollBarPolicy::AsNeeded : jadefx::ScrollBarPolicy::Never);
    scroll_->setVbarPolicy(columns ? jadefx::ScrollBarPolicy::Never : jadefx::ScrollBarPolicy::AsNeeded);
}

void IdeAssets::setView(AssetView view) {
    view_ = view;
    jadefx::ToggleButton& button = *view_buttons_[static_cast<std::size_t>(view)];
    // A click on the checked button turns it off; the view stays, so it stays checked.
    if (!button.isSelected()) {
        button.setSelected(true);
    }
    fit_view();
    if (host_.save_view) {
        host_.save_view(asset_view_name(view));
    }
}

bool IdeAssets::openFolder(engine_core::InstanceId id) {
    engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
    if (!lock.owns()) {
        if (host_.actions.notice) {
            host_.actions.notice("The place is busy, so the folder did not open. Try again.");
        }
        return false;
    }
    return browser_.open(id);
}

jadefx::Node* IdeAssets::itemNode(engine_core::InstanceId id) const {
    const auto found = items_.find(id);
    return found == items_.end() ? nullptr : found->second.get();
}

void IdeAssets::layoutChildren() {
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kFrameLockWait);
        if (lock.owns()) {
            const bool changed = browser_.refresh() || dirty_ || !built_ || built_view_ != view_;
            if (changed) {
                rebuild();
            }
            if (world_.selection().revision() != selection_seen_ || changed) {
                selected_ = world_.selection().get(selection_seen_);
                show_selection();
            }
        }
    }
    back_->setDisable(!browser_.can_back());
    forward_->setDisable(!browser_.can_forward());
    IdePane::layoutChildren();
    place_clear();
    if (scroll_right_) {
        // A deeper folder opened in Columns: show its column, at the right.
        scroll_right_ = false;
        scroll_->setHvalue(scroll_->getHmax());
    }
}

void IdeAssets::handleKey(jadefx::KeyEvent& event) { IdePane::handleKey(event); }

void IdeAssets::rebuild() {
    built_ = true;
    dirty_ = false;
    built_view_ = view_;
    if (sidebar_rows_.empty()) {
        for (const AssetRow& category : browser_.categories()) {
            auto row = jadefx::make<jadefx::HBox>();
            row->getClassList().add("assets-sidebar-row");
            row->setAlignment(jadefx::Pos::CenterLeft);
            row->setStyle("width: 100%;");
            row->getProperties()["asset-id"] = category.id;
            row->getChildren().add(sized_icon(category.class_name, 16));
            row->getChildren().add(text_label(category.name, "assets-sidebar-label"));
            const engine_core::InstanceId id = category.id;
            row->setOnMouseClicked([this, id](const jadefx::MouseEvent& event) {
                if (event.button == 0) {
                    openFolder(id);
                }
            });
            sidebar_->getChildren().add(row);
            sidebar_rows_[id] = std::move(row);
        }
    }
    const std::vector<std::pair<engine_core::InstanceId, std::string>> crumbs = browser_.crumbs();
    const engine_core::InstanceId category = crumbs.size() > 1 ? crumbs[1].first : 0;
    for (const auto& [id, row] : sidebar_rows_) {
        set_class(*row, "selected", id == category);
    }
    rebuild_crumbs();

    items_.clear();
    preview_.reset();
    switch (view_) {
    case AssetView::Icons:
        rows_ = browser_.children();
        count_ = rows_.size();
        rebuild_icons(rows_);
        break;
    case AssetView::List:
        rows_ = browser_.list_rows();
        count_ = static_cast<std::size_t>(
            std::count_if(rows_.begin(), rows_.end(), [](const AssetRow& row) { return row.depth == 0; }));
        rebuild_list(rows_);
        break;
    case AssetView::Columns:
        rebuild_columns(crumbs);
        break;
    }
    scroll_->applyCss();
}

void IdeAssets::rebuild_crumbs() {
    crumbs_->getChildren().clear();
    const std::vector<std::pair<engine_core::InstanceId, std::string>> crumbs = browser_.crumbs();
    for (std::size_t index = 0; index < crumbs.size(); ++index) {
        if (index > 0) {
            crumbs_->getChildren().add(text_label("›", "assets-crumb-gap"));
        }
        auto crumb = jadefx::make<jadefx::Button>(crumbs[index].second);
        crumb->getClassList().add("assets-crumb");
        if (index + 1 == crumbs.size()) {
            crumb->getClassList().add("current");
        }
        crumb->getProperties()["asset-id"] = crumbs[index].first;
        const engine_core::InstanceId id = crumbs[index].first;
        crumb->setOnAction([this, id](jadefx::ActionEvent&) { openFolder(id); });
        crumbs_->getChildren().add(crumb);
    }
    crumbs_->applyCss();
}

void IdeAssets::rebuild_icons(const std::vector<AssetRow>& rows) {
    auto flow = jadefx::make<jadefx::FlowPane>(8, 8);
    flow->getClassList().add("assets-icons");
    flow->setAlignment(jadefx::Pos::TopLeft);
    flow->setRowValignment(jadefx::VPos::Top);
    for (const AssetRow& row : rows) {
        auto tile = jadefx::make<jadefx::VBox>();
        tile->getClassList().add("assets-tile");
        tile->setAlignment(jadefx::Pos::TopCenter);
        fix_width(*tile, kTileWidth);
        tile->getChildren().add(sized_icon(row.class_name, kTileIcon));
        auto name = text_label(row.name, "assets-name");
        name->setAlignment(jadefx::Pos::Center);
        name->setMaxSize(kTileWidth - 6, 100000);
        tile->getChildren().add(name);
        jadefx::Tooltip::install(tile.get(), jadefx::make<jadefx::Tooltip>(row.name));
        add_item(tile, row);
        flow->getChildren().add(tile);
    }
    if (rows.empty()) {
        scroll_->setContent(text_label("This folder is empty.", "assets-empty"));
        return;
    }
    scroll_->setContent(flow);
}

void IdeAssets::rebuild_list(const std::vector<AssetRow>& rows) {
    auto list = jadefx::make<jadefx::VBox>();
    list->getClassList().add("assets-list");
    list->setStyle("width: 100%;");

    auto header = jadefx::make<FractionRow>(kListColumns);
    header->getClassList().add("assets-list-header");
    struct SortColumn {
        AssetSort sort;
        const char* title;
    };
    constexpr SortColumn kSorts[] = {{AssetSort::Name, "Name"}, {AssetSort::Kind, "Kind"}, {AssetSort::Path, "Path"}};
    for (const SortColumn& entry : kSorts) {
        const bool active = browser_.sort() == entry.sort;
        auto button = jadefx::make<jadefx::Button>(entry.title);
        // Open Sans has no ▲ or ▼, so the direction is the find bar's up or down chevron.
        if (active) {
            button->setGraphic(icon_graphic(browser_.descending() ? "FindNext.png" : "FindPrevious.png"));
            button->setContentDisplay(jadefx::ContentDisplay::Right);
            button->setGraphicTextGap(3);
            button->getClassList().add(browser_.descending() ? "descending" : "ascending");
        }
        button->getClassList().add("assets-sort");
        button->getClassList().add(std::string("assets-sort-") + entry.title);
        if (active) {
            button->getClassList().add("active");
        }
        button->setAlignment(jadefx::Pos::CenterLeft);
        button->setStyle("width: 100%;");
        const AssetSort sort = entry.sort;
        button->setOnAction([this, sort](jadefx::ActionEvent&) {
            // The active column again reverses it; another starts ascending.
            browser_.set_sort(sort, browser_.sort() == sort && !browser_.descending());
            dirty_ = true;
        });
        header->getChildren().add(button);
    }
    list->getChildren().add(header);

    for (const AssetRow& row : rows) {
        auto line = jadefx::make<FractionRow>(kListColumns);
        line->getClassList().add("assets-list-row");
        line->setMinSize(0, kRowHeight);
        line->setPrefHeight(kRowHeight);

        auto name = jadefx::make<StretchRow>(3);
        name->getClassList().add("assets-name-cell");
        auto indent = jadefx::make<jadefx::Pane>();
        fix_width(*indent, row.depth * kIndent);
        indent->setMouseTransparent(true);
        name->getChildren().add(indent);
        // The find bar's chevrons, since Open Sans has no ▸ or ▾.
        auto disclosure = jadefx::make<jadefx::Label>("");
        disclosure->getClassList().add("assets-disclosure");
        if (row.opens) {
            disclosure->setGraphic(icon_graphic(row.expanded ? "FindExpanded.png" : "FindCollapsed.png"));
            disclosure->setContentDisplay(jadefx::ContentDisplay::GraphicOnly);
            disclosure->getClassList().add(row.expanded ? "expanded" : "collapsed");
        }
        disclosure->setAlignment(jadefx::Pos::Center);
        fix_width(*disclosure, kDisclosureWidth);
        if (row.opens) {
            const engine_core::InstanceId id = row.id;
            const bool expanded = row.expanded;
            disclosure->setCursor(jadefx::Cursor::Pointer);
            disclosure->setOnMouseClicked([this, id, expanded](const jadefx::MouseEvent& event) {
                if (event.button == 0) {
                    browser_.set_expanded(id, !expanded);
                    disclosed_ = true;
                    dirty_ = true;
                }
            });
        } else {
            disclosure->setMouseTransparent(true);
        }
        name->getChildren().add(disclosure);
        name->getChildren().add(sized_icon(row.class_name, 16));
        name->getChildren().add(text_label(row.name, "assets-cell"));
        line->getChildren().add(name);
        auto kind = text_label(row.class_name, "assets-cell");
        kind->getClassList().add("muted");
        line->getChildren().add(kind);
        auto path = text_label(row.path, "assets-cell");
        path->getClassList().add("muted");
        line->getChildren().add(path);
        add_item(line, row);
        list->getChildren().add(line);
    }
    if (rows.empty()) {
        list->getChildren().add(text_label("This folder is empty.", "assets-empty"));
    }
    scroll_->setContent(list);
}

void IdeAssets::rebuild_columns(const std::vector<std::pair<engine_core::InstanceId, std::string>>& crumbs) {
    auto strip = jadefx::make<jadefx::HBox>();
    strip->getClassList().add("assets-columns");
    strip->setStyle("height: 100%;");
    const std::vector<std::vector<AssetRow>> columns = browser_.columns();
    for (std::size_t level = 0; level < columns.size(); ++level) {
        // The row in this column that leads to the folder shown: crumbs are
        // Assets, the category, and so on down, so column k leads to crumb k + 1.
        const engine_core::InstanceId on_path = level + 1 < crumbs.size() ? crumbs[level + 1].first : 0;
        auto list = jadefx::make<jadefx::VBox>();
        list->getClassList().add("assets-column-list");
        list->setStyle("width: 100%;");
        for (const AssetRow& row : columns[level]) {
            auto line = jadefx::make<StretchRow>(1);
            line->getClassList().add("assets-column-row");
            line->setStyle("width: 100%;");
            line->setMinSize(0, kRowHeight);
            line->setPrefHeight(kRowHeight);
            line->getChildren().add(sized_icon(row.class_name, 16));
            line->getChildren().add(text_label(row.name, "assets-cell"));
            const bool opens = level == 0 || row.opens;
            line->getChildren().add(text_label(opens ? "›" : "", "assets-opens"));
            if (row.id == on_path) {
                line->getClassList().add("assets-on-path");
            }
            if (level == 0) {
                // A category opens; it is not an asset to select.
                line->getClassList().add("assets-item");
                const engine_core::InstanceId id = row.id;
                line->setOnMouseClicked([this, id](const jadefx::MouseEvent& event) {
                    if (event.button == 0) {
                        openFolder(id);
                    }
                });
            } else {
                add_item(line, row);
            }
            list->getChildren().add(line);
        }
        auto column = jadefx::make<jadefx::ScrollPane>(list);
        column->getClassList().add("assets-column");
        column->setFitToWidth(true);
        column->setHbarPolicy(jadefx::ScrollBarPolicy::Never);
        column->setStyle("height: 100%;");
        fix_width(*column, kColumnWidth);
        strip->getChildren().add(column);
    }
    rows_ = columns.empty() ? std::vector<AssetRow>{} : columns.back();
    count_ = rows_.size();
    preview_ = jadefx::make<jadefx::VBox>();
    preview_->getClassList().add("assets-preview");
    preview_->setAlignment(jadefx::Pos::TopCenter);
    preview_->setStyle("height: 100%;");
    fix_width(*preview_, kPreviewWidth);
    strip->getChildren().add(preview_);
    scroll_->setContent(strip);
    scroll_right_ = true;
}

void IdeAssets::rebuild_preview() {
    if (!preview_) {
        return;
    }
    preview_->getChildren().clear();
    if (selected_.size() != 1) {
        return;
    }
    const engine_core::InstanceId id = selected_.front();
    engine_core::DataModel* object = world_.instance(id);
    const auto shown = std::find_if(rows_.begin(), rows_.end(), [id](const AssetRow& row) { return row.id == id; });
    // A single selected asset in the folder shown; a Folder or Prefab has its own column instead.
    if (object == nullptr || shown == rows_.end() || shown->opens) {
        return;
    }
    const std::string class_name = object->class_name();
    const double text_width = kPreviewWidth - 24;
    preview_->getChildren().add(sized_icon(class_name, kPreviewIcon));
    auto add = [&](const std::string& text, const char* style_class, jadefx::Pos alignment) {
        auto label = text_label(text, style_class);
        label->setAlignment(alignment);
        label->setStyle("width: 100%;");
        label->setMaxSize(text_width, 100000);
        preview_->getChildren().add(label);
    };
    add(world_.name(id), "assets-preview-name", jadefx::Pos::Center);
    add(class_name, "assets-preview-class", jadefx::Pos::Center);
    // Its saved properties: Path for a file asset, each reference for the rest.
    for (const engine_core::LuaField& field : engine_core::lua_saved_fields(class_name.c_str())) {
        engine_core::LuaSlot slot;
        if (field.read == nullptr || !field.read(world_, *object, slot)) {
            continue;
        }
        std::string value;
        if (slot.kind == engine_core::LuaSlot::Kind::Instance && slot.id != 0) {
            value = ref_label(world_, slot.id);
        } else if (slot.kind == engine_core::LuaSlot::Kind::String) {
            value = slot.text;
        }
        add(field.name, "assets-preview-key", jadefx::Pos::CenterLeft);
        add(value.empty() ? "None" : value, "assets-preview-value", jadefx::Pos::CenterLeft);
    }
    preview_->applyCss();
}

void IdeAssets::add_item(const std::shared_ptr<jadefx::Node>& node, const AssetRow& row) {
    node->getClassList().add("assets-item");
    node->getProperties()["asset-id"] = row.id;
    node->setOnMouseClicked([this, row](const jadefx::MouseEvent& event) { clicked(row, event); });
    items_[row.id] = node;
}

void IdeAssets::clicked(const AssetRow& row, const jadefx::MouseEvent& event) {
    if (event.button != 0) {
        return;
    }
    // The disclosure opened or closed this row just before the click bubbled here; it does not select.
    if (disclosed_) {
        disclosed_ = false;
        return;
    }
    if (event.clickCount == 2 && row.opens) {
        if (browser_.folder() != row.id) {
            openFolder(row.id);
        }
        return;
    }
    std::vector<engine_core::InstanceId> ids = world_.selection().get();
    const bool plain = !event.shortcut() && !event.shift();
    if (event.shortcut()) {
        const auto found = std::find(ids.begin(), ids.end(), row.id);
        if (found != ids.end()) {
            ids.erase(found);
        } else {
            ids.push_back(row.id);
        }
        anchor_ = row.id;
    } else if (event.shift() && anchor_ != 0) {
        auto at = [this](engine_core::InstanceId id) {
            for (std::size_t index = 0; index < rows_.size(); ++index) {
                if (rows_[index].id == id) {
                    return static_cast<long long>(index);
                }
            }
            return -1LL;
        };
        const long long from = at(anchor_);
        const long long to = at(row.id);
        ids.clear();
        if (from < 0 || to < 0) {
            ids.push_back(row.id);
            anchor_ = row.id;
        } else {
            const std::size_t low = static_cast<std::size_t>(std::min(from, to));
            const std::size_t high = static_cast<std::size_t>(std::max(from, to));
            for (std::size_t index = low; index <= high; ++index) {
                ids.push_back(rows_[index].id);
            }
        }
    } else {
        ids = {row.id};
        anchor_ = row.id;
    }
    world_.selection().set(std::move(ids));
    if (view_ != AssetView::Columns || !plain) {
        return;
    }
    // In Columns, selecting a Folder or Prefab opens its column, and selecting
    // an asset in an earlier column closes the columns after it.
    if (row.opens) {
        openFolder(row.id);
        return;
    }
    engine_core::InstanceId parent = 0;
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
        if (lock.owns()) {
            parent = world_.parent(row.id);
        }
    }
    if (parent != 0 && parent != browser_.folder()) {
        openFolder(parent);
    }
}

void IdeAssets::show_selection() {
    for (const auto& [id, node] : items_) {
        set_class(*node, "selected", std::find(selected_.begin(), selected_.end(), id) != selected_.end());
    }
    std::string text = std::to_string(count_) + (count_ == 1 ? " item" : " items");
    if (selected_.size() == 1) {
        text += " · " + world_.name(selected_.front()) + " selected";
    } else if (selected_.size() > 1) {
        text += " · " + std::to_string(selected_.size()) + " selected";
    }
    status_->setText(text);
    rebuild_preview();
}

void IdeAssets::place_clear() {
    const double x = search_field_->getAbsoluteX() + search_field_->getWidth() - kClearInset - kClearSize;
    const double y = search_field_->getAbsoluteY() + (search_field_->getHeight() - kClearSize) * 0.5;
    search_clear_->performLayout(x - getAbsoluteX(), y - getAbsoluteY(), kClearSize, kClearSize);
}

}  // namespace ide
