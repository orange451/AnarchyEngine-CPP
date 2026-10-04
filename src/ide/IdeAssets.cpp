#include "IdeAssets.hpp"
#include "LockWaits.hpp"

#include "AssetInstances.hpp"
#include "DataModelLock.hpp"
#include "FindBar.hpp"
#include "IdeIcons.hpp"
#include "IdeResources.hpp"
#include "LuaApi.hpp"
#include "PropertySheet.hpp"
#include "SelectionService.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
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
// The longer side of a texture's thumbnail, in pixels: the preview's icon at twice its points, for HiDPI.
constexpr int kThumbnailSize = 128;
// A Material's ball is drawn this big, then shrunk to kThumbnailSize, smoothing its edge and highlights.
constexpr int kBallRenderSize = 256;
// A ball takes a few milliseconds to draw, so a folder of Materials fills in over some frames.
constexpr int kBallsPerFrame = 2;

void fix_width(jadefx::Node& node, double width) {
    node.setPrefWidth(width);
    node.setMinSize(width, 0);
    node.setMaxSize(width, 100000);
}

// A list or column row's name, which a rename covers.
std::shared_ptr<jadefx::Label> row_name(const std::string& name) {
    auto label = text_label(name, "assets-cell");
    label->getClassList().add("assets-name");
    return label;
}

using engine_core::InstanceAction;

// A second click on the selected item, at least this long after the first, renames it.
constexpr double kSlowClickSeconds = 0.5;
// JadeFX turns two clicks inside this window into a double-click, so a slow
// click waits this long before renaming.
constexpr double kDoubleClickSeconds = 0.4;

const char* ActionIcon(InstanceAction action) {
    switch (action) {
    case InstanceAction::Cut:
        return "Cut.png";
    case InstanceAction::Paste:
        return "Paste.png";
    case InstanceAction::Rename:
        return "Rename.png";
    case InstanceAction::Delete:
        return "Cross.png";
    case InstanceAction::Edit:
        break;
    }
    return nullptr;
}

// The name editor laid over an item. A plain TextField leaves Escape to its
// parent; this one drops the rename.
class RenameField : public jadefx::TextField {
public:
    explicit RenameField(std::function<void()> cancel) : cancel_(std::move(cancel)) {
        getClassList().add("assets-rename");
        setStyle("padding: 0 3px; border-width: 0; border-radius: 3px; background-color: var(--ide-field-color); "
                 "font-size: 13px;");
        setPrefColumnCount(1);
        // Its keys come before a tile's tooltip, which Escape would close first, and before menu shortcuts.
        setCapturesKeys(true);
        setVisible(false);
    }

protected:
    void handleKey(jadefx::KeyEvent& event) override {
        if (event.pressed && event.key == jadefx::Key::Escape) {
            event.consume();
            if (cancel_) {
                cancel_();
            }
            return;
        }
        TextField::handleKey(event);
    }

private:
    std::function<void()> cancel_;
};

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
    : IdePane("Assets", true), world_(world), host_(std::move(host)), browser_(world),
      ball_(kBallRenderSize, kThumbnailSize),
      previews_(
          [this](const MaterialLook& look, std::shared_ptr<jadefx::Image>& image) {
              runner::ViewPixels pixels;
              if (!ball_.draw(look, pixels)) {
                  return false;
              }
              if (!pixels.empty()) {
                  image = jadefx::Image::fromRgba(pixels.width, pixels.height, std::move(pixels.rgba));
              }
              return true;
          },
          kBallsPerFrame),
      thumbnails_(kThumbnailSize, thumbnails_ready_.setter()) {
    // The material balls draw with GL of their own in renderContent.
    setDrawsRawGl(true);
    watch_ = world_.watch_changes(edited_.setter());
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
    search_clear_->setOnMouseClicked([this](const jadefx::MouseEvent& event) {
        if (event.button == 0 && !search_field_->getText().empty()) {
            search_field_->clear();
            requestFocus();
        }
    });
    getChildren().add(search_clear_);
    // After the column, so it draws over the items and is hit first.
    rename_field_ = jadefx::make<RenameField>([this] { finish_rename(false); });
    rename_field_->setOnAction([this](jadefx::ActionEvent&) { finish_rename(true); });
    getChildren().add(rename_field_);

    scroll_->setOnContextMenuRequested([this](const jadefx::MouseEvent& event) { show_empty_menu(event.x, event.y); });
    fit_view();
}

