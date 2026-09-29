#include "IdeExplorer.hpp"

#include "DataModelLock.hpp"
#include "FindBar.hpp"
#include "IdeIcons.hpp"
#include "IdeTheme.hpp"
#include "SelectionService.hpp"
#include "Strings.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ide {
namespace {

// JadeFX rebuilds every visible row when a TreeItem is inserted or removed.
// A few of those rebuilds are cheap. Past this many edits in one frame, the
// rows are rewritten while the view is detached and rebuilt once.
constexpr std::size_t kInPlaceEdits = 8;

// The simulation thread can hold the DataModel lock for a whole step.
// This wait is short so a busy step does not freeze the shell.
constexpr std::chrono::milliseconds kLockWait(1);
// An action the person asked for waits longer than a repaint, then says why it did nothing.
constexpr std::chrono::milliseconds kActionWait(250);

// A second click on the same row, at least this long after the first, renames it.
constexpr double kSlowClickSeconds = 0.5;
// JadeFX's TreeView turns two clicks inside this window into a double-click.
// A slow click waits this long before renaming, so a double-click that starts
// with it still runs the primary action.
constexpr double kDoubleClickSeconds = 0.4;
// The field starts this far before the row's name, clear of the icon, and
// stops this far short of the row's right edge. With the field's padding, the
// typed text lands where the name was drawn.
constexpr double kRenameLead = 2;
constexpr double kRenameTrail = 4;
// The + in the header, the same chip a hovered row shows.
constexpr double kAddSize = 22;

// The header takes the Search pane's look: an inset filter field, then a
// divider before the rows. kFindStylesheet styles the field itself.
constexpr const char* kExplorerRules = R"CSS(
.explorer-pane {
    background-color: var(--ide-panel-color);
}
.explorer-header {
    padding: 8px 8px 4px 8px;
    spacing: 4px;
}
.explorer-tree {
    border-width: 1px 0 0 0;
    border-style: solid;
    border-color: var(--ide-search-divider-color);
}
)CSS";

struct ApplyGuard {
    bool& flag;
    explicit ApplyGuard(bool& flag) : flag(flag) { flag = true; }
    ~ApplyGuard() { flag = false; }
};

// Actions the explorer runs over the whole selection. The rest run on one row.
bool Batchable(std::string_view action) { return action == "Delete" || action == "Cut"; }

// Keys that make a click edit the selection instead of picking one row.
constexpr int kSelectKeys = jadefx::Key::ModControl | jadefx::Key::ModSuper | jadefx::Key::ModShift;

const char* ActionIcon(std::string_view name) {
    if (name == "Edit") {
        return "Script.png";
    }
    if (name == "Cut") {
        return "Cut.png";
    }
    if (name == "Paste") {
        return "Paste.png";
    }
    if (name == "Rename") {
        return "Rename.png";
    }
    if (name == "Delete") {
        return "Cross.png";
    }
    return nullptr;
}

// The + drawn on the hovered row. The icon stays 16px; the chip is the hit target.
// Plus.png is white, and takes the chip's text color, as the find bar's icons do.
class InsertButton : public jadefx::StackPane {
public:
    InsertButton() {
        setAlignment(jadefx::Pos::Center);
        setCursor(jadefx::Cursor::Pointer);
        setStyle("color: var(--ide-explorer-button-color);");
        if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic("Plus.png")) {
            icon->setStyle("image-color: currentColor;");
            getChildren().add(std::move(icon));
        } else {
            auto plus = jadefx::make<jadefx::Label>("+");
            plus->setMouseTransparent(true);
            plus->setAlignment(jadefx::Pos::Center);
            plus->setStyle("color: var(--ide-explorer-button-color);");
            getChildren().add(std::move(plus));
        }
        setOnMouseEntered([this](const jadefx::MouseEvent&) {
            setBackground(theme_color("--ide-explorer-button-hover-color"));
        });
        setOnMouseExited([this](const jadefx::MouseEvent&) { setBackground(jadefx::Color::transparent()); });
    }
};