IdeAssets::~IdeAssets() { world_.unwatch_changes(watch_); }

void IdeAssets::fit_view() {
    // A search lists its matches the way List does, whatever the view.
    const bool columns = view_ == AssetView::Columns && !searching();
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
    if (search_field_->getText() != browser_.search()) {
        browser_.set_search(search_field_->getText());
        dirty_ = true;
    }
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kFrameLockWait);
        if (lock.owns()) {
            // Taken before the read, so a change made while reading reads again next frame.
            const bool edited = edited_.take();
            const bool changed = browser_.refresh() || dirty_ || !built_ || built_view_ != view_ || edited;
            if (changed) {
                rebuild();
                // Keeps the thumbnail of every file a Texture under Assets names, and
                // every Material's ball, so a folder shown again draws at once, and lets
                // the rest go. After the rebuild, so only what it shows stays queued.
                std::vector<std::filesystem::path> texture_files;
                std::vector<engine_core::InstanceId> materials;
                kept_icons(texture_files, materials);
                thumbnails_.retain(texture_files);
                previews_.retain(materials);
                // Every item shown, the preview's among them, so an edit to one rebuilds.
                std::vector<engine_core::InstanceId> shown;
                shown.reserve(items_.size());
                for (const auto& item : items_) {
                    shown.push_back(item.first);
                }
                std::sort(shown.begin(), shown.end());
                if (shown != watched_) {
                    watched_ = shown;
                    world_.set_watched(watch_, std::move(shown));
                }
            }
            if (pending_insert_ && pending_insert_->done.load(std::memory_order_acquire)) {
                finish_insert();
            }
            if (world_.selection().revision() != selection_seen_ || changed) {
                selected_ = world_.selection().get(selection_seen_);
                show_selection();
            }
        }
    }
    const bool balls_drawn = std::exchange(balls_drawn_, false);
    if (thumbnails_ready_.take() || balls_drawn) {
        refresh_icons();
    }
    back_->setDisable(!browser_.can_back());
    forward_->setDisable(!browser_.can_forward());
    poll_clicks();
    IdePane::layoutChildren();
    place_clear();
    place_rename();
    if (scroll_right_) {
        // A deeper folder opened in Columns: show its column, at the right.
        scroll_right_ = false;
        scroll_->setHvalue(scroll_->getHmax());
    }
}

void IdeAssets::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    IdePane::renderContent(renderer, opacity);
    if (!previews_.idle()) {
        // The open project's resources folder, which Project keeps on the game.
        ball_.setRoot(world_.resources_root());
        balls_drawn_ = previews_.draw_pending() || balls_drawn_;
    }
}

void IdeAssets::sceneChanged(jadefx::Scene* previous) {
    IdePane::sceneChanged(previous);
    // Leaving a live scene releases the ball's GL objects while the context
    // that made them is current, as the Scene View does; the next paint makes
    // them again. Scene teardown runs after JadeFX destroyed the context.
    if (previous == nullptr || previous->isTearingDown() || getScene() == previous) {
        return;
    }
    ball_.release();
}