// The name editor laid over a row. A plain TextField leaves Escape to its
// parent; this one cancels the rename.
class RenameField : public jadefx::TextField {
public:
    explicit RenameField(std::function<void()> cancel) : cancel_(std::move(cancel)) {
        getClassList().add("explorer-rename");
        setStyle("padding: 0 3px; border-width: 0; border-radius: 4px; background-color: var(--ide-field-color);");
        setPrefColumnCount(1);
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

// The filter above the tree. Escape leaves it and keeps the text. The right
// padding is room for the clear button.
class FilterField : public jadefx::TextField {
public:
    explicit FilterField(std::function<void()> leave) : leave_(std::move(leave)) {
        getClassList().add("explorer-filter");
        getClassList().add("search-field");
        setPromptText("Filter");
        setStyle("width: 100%; padding: 3px 26px 3px 6px;");
    }

protected:
    void handleKey(jadefx::KeyEvent& event) override {
        if (event.pressed && event.key == jadefx::Key::Escape) {
            event.consume();
            if (leave_) {
                leave_();
            }
            return;
        }
        TextField::handleKey(event);
    }

private:
    std::function<void()> leave_;
};

// The clear button's chip, and its gap from the field's right edge.
constexpr double kClearSize = 18;
constexpr double kClearInset = 3;

// The × at the filter's right end, the same glyph as a tab's close button.
// place_clear disables it while the filter is empty.
class ClearButton : public jadefx::Label {
public:
    ClearButton() : Label("\u00d7") {
        getClassList().add("explorer-filter-clear");
        setAlignment(jadefx::Pos::Center);
        setFont(jadefx::Font("Open Sans", 16.f));
        setOnMouseEntered([this](const jadefx::MouseEvent&) {
            if (!isDisabled()) {
                setBackground(theme_color("--ide-explorer-button-hover-color"));
            }
        });
        setOnMouseExited([this](const jadefx::MouseEvent&) { setBackground(jadefx::Color::transparent()); });
    }
};

}  // namespace

void IdeExplorer::Snapshot::clear() {
    ids.clear();
    child_counts.clear();
    child_begins.clear();
    children.clear();
    labels.clear();
    classes.clear();
}

bool IdeExplorer::Snapshot::same_shape(const Snapshot& other) const {
    return ids == other.ids && child_counts == other.child_counts && labels == other.labels;
}

IdeExplorer::IdeExplorer(engine_core::DataModel& root, std::string name, ExplorerHost host)
    : IdePane(std::move(name), true), root_(root), host_(std::move(host)) {
    setPrefWidth(9999999);
    setMinSize(150, 80);
    getClassList().add("explorer-pane");
    setStylesheet(std::string(kFindStylesheet) + kExplorerRules);

    root_item_ = jadefx::make<jadefx::TreeItem>(root_.name(root_.id()));
    root_item_->setExpanded(true);
    if (const char* type = root_.class_name()) {
        if (std::shared_ptr<jadefx::ImageView> icon = icon_view(type)) {
            root_item_->setGraphic(std::move(icon));
        }
    }

    tree_ = jadefx::make<jadefx::TreeView>(root_item_);
    tree_->getClassList().add("explorer-tree");
    tree_->setShowRoot(false);
    tree_->setFixedCellSize(24);
    tree_->setOnContextMenuRequested([this](jadefx::TreeItem& item, const jadefx::MouseEvent& event) {
        show_menu(item, event.x, event.y);
    });
    tree_->setOnItemActivated([this](jadefx::TreeItem& item) { return activate(item); });
    tree_->setSelectionMode(jadefx::SelectionMode::Multiple);
    tree_->setOnSelectedItemsChanged([this] { tree_selected(); });
    if (host_.move) {
        tree_->setOnItemsDropped([this](const jadefx::TreeDrop& drop) { dropped(drop); });
    }
    // Row clicks bubble here after the row has selected itself.
    tree_->setOnMouseClicked([this](const jadefx::MouseEvent& event) { clicked(event); });
    insert_button_ = jadefx::make<InsertButton>();
    insert_button_->setOnMouseClicked([this](const jadefx::MouseEvent&) { open_insert(); });
    tree_->setHoverAccessory(insert_button_);
    filter_field_ = jadefx::make<FilterField>([this] { leave_filter(); });
    // Inserts under the root itself, which has no row of its own to hover.
    add_button_ = jadefx::make<InsertButton>();
    add_button_->getClassList().add("explorer-add");
    add_button_->setStyle("color: var(--ide-explorer-button-color); border-radius: 3px;");
    add_button_->setMinSize(kAddSize, kAddSize);
    add_button_->setPrefSize(kAddSize, kAddSize);
    add_button_->setMaxSize(kAddSize, kAddSize);
    jadefx::Tooltip::install(add_button_.get(), jadefx::make<jadefx::Tooltip>("Insert Object"));
    add_button_->setOnMouseClicked([this](const jadefx::MouseEvent& event) {
        if (event.button == 0) {
            insert_parent_ = root_.id();
            show_insert(*add_button_);
        }
    });
    auto actions = jadefx::make<jadefx::HBox>();
    actions->setAlignment(jadefx::Pos::CenterRight);
    actions->setStyle("width: 100%;");
    actions->getChildren().add(add_button_);
    auto header = jadefx::make<jadefx::VBox>();
    header->getClassList().add("explorer-header");
    header->setStyle("width: 100%;");
    header->getChildren().add(filter_field_);
    header->getChildren().add(actions);
    auto column = jadefx::make<jadefx::BorderPane>();
    Fill(*column);
    column->setTop(header);
    column->setCenter(tree_);
    getChildren().add(column);
    // After the column, so it draws over the field and is hit first.
    filter_clear_ = jadefx::make<ClearButton>();
    filter_clear_->setOnMouseClicked([this](const jadefx::MouseEvent&) {
        if (filter_field_->getText().empty()) {
            return;
        }
        filter_field_->clear();
        leave_filter();
    });
    getChildren().add(filter_clear_);
    // After the tree, so it draws over the rows and is hit first.
    rename_field_ = jadefx::make<RenameField>([this] { finish_rename(false); });
    rename_field_->setOnAction([this](jadefx::ActionEvent&) { finish_rename(true); });
    getChildren().add(rename_field_);
    sync();
}

void IdeExplorer::dropped(const jadefx::TreeDrop& drop) {
    engine_core::InstanceId parent = 0;
    if (!host_.move || !find_id(drop.parent(), parent)) {
        return;
    }
    std::vector<engine_core::InstanceId> ids;
    for (const jadefx::TreeItem* item : drop.items) {
        engine_core::InstanceId id = 0;
        if (find_id(item, id)) {
            ids.push_back(id);
        }
    }
    if (ids.empty()) {
        return;
    }
    if (drop.position == jadefx::TreeDropPosition::Into) {
        drop.target->setExpanded(true);
    }
    // Beside a row means that row's parent. They go last there, whatever the line showed.
    host_.move(ids, parent);
}

bool IdeExplorer::find_id(const jadefx::TreeItem* item, engine_core::InstanceId& id) const {
    if (item == nullptr) {
        return false;
    }
    if (root_item_ && item == root_item_.get()) {
        id = root_.id();
        return true;
    }
    const auto found = item_ids_.find(item);
    if (found == item_ids_.end()) {
        return false;
    }
    id = found->second;
    return true;
}

bool IdeExplorer::actions_for(engine_core::InstanceId id, std::vector<engine_core::ContextAction>& out) const {
    engine_core::DataModelLock lock(root_, engine_core::DataModelLock::Read, kLockWait);
    if (!lock.owns()) {
        return false;
    }
    const engine_core::DataModel* object = id == root_.id() ? &root_ : root_.instance(id);
    if (object == nullptr) {
        return false;
    }
    object->context_actions(out);
    return !out.empty();
}

bool IdeExplorer::offers(engine_core::InstanceId id, std::string_view action) const {
    std::vector<engine_core::ContextAction> actions;
    if (!actions_for(id, actions)) {
        return false;
    }
    for (const engine_core::ContextAction& entry : actions) {
        if (entry.name != nullptr && action == entry.name) {
            return !host_.enabled || host_.enabled(action);
        }
    }
    return false;
}

bool IdeExplorer::run_on_selection(std::string_view action) {
    if (!tree_) {
        return false;
    }
    std::vector<engine_core::InstanceId> ids;
    {
        // One wait for the whole selection, so no instance is left out because
        // its own short wait ran out. offers() takes the lock again at once.
        engine_core::DataModelLock lock(root_, engine_core::DataModelLock::Read, kActionWait);
        if (!lock.owns()) {
            if (host_.notice) {
                host_.notice("The place is busy, so " + std::string(action) + " did nothing. Try again.");
            }
            return true;
        }
        for (engine_core::InstanceId id : selected_) {
            if (shown_row(id) != nullptr && offers(id, action)) {
                ids.push_back(id);
            }
        }
    }
    if (ids.empty()) {
        return false;
    }
    if (ids.size() > 1 && Batchable(action)) {
        if (host_.run_many) {
            host_.run_many(action, ids);
        } else {
            for (engine_core::InstanceId id : ids) {
                run(std::string(action), id);
            }
        }
        return true;
    }
    // One instance: the tree's own row when it is one of them, else the last picked.
    engine_core::InstanceId one = ids.back();
    for (engine_core::InstanceId id : ids) {
        if (existing_row(id) == tree_->getSelectedItem()) {
            one = id;
        }
    }
    run(std::string(action), one);
    return true;
}

void IdeExplorer::run(const std::string& action, engine_core::InstanceId id) {
    if (action == "Rename") {
        begin_rename(id);
        return;
    }
    if (host_.run) {
        host_.run(action, id);
    }
}

void IdeExplorer::show_menu(jadefx::TreeItem& item, double x, double y) {
    // A right-click is outside the field, and a menu is not the second click of a pair.
    finish_rename(false);
    forget_clicks();
    engine_core::InstanceId id = 0;
    if (!find_id(&item, id)) {
        return;
    }
    // The tree has already selected the row, or kept the selection it is in.
    const bool many = selected_.size() > 1;
    std::vector<engine_core::ContextAction> actions;
    if (!actions_for(id, actions)) {
        return;
    }
    jadefx::Scene* scene = tree_ ? tree_->getScene() : nullptr;
    if (scene == nullptr) {
        return;
    }
    if (menu_) {
        menu_->hide();
    }
    menu_ = jadefx::make<jadefx::Menu>();
    bool any = false;
    for (const engine_core::ContextAction& action : actions) {
        if (action.name == nullptr) {
            continue;
        }
        if (any && std::string_view(action.name) == "Cut") {
            menu_->getItems().add(jadefx::make<jadefx::SeparatorMenuItem>());
        }
        auto entry = jadefx::make<jadefx::MenuItem>(action.name);
        if (const char* file = ActionIcon(action.name)) {
            if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(file)) {
                entry->setGraphic(std::move(icon));
            }
        }
        const bool on = !host_.enabled || host_.enabled(action.name);
        entry->setDisable(!on);
        const std::string name = action.name;
        const bool batch = many && Batchable(name);
        entry->setOnAction([this, id, name, batch](jadefx::ActionEvent&) {
            if (!batch || !run_on_selection(name)) {
                run(name, id);
            }
        });
        menu_->getItems().add(std::move(entry));
        any = true;
    }
    if (any) {
        menu_->show(*scene, x, y);
    }
}

bool IdeExplorer::activate(jadefx::TreeItem& item) {
    if (!host_.run) {
        return false;
    }
    engine_core::InstanceId id = 0;
    if (!find_id(&item, id)) {
        return false;
    }
    std::vector<engine_core::ContextAction> actions;
    if (!actions_for(id, actions)) {
        return false;
    }
    for (const engine_core::ContextAction& action : actions) {
        if (!action.primary || action.name == nullptr) {
            continue;
        }
        if (host_.enabled && !host_.enabled(action.name)) {
            return false;
        }
        run(action.name, id);
        return true;
    }
    return false;
}

double IdeExplorer::now() const {
    const jadefx::Scene* scene = getScene();
    return scene != nullptr ? scene->timeSeconds() : -1;
}

void IdeExplorer::clicked(const jadefx::MouseEvent& event) {
    slow_pending_ = false;
    // The release that ends a drag is not half of a rename pair.
    if (!event.stillSincePress) {
        forget_clicks();
        return;
    }
    const double at = now();
    // The row's own handler already selected it. The disclosure arrow, the +
    // button, the scrollbar, and the empty space under the rows are not row clicks.
    jadefx::TreeItem* item = nullptr;
    for (jadefx::Node* node = tree_->pick(event.x, event.y); node != nullptr && node != tree_.get();
         node = node->getParent()) {
        const std::string_view type = node->getElementType();
        if (type == "tree-disclosure-node") {
            break;
        }
        if (type == "tree-cell") {
            item = tree_->getSelectedItem();
            break;
        }
    }
    engine_core::InstanceId id = 0;
    // A click that edits the selection does not start a rename pair.
    if (at < 0 || item == nullptr || (event.mods & kSelectKeys) != 0 || !find_id(item, id)) {
        forget_clicks();
        return;
    }
    const bool same = click_held_ && click_id_ == id;
    const double gap = at - click_at_;
    if (same && gap < kDoubleClickSeconds) {
        // The tree took this pair as a double-click.
        click_held_ = false;
        return;
    }
    if (same && click_pairs_ && gap >= kSlowClickSeconds) {
        slow_pending_ = true;
        slow_id_ = id;
        slow_at_ = at;
        click_pairs_ = false;
    } else {
        click_pairs_ = true;
    }
    click_held_ = true;
    click_id_ = id;
    click_at_ = at;
}