void IdeAssets::handleKey(jadefx::KeyEvent& event) {
    // Keys bubble here from the items and the fields. The fields keep theirs.
    const jadefx::Scene* scene = getScene();
    const jadefx::Node* focus = scene != nullptr ? scene->focusedNode() : nullptr;
    if (!event.pressed || focus == search_field_.get() || focus == rename_field_.get()) {
        IdePane::handleKey(event);
        return;
    }
    const std::vector<engine_core::InstanceId> ids = shown_selection();
    const bool plain = !event.shortcut() && !event.shift && !event.alt;
    auto enabled = [this](InstanceAction action) {
        return !host_.actions.enabled || host_.actions.enabled(action);
    };
    const int key = event.key;
    if (plain && (key == jadefx::Key::Delete || key == jadefx::Key::Backspace) && !ids.empty()) {
        if (host_.actions.run_many && enabled(InstanceAction::Delete)) {
            host_.actions.run_many(InstanceAction::Delete, ids);
        }
        event.consume();
    } else if (plain && (key == jadefx::Key::Enter || key == jadefx::Key::KpEnter) && ids.size() == 1) {
        beginRename(ids.front());
        event.consume();
    } else if (plain && key == jadefx::Key::Escape && !world_.selection().get().empty()) {
        world_.selection().set({});
        event.consume();
    } else if (event.shortcut() && key == jadefx::Key::Up) {
        // Up one level, but not above the category. The crumbs walk the tree, so under the read lock.
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
        if (lock.owns()) {
            const std::vector<std::pair<engine_core::InstanceId, std::string>> crumbs = browser_.crumbs();
            if (crumbs.size() > 2) {
                browser_.open(crumbs[crumbs.size() - 2].first);
            }
        }
        event.consume();
    } else if (event.shortcut() && (key == jadefx::Key::LeftBracket || key == jadefx::Key::RightBracket)) {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
        if (lock.owns()) {
            if (key == jadefx::Key::LeftBracket) {
                browser_.back();
            } else {
                browser_.forward();
            }
        }
        event.consume();
    } else if (event.shortcut() && !event.shift && key == jadefx::Key::X && !ids.empty()) {
        if (host_.actions.run_many && enabled(InstanceAction::Cut)) {
            host_.actions.run_many(InstanceAction::Cut, ids);
        }
        event.consume();
    } else if (event.shortcut() && !event.shift && key == jadefx::Key::V) {
        // Paste from the keyboard goes into the folder shown.
        if (host_.actions.run && enabled(InstanceAction::Paste)) {
            host_.actions.run(InstanceAction::Paste, browser_.folder());
        }
        event.consume();
    }
    if (!event.consumed) {
        IdePane::handleKey(event);
    }
}

std::vector<engine_core::InstanceId> IdeAssets::shown_selection() const {
    std::vector<engine_core::InstanceId> out;
    for (engine_core::InstanceId id : world_.selection().get()) {
        if (items_.count(id) != 0) {
            out.push_back(id);
        }
    }
    return out;
}

double IdeAssets::now() const {
    const jadefx::Scene* scene = getScene();
    return scene != nullptr ? scene->timeSeconds() : -1;
}

void IdeAssets::poll_clicks() {
    if (!slow_pending_ || now() - slow_at_ < kDoubleClickSeconds) {
        return;
    }
    slow_pending_ = false;
    if (world_.selection().get() == std::vector<engine_core::InstanceId>{slow_id_} && itemNode(slow_id_) != nullptr &&
        (!host_.actions.enabled || host_.actions.enabled(InstanceAction::Rename))) {
        beginRename(slow_id_);
    }
}

void IdeAssets::beginRename(engine_core::InstanceId id) {
    std::string name;
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
        if (!lock.owns() || !world_.alive(id)) {
            return;
        }
        name = world_.name(id);
    }
    begin_rename(id, name);
}

void IdeAssets::begin_rename(engine_core::InstanceId id, const std::string& name) {
    finish_rename(false);
    slow_pending_ = false;
    click_id_ = 0;
    if (itemNode(id) == nullptr || getScene() == nullptr) {
        return;
    }
    // Renaming one item of a selection keeps the rest selected.
    std::vector<engine_core::InstanceId> ids = world_.selection().get();
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
        world_.selection().set({id});
    }
    rename_from_ = name;
    rename_field_->setText(name);
    rename_field_->selectAll();
    rename_field_->setVisible(true);
    rename_field_->requestFocus();
    rename_id_ = id;
    renaming_ = true;
}

void IdeAssets::finish_rename(bool apply) {
    if (!renaming_) {
        return;
    }
    renaming_ = false;
    const bool had_focus = rename_field_->isFocused();
    rename_field_->setVisible(false);
    // Enter and Escape give the keys back to the pane. A click elsewhere keeps the focus it moved.
    if (had_focus) {
        if (jadefx::Scene* scene = getScene()) {
            scene->releaseFocus(rename_field_.get());
        }
        requestFocus();
    }
    std::string name = rename_field_->getText();
    rename_field_->clear();
    if (apply && !name.empty() && name != rename_from_ && host_.actions.rename) {
        host_.actions.rename(rename_id_, std::move(name));
    }
}