void IdeExplorer::poll_clicks() {
    if (!click_held_ && !slow_pending_) {
        return;
    }
    // Two clicks only pair while the tree keeps focus between them.
    const jadefx::Scene* scene = getScene();
    jadefx::Node* focus = scene != nullptr ? scene->focusedNode() : nullptr;
    if (focus == nullptr || !tree_->isAncestorOf(focus)) {
        forget_clicks();
        return;
    }
    if (!slow_pending_ || now() - slow_at_ < kDoubleClickSeconds) {
        return;
    }
    slow_pending_ = false;
    const std::shared_ptr<jadefx::TreeItem> row = row_ptr(slow_id_);
    if (!row || tree_->getSelectedItem() != row.get() || !offers(slow_id_, "Rename")) {
        return;
    }
    run("Rename", slow_id_);
}

void IdeExplorer::forget_clicks() {
    click_held_ = false;
    click_pairs_ = false;
    slow_pending_ = false;
}

void IdeExplorer::tree_selected() {
    // A rebuild, or the explorer copying the service into the tree, is not a pick.
    if (picking_ || applying_) {
        return;
    }
    std::unordered_map<const jadefx::TreeItem*, engine_core::InstanceId> ids;
    ids.reserve(items_.size());
    for (const auto& entry : items_) {
        ids.emplace(entry.second.get(), entry.first);
    }
    std::vector<engine_core::InstanceId> picked;
    for (jadefx::TreeItem* item : tree_->getSelectedItems()) {
        const auto found = ids.find(item);
        if (found != ids.end()) {
            picked.push_back(found->second);
        }
    }
    write_selection(std::move(picked));
}

void IdeExplorer::write_selection(std::vector<engine_core::InstanceId> ids) {
    selected_ = ids;
    root_.selection().set(std::move(ids));
}

bool IdeExplorer::is_selected(engine_core::InstanceId id) const {
    return std::find(selected_.begin(), selected_.end(), id) != selected_.end();
}

void IdeExplorer::pull_selection(bool rows_changed) {
    const bool changed = root_.selection().revision() != selection_seen_;
    if (changed) {
        std::vector<engine_core::InstanceId> ids = root_.selection().get(selection_seen_);
        if (ids != selected_) {
            // Set from somewhere else: a script, or the other explorer. Open
            // the branches above each newly selected row so it can be seen.
            for (engine_core::InstanceId id : ids) {
                if (is_selected(id)) {
                    continue;
                }
                jadefx::TreeItem* row = existing_row(id);
                for (jadefx::TreeItem* up = row != nullptr ? row->getParent() : nullptr; up != nullptr;
                     up = up->getParent()) {
                    if (!up->isExpanded()) {
                        up->setExpanded(true);
                    }
                }
            }
            selected_ = std::move(ids);
        }
    }
    if (!changed && !rows_changed) {
        return;
    }
    // Rows for the selected instances this explorer shows. An instance made
    // this step may get its row on a later sync, which comes back here.
    std::vector<jadefx::TreeItem*> rows;
    for (engine_core::InstanceId id : selected_) {
        if (jadefx::TreeItem* row = shown_row(id)) {
            rows.push_back(row);
        }
    }
    if (rows == tree_->getSelectedItems()) {
        return;
    }
    ApplyGuard guard(picking_);
    tree_->selectItems(rows);
}

void IdeExplorer::begin_rename(engine_core::InstanceId id) {
    finish_rename(false);
    forget_clicks();
    const std::shared_ptr<jadefx::TreeItem> row = row_ptr(id);
    if (!row || !tree_ || !rename_field_ || getScene() == nullptr) {
        return;
    }
    // Renaming one row of a selection keeps the rest selected.
    if (!tree_->isSelected(row.get())) {
        tree_->select(row.get());
    }
    // The label is the instance's Name, copied at the last sync.
    rename_from_ = row->getValue();
    rename_field_->setText(rename_from_);
    rename_field_->selectAll();
    rename_field_->setVisible(true);
    rename_field_->requestFocus();
    rename_id_ = id;
    renaming_ = true;
}

void IdeExplorer::finish_rename(bool apply) {
    if (!renaming_) {
        return;
    }
    renaming_ = false;
    const bool had_focus = rename_field_->isFocused();
    rename_field_->setVisible(false);
    // Enter and Escape give the keys back to the tree. A click elsewhere keeps
    // the focus it moved.
    if (had_focus) {
        if (jadefx::Scene* scene = getScene()) {
            scene->releaseFocus(rename_field_.get());
        }
        tree_->requestFocus();
    }
    std::string name = rename_field_->getText();
    rename_field_->clear();
    if (!apply || name.empty() || name == rename_from_ || !host_.rename) {
        return;
    }
    // Show the new name now. The next sync reads the same name once the
    // simulation has applied it.
    if (const std::shared_ptr<jadefx::TreeItem> row = row_ptr(rename_id_)) {
        row->setValue(name);
    }
    host_.rename(rename_id_, std::move(name));
}

void IdeExplorer::place_rename() {
    if (!renaming_) {
        return;
    }
    const std::shared_ptr<jadefx::TreeItem> row = row_ptr(rename_id_);
    if (!row || !tree_->isSelected(row.get()) || !rename_field_->isFocused()) {
        finish_rename(false);
        return;
    }
    // The cell's label holds the name.
    jadefx::Node* cell = tree_->getCell(row.get());
    if (cell == nullptr) {
        finish_rename(false);
        return;
    }
    double left = cell->getAbsoluteX();
    const std::vector<jadefx::Node*> labels = cell->getElementsByClassName("tree-cell-label");
    if (!labels.empty()) {
        left = labels.front()->getAbsoluteX() - kRenameLead;
    }
    const double right = cell->getAbsoluteX() + cell->getWidth() - kRenameTrail;
    const double height = std::max(0.0, cell->getHeight() - 2);
    // A row cut off by the tree's edge keeps the whole field inside the tree.
    const double top = tree_->getAbsoluteY();
    const double bottom = top + tree_->getHeight();
    double y = cell->getAbsoluteY() + 1;
    y = std::min(y, bottom - height);
    y = std::max(y, top);
    const double width = std::max(0.0, right - left);
    rename_field_->performLayout(left - getAbsoluteX(), y - getAbsoluteY(), width, height);
}

void IdeExplorer::open_insert() {
    if (!tree_ || !insert_button_) {
        return;
    }
    jadefx::TreeItem* item = tree_->getHoveredItem();
    if (item == nullptr) {
        return;
    }
    engine_core::InstanceId id = 0;
    if (!find_id(item, id)) {
        return;
    }
    tree_->select(item);
    item->setExpanded(true);
    insert_parent_ = id;
    show_insert(*insert_button_);
}

void IdeExplorer::show_insert(jadefx::Node& anchor) {
    if (!insert_popup_) {
        insert_popup_ = std::make_unique<InsertPopup>();
        insert_popup_->setOnCreate([this](const std::string& name) { create_child(name); });
    }
    insert_popup_->show(anchor);
}

void IdeExplorer::create_child(const std::string& class_name) {
    if (!host_.insert || class_name.empty()) {
        return;
    }
    jadefx::TreeItem* parent = nullptr;
    if (root_item_ && insert_parent_ == root_.id()) {
        parent = root_item_.get();
    } else if (const std::shared_ptr<jadefx::TreeItem> row = row_ptr(insert_parent_)) {
        parent = row.get();
    }
    if (parent != nullptr) {
        parent->setExpanded(true);
    }
    auto result = std::make_shared<InsertResult>();
    pending_insert_ = result;
    host_.insert(class_name, insert_parent_, std::move(result));
}

void IdeExplorer::finish_insert(engine_core::InstanceId made) {
    if (!pending_insert_) {
        return;
    }
    if (made == 0 || (read_ok_ && seen_.find(made) == seen_.end())) {
        pending_insert_.reset();
        return;
    }
    // Either way the focus is still in the closed class list. The tree takes it,
    // so Delete, F, and Escape act on the new instance without another click.
    // The filter hides it. It is still the selection, for Properties.
    if (read_ok_ && !filter_.empty() && shown_row(made) == nullptr) {
        write_selection({made});
        pending_insert_.reset();
        tree_->requestFocus();
        return;
    }
    const std::shared_ptr<jadefx::TreeItem> row = row_ptr(made);
    if (!row || !tree_) {
        return;
    }
    tree_->select(row.get());
    pending_insert_.reset();
    tree_->requestFocus();
}

void IdeExplorer::layoutChildren() {
    poll_filter();
    pull_selection(sync());
    if (reveal_wanted_) {
        reveal_wanted_ = false;
        open_selection();
    }
    poll_clicks();
    StackPane::layoutChildren();
    place_clear();
    place_reveal();
    place_rename();
}

void IdeExplorer::poll_filter() {
    if (!filter_field_ || filter_field_->getText() == filter_typed_) {
        return;
    }
    filter_typed_ = filter_field_->getText();
    const std::string next = AsciiLower(filter_typed_);
    if (filter_.empty() && !next.empty()) {
        open_before_filter_.clear();
        for (const auto& entry : items_) {
            open_before_filter_[entry.first] = entry.second->isExpanded();
        }
    }
    const bool ending = !filter_.empty() && next.empty();
    filter_ = next;
    filter_dirty_ = true;
    if (!ending) {
        return;
    }
    // Rows first shown while filtering start closed.
    for (const auto& entry : items_) {
        const auto was = open_before_filter_.find(entry.first);
        entry.second->setExpanded(was != open_before_filter_.end() && was->second);
    }
    open_before_filter_.clear();
    // What was picked while filtering stays in view.
    reveal_wanted_ = true;
}

void IdeExplorer::handleKey(jadefx::KeyEvent& event) {
    // Keys bubble here from the tree. The filter and the rename field keep their Escape.
    if (event.pressed && !event.repeat && event.key == jadefx::Key::Escape && !event.shift && !event.alt &&
        !event.shortcut() && !selected_.empty()) {
        event.consume();
        write_selection({});
        return;
    }
    IdePane::handleKey(event);
}