void IdeAssets::place_rename() {
    if (!renaming_) {
        return;
    }
    jadefx::Node* item = itemNode(rename_id_);
    const std::vector<jadefx::Node*> labels =
        item != nullptr ? item->getElementsByClassName("assets-name") : std::vector<jadefx::Node*>{};
    if (labels.empty() || !rename_field_->isFocused()) {
        finish_rename(false);
        return;
    }
    const jadefx::Node& label = *labels.front();
    const double height = std::max(20.0, label.getHeight() + 4);
    double left = label.getAbsoluteX() - 3;
    double width = std::max(80.0, label.getWidth() + 6);
    if (view_ == AssetView::Icons && !searching()) {
        // A tile's name is centered; the field takes the tile's width.
        left = item->getAbsoluteX() + 1;
        width = item->getWidth() - 2;
    } else {
        width = std::min(width, item->getAbsoluteX() + item->getWidth() - left);
    }
    const double y = label.getAbsoluteY() + (label.getHeight() - height) * 0.5;
    rename_field_->performLayout(left - getAbsoluteX(), y - getAbsoluteY(), width, height);
}

bool IdeAssets::dropInto(const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId target) {
    if (ids.empty() || !host_.actions.move) {
        return false;
    }
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kDropLockWait);
        if (!lock.owns()) {
            if (host_.actions.notice) {
                host_.actions.notice("The place is busy, so the move did nothing. Try again.");
            }
            return false;
        }
        for (engine_core::InstanceId id : ids) {
            if (const std::optional<std::string> error = world_.parent_error(id, target)) {
                if (host_.actions.notice) {
                    host_.actions.notice(*error);
                }
                return false;
            }
        }
    }
    host_.actions.move(ids, target);
    return true;
}

void IdeAssets::accept_drops(jadefx::Node& node, engine_core::InstanceId target) {
    node.setOnDragOver([](jadefx::DragEvent& event) {
        if (event.dragboard != nullptr && event.dragboard->has(kInstanceDragFormat)) {
            event.acceptTransferModes(jadefx::TransferMode::Move);
            event.consume();
        }
    });
    node.setOnDragDropped([this, target](jadefx::DragEvent& event) {
        if (event.dragboard == nullptr || !event.dragboard->has(kInstanceDragFormat)) {
            return;
        }
        event.setDropCompleted(dropInto(instance_drag_ids(event.dragboard->get(kInstanceDragFormat)), target));
        event.consume();
    });
}

void IdeAssets::show_item_menu(const AssetRow& row, double x, double y) {
    finish_rename(false);
    slow_pending_ = false;
    click_id_ = 0;
    jadefx::Scene* scene = getScene();
    if (scene == nullptr) {
        return;
    }
    // A right-click on a selected item keeps the rest of the selection.
    std::vector<engine_core::InstanceId> ids = world_.selection().get();
    if (std::find(ids.begin(), ids.end(), row.id) == ids.end()) {
        ids = {row.id};
        world_.selection().set(ids);
    }
    const std::vector<engine_core::InstanceId> shown = shown_selection();
    if (!shown.empty()) {
        ids = shown;
    }
    if (menu_) {
        menu_->hide();
    }
    menu_ = jadefx::make<jadefx::Menu>();
    const engine_core::InstanceId id = row.id;
    if (row.class_name == "Prefab") {
        auto edit = jadefx::make<jadefx::MenuItem>(engine_core::action_label(InstanceAction::Edit));
        if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic("ModelAlt.png")) {
            edit->setGraphic(std::move(icon));
        }
        edit->setDisable(!static_cast<bool>(host_.actions.run));
        edit->setOnAction([this, id](jadefx::ActionEvent&) {
            if (host_.actions.run) {
                host_.actions.run(InstanceAction::Edit, id);
            }
        });
        menu_->getItems().add(std::move(edit));
        auto add_as_game_object = jadefx::make<jadefx::MenuItem>("Add as GameObject");
        if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic("GameObject.png")) {
            add_as_game_object->setGraphic(std::move(icon));
        }
        add_as_game_object->setDisable(!static_cast<bool>(host_.add_as_game_object));
        add_as_game_object->setOnAction([this, id](jadefx::ActionEvent&) {
            if (host_.add_as_game_object) {
                host_.add_as_game_object(id);
            }
        });
        menu_->getItems().add(std::move(add_as_game_object));
    }
    // Paste goes into the item when it holds things, else beside it.
    const engine_core::InstanceId paste_into = row.opens ? row.id : browser_.folder();
    for (const InstanceAction action :
         {InstanceAction::Rename, InstanceAction::Cut, InstanceAction::Paste, InstanceAction::Delete}) {
        if (action == InstanceAction::Cut || action == InstanceAction::Delete) {
            menu_->getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
        }
        auto entry = jadefx::make<jadefx::MenuItem>(engine_core::action_label(action));
        if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(ActionIcon(action))) {
            entry->setGraphic(std::move(icon));
        }
        bool on = !host_.actions.enabled || host_.actions.enabled(action);
        if (action == InstanceAction::Rename) {
            on = on && ids.size() == 1;
        }
        entry->setDisable(!on);
        entry->setOnAction([this, action, id, ids, paste_into](jadefx::ActionEvent&) {
            switch (action) {
            case InstanceAction::Rename:
                beginRename(id);
                break;
            case InstanceAction::Paste:
                if (host_.actions.run) {
                    host_.actions.run(action, paste_into);
                }
                break;
            default:
                if (host_.actions.run_many) {
                    host_.actions.run_many(action, ids);
                }
                break;
            }
        });
        menu_->getItems().add(std::move(entry));
    }
    menu_->show(*scene, x, y);
}