void IdeExplorer::leave_filter() {
    if (filter_field_->isFocused()) {
        if (jadefx::Scene* scene = getScene()) {
            scene->releaseFocus(filter_field_.get());
        }
    }
    // The tree takes the keys, so a second Escape clears the selection.
    tree_->requestFocus();
}

void IdeExplorer::place_clear() {
    const bool on = !filter_field_->getText().empty();
    if (on == filter_clear_->isDisabled()) {
        filter_clear_->setDisable(!on);
        filter_clear_->setCursor(on ? jadefx::Cursor::Pointer : jadefx::Cursor::Default);
        filter_clear_->setStyle(on ? "color: var(--ide-explorer-button-color);"
                                   : "color: var(--ide-explorer-button-disabled-color);");
        if (!on) {
            filter_clear_->setBackground(jadefx::Color::transparent());
        }
    }
    const double x = filter_field_->getAbsoluteX() + filter_field_->getWidth() - kClearInset - kClearSize;
    const double y = filter_field_->getAbsoluteY() + (filter_field_->getHeight() - kClearSize) * 0.5;
    filter_clear_->performLayout(x - getAbsoluteX(), y - getAbsoluteY(), kClearSize, kClearSize);
}

bool IdeExplorer::reveal_selection() {
    if (!tree_ || selected_.empty()) {
        return false;
    }
    if (!filter_.empty()) {
        for (engine_core::InstanceId id : selected_) {
            if (shown_row(id) == nullptr) {
                filter_field_->clear();
                break;
            }
        }
    }
    reveal_wanted_ = true;
    return true;
}

void IdeExplorer::open_selection() {
    reveal_id_ = 0;
    for (engine_core::InstanceId id : selected_) {
        jadefx::TreeItem* row = shown_row(id);
        if (row == nullptr) {
            continue;
        }
        for (jadefx::TreeItem* up = row->getParent(); up != nullptr; up = up->getParent()) {
            if (!up->isExpanded()) {
                up->setExpanded(true);
            }
        }
        // The tree's own row when it is selected, else the last picked.
        if (reveal_id_ == 0 || row == tree_->getSelectedItem()) {
            reveal_id_ = id;
        }
    }
}

void IdeExplorer::place_reveal() {
    if (reveal_id_ == 0) {
        return;
    }
    jadefx::TreeItem* row = shown_row(reveal_id_);
    reveal_id_ = 0;
    if (row == nullptr) {
        return;
    }
    const double top = tree_->getAbsoluteY();
    const double bottom = top + tree_->getHeight();
    if (const jadefx::Node* cell = tree_->getCell(row)) {
        if (cell->getAbsoluteY() >= top && cell->getAbsoluteY() + cell->getHeight() <= bottom) {
            return;
        }
    }
    // Out of view: put it in the middle of the tree.
    const int index = tree_->getRow(row);
    if (index < 0) {
        return;
    }
    const int rows = static_cast<int>(tree_->getHeight() / tree_->getFixedCellSize());
    tree_->scrollTo(std::max(0, index - rows / 2));
}

bool IdeExplorer::sync() {
    if (applying_ || !tree_ || !root_item_) {
        return false;
    }
    const bool pending = pending_insert_ && pending_insert_->done.load(std::memory_order_acquire);
    const engine_core::InstanceId made = pending ? pending_insert_->id.load(std::memory_order_relaxed) : 0;
    bool changed = false;
    if (capture() || filter_dirty_) {
        filter_dirty_ = false;
        if (!filter_.empty()) {
            filter_rows();
        }
        ApplyGuard guard(applying_);
        apply(edit_weight() > kInPlaceEdits);
        if (!filter_.empty()) {
            open_matches();
        }
        committed_.ids = scratch_.ids;
        committed_.child_counts = scratch_.child_counts;
        committed_.labels = scratch_.labels;
        changed = true;
    }
    if (pending) {
        finish_insert(made);
    }
    return changed;
}

bool IdeExplorer::capture() {
    // Names and the hierarchy move the revision under the write lock, so an
    // unmoved revision means scratch_ still matches the tree.
    if (read_ok_ && root_.tree_revision() == read_revision_) {
        return false;
    }
    read_ok_ = false;
    {
        engine_core::DataModelLock lock(root_, engine_core::DataModelLock::Read, kLockWait);
        if (!lock.owns()) {
            return false;
        }
        read_revision_ = root_.tree_revision();
        read_hierarchy(scratch_);
        read_ok_ = true;
    }
    return !scratch_.same_shape(committed_);
}