void IdeAssets::show_empty_menu(double x, double y) { show_insert_menu(browser_.folder(), "New", x, y); }

void IdeAssets::show_category_menu(engine_core::InstanceId category, double x, double y) {
    show_insert_menu(category, "Add", x, y);
}

void IdeAssets::show_insert_menu(engine_core::InstanceId folder, const std::string& verb, double x, double y) {
    finish_rename(false);
    jadefx::Scene* scene = getScene();
    if (scene == nullptr) {
        return;
    }
    std::string kind;
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kActionLockWait);
        if (lock.owns()) {
            kind = browser_.new_kind(folder);
        }
    }
    if (menu_) {
        menu_->hide();
    }
    menu_ = jadefx::make<jadefx::Menu>();
    const bool can_insert = static_cast<bool>(host_.actions.insert);
    // Import first, where a Texture, a Sound, or a Prefab comes from files on disk: an image, a sound, a model.
    if (kind == "Texture" || kind == "Sound" || kind == "Prefab") {
        auto entry = jadefx::make<jadefx::MenuItem>("Import " + std::string(kind == "Prefab" ? "Model" : kind) + "…");
        if (std::shared_ptr<jadefx::ImageView> icon = icon_view(kind)) {
            icon->setPrefSize(16, 16);
            icon->setMouseTransparent(true);
            entry->setGraphic(std::move(icon));
        }
        entry->setDisable(!host_.import_assets);
        entry->setOnAction([this, folder, kind](jadefx::ActionEvent&) {
            if (host_.import_assets) {
                host_.import_assets(folder, kind);
            }
        });
        menu_->getItems().add(std::move(entry));
    }
    // Then the folder's own kind, then a Folder, which every folder offers.
    std::vector<std::string> classes;
    if (!kind.empty() && kind != "Folder") {
        classes.push_back(kind);
    }
    classes.push_back("Folder");
    for (const std::string& klass : classes) {
        auto entry = jadefx::make<jadefx::MenuItem>(verb + " " + klass);
        if (std::shared_ptr<jadefx::ImageView> icon = icon_view(klass)) {
            icon->setPrefSize(16, 16);
            icon->setMouseTransparent(true);
            entry->setGraphic(std::move(icon));
        }
        entry->setDisable(!can_insert);
        entry->setOnAction([this, klass, folder](jadefx::ActionEvent&) { new_item(klass, folder); });
        menu_->getItems().add(std::move(entry));
    }
    menu_->getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
    auto paste = jadefx::make<jadefx::MenuItem>(engine_core::action_label(InstanceAction::Paste));
    if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic("Paste.png")) {
        paste->setGraphic(std::move(icon));
    }
    paste->setDisable(!host_.actions.run ||
                      (host_.actions.enabled && !host_.actions.enabled(InstanceAction::Paste)));
    paste->setOnAction([this, folder](jadefx::ActionEvent&) { host_.actions.run(InstanceAction::Paste, folder); });
    menu_->getItems().add(std::move(paste));
    menu_->show(*scene, x, y);
}