void IdeExplorer::read_hierarchy(Snapshot& snap) {
    snap.clear();
    seen_.clear();
    pending_.clear();

    const engine_core::InstanceId root_id = root_.id();
    // The world root is id 0 and is not a slot. A destroyed child root is empty.
    const bool root_gone = root_id != 0 && !root_.alive(root_id);
    seen_.insert(root_id);
    pending_.push_back(root_id);

    while (!pending_.empty()) {
        const engine_core::InstanceId id = pending_.back();
        pending_.pop_back();

        // Name defaults to the class, so an unnamed instance still reads as its class.
        std::string label = root_.name(id);
        std::string type_name;
        if (id == root_.id()) {
            if (const char* type = root_.class_name()) {
                type_name = type;
            }
        } else if (const engine_core::DataModel* object = root_.instance(id)) {
            if (const char* type = object->class_name()) {
                type_name = type;
            }
        }

        const std::uint32_t begin = static_cast<std::uint32_t>(snap.children.size());
        std::uint32_t count = 0;
        // Current node is not in ids yet (about to be recorded) and not in pending (just popped).
        const std::size_t cap = engine_core::DataModel::kMaxInstances + 1;
        if (!root_gone) {
            for (engine_core::InstanceId child = root_.first_child(id); child != 0;
                 child = root_.next_sibling(child)) {
                if (seen_.find(child) != seen_.end()) {
                    continue;
                }
                if (snap.ids.size() + pending_.size() + static_cast<std::size_t>(count) + 1 >= cap) {
                    break;
                }
                seen_.insert(child);
                snap.children.push_back(child);
                ++count;
            }
        }

        snap.ids.push_back(id);
        snap.child_counts.push_back(count);
        snap.child_begins.push_back(begin);
        snap.labels.push_back(std::move(label));
        snap.classes.push_back(std::move(type_name));

        for (std::uint32_t i = count; i-- > 0;) {
            pending_.push_back(snap.children[begin + i]);
        }
    }
}

void IdeExplorer::filter_rows() {
    const Snapshot& all = scratch_;
    const std::size_t count = all.ids.size();
    std::unordered_map<engine_core::InstanceId, std::size_t> index;
    index.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        index.emplace(all.ids[i], i);
    }
    std::vector<std::size_t> parent(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::uint32_t c = 0; c < all.child_counts[i]; ++c) {
            const auto found = index.find(all.children[all.child_begins[i] + c]);
            if (found != index.end()) {
                parent[found->second] = i;
            }
        }
    }
    // Preorder puts a parent before its children, so walking back reaches the
    // children first and they can keep their parent.
    std::vector<char> keep(count, 0);
    if (count > 0) {
        keep[0] = 1;
    }
    for (std::size_t i = count; i-- > 1;) {
        if (!keep[i] && AsciiLower(all.labels[i]).find(filter_) != std::string::npos) {
            keep[i] = 1;
        }
        if (keep[i]) {
            keep[parent[i]] = 1;
        }
    }

    filtered_.clear();
    for (std::size_t i = 0; i < count; ++i) {
        if (!keep[i]) {
            continue;
        }
        const std::uint32_t begin = static_cast<std::uint32_t>(filtered_.children.size());
        std::uint32_t kept = 0;
        for (std::uint32_t c = 0; c < all.child_counts[i]; ++c) {
            const engine_core::InstanceId child = all.children[all.child_begins[i] + c];
            const auto found = index.find(child);
            if (found != index.end() && keep[found->second]) {
                filtered_.children.push_back(child);
                ++kept;
            }
        }
        filtered_.ids.push_back(all.ids[i]);
        filtered_.child_counts.push_back(kept);
        filtered_.child_begins.push_back(begin);
        filtered_.labels.push_back(all.labels[i]);
        filtered_.classes.push_back(all.classes[i]);
    }
}

void IdeExplorer::open_matches() {
    for (std::size_t i = 1; i < filtered_.ids.size(); ++i) {
        if (filtered_.child_counts[i] == 0) {
            continue;
        }
        jadefx::TreeItem* row = existing_row(filtered_.ids[i]);
        if (row != nullptr && !row->isExpanded()) {
            row->setExpanded(true);
        }
    }
}

jadefx::TreeItem* IdeExplorer::shown_row(engine_core::InstanceId id) const {
    jadefx::TreeItem* row = existing_row(id);
    for (jadefx::TreeItem* up = row; up != nullptr; up = up->getParent()) {
        if (up == root_item_.get()) {
            return row;
        }
    }
    return nullptr;
}

jadefx::TreeItem* IdeExplorer::existing_row(engine_core::InstanceId id) const {
    const auto found = items_.find(id);
    if (found == items_.end()) {
        return nullptr;
    }
    return found->second.get();
}

std::shared_ptr<jadefx::TreeItem> IdeExplorer::row_ptr(engine_core::InstanceId id) const {
    const auto found = items_.find(id);
    if (found == items_.end()) {
        return nullptr;
    }
    return found->second;
}

std::size_t IdeExplorer::edit_weight() const {
    const Snapshot& snap = shown();
    std::unordered_set<engine_core::InstanceId> live;
    live.reserve(snap.ids.size());
    std::size_t weight = 0;
    for (std::size_t i = 1; i < snap.ids.size(); ++i) {
        live.insert(snap.ids[i]);
        if (items_.find(snap.ids[i]) == items_.end()) {
            ++weight;
        }
    }
    for (const auto& entry : items_) {
        if (live.find(entry.first) == live.end()) {
            ++weight;
        }
    }

    for (std::size_t i = 0; i < snap.ids.size(); ++i) {
        jadefx::TreeItem* parent = i == 0 ? root_item_.get() : existing_row(snap.ids[i]);
        const std::uint32_t begin = snap.child_begins[i];
        const std::uint32_t count = snap.child_counts[i];
        if (parent == nullptr) {
            for (std::uint32_t c = 0; c < count; ++c) {
                if (items_.find(snap.children[begin + c]) != items_.end()) {
                    ++weight;
                }
            }
            continue;
        }

        std::unordered_set<jadefx::TreeItem*> staying;
        std::vector<jadefx::TreeItem*> desired_here;
        desired_here.reserve(count);
        for (std::uint32_t c = 0; c < count; ++c) {
            jadefx::TreeItem* row = existing_row(snap.children[begin + c]);
            if (row == nullptr) {
                continue;
            }
            if (row->getParent() != parent) {
                ++weight;
                continue;
            }
            staying.insert(row);
            desired_here.push_back(row);
        }

        std::vector<jadefx::TreeItem*> current_here;
        current_here.reserve(staying.size());
        for (const std::shared_ptr<jadefx::TreeItem>& kid : parent->getChildren().items()) {
            if (kid && staying.find(kid.get()) != staying.end()) {
                current_here.push_back(kid.get());
            }
        }
        const std::size_t compared = desired_here.size() < current_here.size() ? desired_here.size() : current_here.size();
        for (std::size_t k = 0; k < compared; ++k) {
            if (desired_here[k] != current_here[k]) {
                ++weight;
            }
        }
        if (desired_here.size() > current_here.size()) {
            weight += desired_here.size() - current_here.size();
        } else if (current_here.size() > desired_here.size()) {
            weight += current_here.size() - desired_here.size();
        }
    }
    return weight;
}

void IdeExplorer::set_children(jadefx::TreeItem& item, std::uint32_t begin, std::uint32_t count, bool batch) {
    auto& kids = item.getChildren();
    bool same = kids.size() == count;
    for (std::uint32_t i = 0; same && i < count; ++i) {
        const std::shared_ptr<jadefx::TreeItem> desired = row_ptr(shown().children[begin + i]);
        same = desired && kids[i].get() == desired.get();
    }
    if (same) {
        return;
    }

    if (batch) {
        while (!kids.empty()) {
            kids.removeAt(kids.size() - 1);
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (std::shared_ptr<jadefx::TreeItem> row = row_ptr(shown().children[begin + i])) {
                kids.add(std::move(row));
            }
        }
        return;
    }

    // Insert, remove, or move only the rows that differ. Each call rebuilds the
    // visible rows, which is why the caller detaches when edit_weight is large.
    const std::size_t limit = (kids.size() + static_cast<std::size_t>(count)) * 2 + 4;
    std::size_t index = 0;
    std::size_t steps = 0;
    while ((index < count || index < kids.size()) && steps < limit) {
        ++steps;
        const bool has_current = index < kids.size();
        const bool has_desired = index < count;
        std::shared_ptr<jadefx::TreeItem> desired = has_desired ? row_ptr(shown().children[begin + index]) : nullptr;
        if (has_current && desired && kids[index].get() == desired.get()) {
            ++index;
            continue;
        }

        bool wanted_later = false;
        if (has_current) {
            for (std::uint32_t look = static_cast<std::uint32_t>(index); look < count; ++look) {
                const std::shared_ptr<jadefx::TreeItem> later = row_ptr(shown().children[begin + look]);
                if (later && later.get() == kids[index].get()) {
                    wanted_later = true;
                    break;
                }
            }
        }
        if (has_current && !wanted_later) {
            kids.removeAt(index);
            continue;
        }
        if (!desired) {
            if (has_current) {
                kids.removeAt(index);
            } else {
                break;
            }
            continue;
        }

        std::size_t found = index;
        for (; found < kids.size(); ++found) {
            if (kids[found].get() == desired.get()) {
                break;
            }
        }
        if (found < kids.size()) {
            std::shared_ptr<jadefx::TreeItem> moving = kids[found];
            kids.removeAt(found);
            kids.insert(index, std::move(moving));
        } else {
            kids.insert(index, desired);
        }
        ++index;
    }
}

void IdeExplorer::apply(bool batch) {
    const Snapshot& snap = shown();
    if (snap.ids.empty()) {
        return;
    }

    // A batch detaches the rows, which clears the tree's selection. The
    // selection service still holds it, and pull_selection puts it back.
    if (batch) {
        tree_->setRoot(nullptr);
    }

    if (root_item_ && !snap.labels.empty() && root_item_->getValue() != snap.labels[0]) {
        root_item_->setValue(snap.labels[0]);
    }
    for (std::size_t i = 1; i < snap.ids.size(); ++i) {
        std::shared_ptr<jadefx::TreeItem>& row = items_[snap.ids[i]];
        if (!row) {
            row = jadefx::make<jadefx::TreeItem>(snap.labels[i]);
            item_ids_[row.get()] = snap.ids[i];
            if (i < snap.classes.size()) {
                if (std::shared_ptr<jadefx::ImageView> icon = icon_view(snap.classes[i])) {
                    row->setGraphic(std::move(icon));
                }
            }
        } else if (row->getValue() != snap.labels[i]) {
            row->setValue(snap.labels[i]);
        }
    }

    for (std::size_t i = 0; i < snap.ids.size(); ++i) {
        jadefx::TreeItem* parent = i == 0 ? root_item_.get() : existing_row(snap.ids[i]);
        if (parent == nullptr) {
            continue;
        }
        set_children(*parent, snap.child_begins[i], snap.child_counts[i], batch);
    }

    std::vector<engine_core::InstanceId> stale;
    for (const auto& entry : items_) {
        if (seen_.find(entry.first) == seen_.end()) {
            stale.push_back(entry.first);
        }
    }
    for (engine_core::InstanceId id : stale) {
        const auto found = items_.find(id);
        if (found == items_.end()) {
            continue;
        }
        const std::shared_ptr<jadefx::TreeItem> row = found->second;
        if (row) {
            item_ids_.erase(row.get());
            if (jadefx::TreeItem* parent = row->getParent()) {
                parent->getChildren().removeIf(
                    [&](const std::shared_ptr<jadefx::TreeItem>& child) { return child.get() == row.get(); });
            }
        }
        items_.erase(id);
    }

    if (batch) {
        tree_->setRoot(root_item_);
    }
}

}  // namespace ide