void IdeAssets::new_item(const std::string& class_name, engine_core::InstanceId folder) {
    if (!host_.actions.insert) {
        return;
    }
    // The new item is selected and renamed where it shows.
    if (folder != browser_.folder()) {
        openFolder(folder);
    }
    pending_insert_ = std::make_shared<InsertResult>();
    host_.actions.insert(class_name, folder, pending_insert_);
}

void IdeAssets::finish_insert() {
    const std::shared_ptr<InsertResult> result = std::move(pending_insert_);
    pending_insert_.reset();
    const engine_core::InstanceId made = result->id.load(std::memory_order_relaxed);
    if (made == 0) {
        if (!result->error.empty() && host_.actions.notice) {
            host_.actions.notice(result->error);
        }
        return;
    }
    // The tree moved before done was set, so this frame's rebuild has the new item.
    world_.selection().set({made});
    begin_rename(made, world_.name(made));
}

void IdeAssets::rebuild() {
    built_ = true;
    dirty_ = false;
    built_view_ = view_;
    icon_slots_.clear();
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
            row->setOnContextMenuRequested(
                [this, id](const jadefx::MouseEvent& event) { show_category_menu(id, event.x, event.y); });
            sidebar_->getChildren().add(row);
            accept_drops(*row, id);
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
    fit_view();
    if (searching()) {
        rows_ = browser_.search_rows();
        count_ = rows_.size();
        rebuild_search(rows_);
        scroll_->applyCss();
        return;
    }
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
        accept_drops(*crumb, id);
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
        tile->getChildren().add(asset_icon(row, kTileIcon));
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
        name->getChildren().add(row_name(row.name));
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

void IdeAssets::rebuild_search(const std::vector<AssetRow>& rows) {
    auto list = jadefx::make<jadefx::VBox>();
    list->getClassList().add("assets-list");
    list->getClassList().add("assets-search-results");
    list->setStyle("width: 100%;");
    auto header = jadefx::make<FractionRow>(kListColumns);
    header->getClassList().add("assets-list-header");
    for (const char* title : {"Name", "Kind", "Where"}) {
        auto label = text_label(title, "assets-sort");
        label->setStyle("width: 100%;");
        header->getChildren().add(label);
    }
    list->getChildren().add(header);
    for (const AssetRow& row : rows) {
        auto line = jadefx::make<FractionRow>(kListColumns);
        line->getClassList().add("assets-list-row");
        line->setMinSize(0, kRowHeight);
        line->setPrefHeight(kRowHeight);
        auto name = jadefx::make<StretchRow>(1);
        name->getChildren().add(sized_icon(row.class_name, 16));
        name->getChildren().add(row_name(row.name));
        line->getChildren().add(name);
        auto kind = text_label(row.class_name, "assets-cell");
        kind->getClassList().add("muted");
        line->getChildren().add(kind);
        // From the folder shown down to the match's parent.
        const std::size_t cut = row.where.rfind('/');
        auto where = text_label(cut == std::string::npos ? std::string() : row.where.substr(0, cut), "assets-cell");
        where->getClassList().add("muted");
        line->getChildren().add(where);
        add_item(line, row);
        list->getChildren().add(line);
    }
    if (rows.empty()) {
        list->getChildren().add(text_label("No matches in this folder.", "assets-empty"));
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
            line->getChildren().add(row_name(row.name));
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
                line->setOnContextMenuRequested(
                    [this, id](const jadefx::MouseEvent& event) { show_category_menu(id, event.x, event.y); });
                accept_drops(*line, id);
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
        // A drop on a column's space or an asset row in it moves into the folder it lists;
        // a Folder's row takes its own drop first. The categories' column lists Assets, which holds none.
        if (level > 0 && level < crumbs.size()) {
            accept_drops(*column, crumbs[level].first);
        }
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
    // A single selected asset in the folder shown; a Folder has its own column instead.
    if (object == nullptr || shown == rows_.end() || shown->opens) {
        return;
    }
    const std::string class_name = object->class_name();
    const double text_width = kPreviewWidth - 24;
    preview_->getChildren().add(asset_icon(*shown, kPreviewIcon));
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

std::filesystem::path IdeAssets::texture_file(const std::string& path) const {
    const std::filesystem::path root = world_.resources_root();
    if (path.empty() || root.empty()) {
        return {};
    }
    // Texture Paths use '/', which every platform's path splits on.
    return root / path_from_utf8(path);
}

void IdeAssets::kept_icons(std::vector<std::filesystem::path>& texture_files,
                           std::vector<engine_core::InstanceId>& materials) const {
    std::vector<engine_core::InstanceId> pending = {world_.service("Assets")};
    while (!pending.empty()) {
        const engine_core::InstanceId id = pending.back();
        pending.pop_back();
        if (id == 0) {
            continue;
        }
        if (const auto* texture = dynamic_cast<const engine_core::Texture*>(world_.instance(id))) {
            std::filesystem::path file = texture_file(texture->path());
            if (!file.empty()) {
                texture_files.push_back(std::move(file));
            }
        } else if (dynamic_cast<const engine_core::Material*>(world_.instance(id)) != nullptr) {
            materials.push_back(id);
        }
        const std::vector<engine_core::InstanceId> children = world_.get_children(id);
        pending.insert(pending.end(), children.begin(), children.end());
    }
}

std::shared_ptr<jadefx::Node> IdeAssets::asset_icon(const AssetRow& row, double size) {
    std::filesystem::path file = row.class_name == "Texture" ? texture_file(row.path) : std::filesystem::path();
    const std::optional<MaterialLook> look =
        row.class_name == "Material" ? material_look(world_, row.id) : std::optional<MaterialLook>();
    if (file.empty() && !look) {
        return sized_icon(row.class_name, size);
    }
    // A square box either way, so a wide or tall texture lines up with the icons beside it.
    auto box = jadefx::make<jadefx::StackPane>();
    box->setAlignment(jadefx::Pos::Center);
    box->setPrefSize(size, size);
    box->setMinSize(size, size);
    box->setMaxSize(size, size);
    box->setMouseTransparent(true);
    IconSlot slot;
    slot.file = std::move(file);
    if (look) {
        slot.material = row.id;
        slot.look = *look;
    }
    slot.class_name = row.class_name;
    slot.size = size;
    slot.box = box;
    show_icon(slot, slot.material != 0 ? previews_.get(slot.material, slot.look) : thumbnails_.get(slot.file));
    icon_slots_.push_back(std::move(slot));
    return box;
}

bool IdeAssets::show_icon(IconSlot& slot, std::shared_ptr<jadefx::Image> image) {
    const std::shared_ptr<jadefx::StackPane> box = slot.box.lock();
    if (!box || (!box->getChildren().empty() && image == slot.shown)) {
        return false;
    }
    slot.shown = image;
    box->getChildren().clear();
    if (!image || image->getWidth() <= 0 || image->getHeight() <= 0) {
        box->getChildren().add(sized_icon(slot.class_name, slot.size));
        return true;
    }
    const double scale = slot.size / std::max(image->getWidth(), image->getHeight());
    const double width = std::max(1.0, image->getWidth() * scale);
    const double height = std::max(1.0, image->getHeight() * scale);
    auto view = jadefx::make<jadefx::ImageView>(std::move(image));
    view->getClassList().add(slot.material != 0 ? "assets-material-image" : "assets-texture-image");
    view->setPrefSize(width, height);
    view->setMinSize(width, height);
    view->setMaxSize(width, height);
    view->setMouseTransparent(true);
    box->getChildren().add(view);
    return true;
}

void IdeAssets::refresh_icons() {
    icon_slots_.erase(std::remove_if(icon_slots_.begin(), icon_slots_.end(),
                                     [](const IconSlot& slot) { return slot.box.expired(); }),
                      icon_slots_.end());
    for (IconSlot& slot : icon_slots_) {
        if (show_icon(slot, slot.material != 0 ? previews_.get(slot.material, slot.look) : thumbnails_.get(slot.file))) {
            slot.box.lock()->applyCss();
        }
    }
}

void IdeAssets::add_item(const std::shared_ptr<jadefx::Node>& node, const AssetRow& row) {
    node->getClassList().add("assets-item");
    node->getProperties()["asset-id"] = row.id;
    node->setOnMouseClicked([this, row](const jadefx::MouseEvent& event) { clicked(row, event); });
    node->setOnContextMenuRequested(
        [this, row](const jadefx::MouseEvent& event) { show_item_menu(row, event.x, event.y); });
    jadefx::Node* source = node.get();
    const engine_core::InstanceId id = row.id;
    const std::string class_name = row.class_name;
    node->setOnDragDetected([this, source, id, class_name](const jadefx::MouseEvent&) {
        finish_rename(false);
        slow_pending_ = false;
        click_id_ = 0;
        // The selection when the item is in it, else the item alone.
        std::vector<engine_core::InstanceId> ids = shown_selection();
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids = {id};
        }
        // Move into folders, Link into Properties, and Copy onto a Scene View, which adds a GameObject.
        if (jadefx::Dragboard* board = source->startDragAndDrop(jadefx::TransferMode::Move | jadefx::TransferMode::Copy |
                                                               jadefx::TransferMode::Link)) {
            board->put(kInstanceDragFormat, instance_drag_text(ids));
            const std::shared_ptr<jadefx::ImageView> icon = icon_view(class_name);
            board->setDragView(drag_icon(icon ? icon->getImage() : nullptr), kDragIconSize * 0.5,
                               kDragIconSize * 0.5);
        }
    });
    if (row.opens) {
        accept_drops(*node, row.id);
    }
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
    // Keys go to the pane from here: Delete, Enter, Escape, and the rest.
    requestFocus();
    if (event.clickCount >= 2 || !event.stillSincePress) {
        // A double-click, or the release that ends a drag, is not half of a rename pair.
        slow_pending_ = false;
        click_id_ = 0;
        if (event.clickCount == 2 && row.opens && browser_.folder() != row.id) {
            openFolder(row.id);
        } else if (event.clickCount == 2 && row.class_name == "Prefab" && host_.actions.run) {
            host_.actions.run(InstanceAction::Edit, row.id);
        }
        return;
    }
    std::vector<engine_core::InstanceId> ids = world_.selection().get();
    const double at = now();
    if (!event.shortcut() && !event.shift()) {
        // A second plain click on the only selected item, after a pause, renames it.
        if (click_id_ == row.id && ids == std::vector<engine_core::InstanceId>{row.id} &&
            at - click_at_ >= kSlowClickSeconds) {
            slow_pending_ = true;
            slow_id_ = row.id;
            slow_at_ = at;
            click_id_ = 0;
        } else {
            click_id_ = row.id;
            click_at_ = at;
        }
    } else {
        click_id_ = 0;
    }
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
        auto index_of = [this](engine_core::InstanceId id) {
            for (std::size_t index = 0; index < rows_.size(); ++index) {
                if (rows_[index].id == id) {
                    return static_cast<long long>(index);
                }
            }
            return -1LL;
        };
        const long long from = index_of(anchor_);
        const long long to = index_of(row.id);
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
    // In Columns, selecting a Folder opens its column, and selecting
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
    // Only what this view shows: a Part picked in an explorer, or a dead id, is not counted.
    std::vector<engine_core::InstanceId> shown;
    for (engine_core::InstanceId id : selected_) {
        if (items_.count(id) != 0) {
            shown.push_back(id);
        }
    }
    std::string text = std::to_string(count_) + (count_ == 1 ? " item" : " items");
    if (shown.size() == 1) {
        text += " · " + world_.name(shown.front()) + " selected";
    } else if (shown.size() > 1) {
        text += " · " + std::to_string(shown.size()) + " selected";
    }
    status_->setText(text);
    rebuild_preview();
}

void IdeAssets::place_clear() {
    // Greyed out while there is nothing to clear.
    const bool on = !search_field_->getText().empty();
    if (on == search_clear_->isDisable()) {
        search_clear_->setDisable(!on);
        search_clear_->setCursor(on ? jadefx::Cursor::Pointer : jadefx::Cursor::Default);
    }
    const double x = search_field_->getAbsoluteX() + search_field_->getWidth() - kClearInset - kClearSize;
    const double y = search_field_->getAbsoluteY() + (search_field_->getHeight() - kClearSize) * 0.5;
    search_clear_->performLayout(x - getAbsoluteX(), y - getAbsoluteY(), kClearSize, kClearSize);
}

}  // namespace ide
