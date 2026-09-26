#include "IdeExplorer.hpp"

#include "DataModelLock.hpp"
#include "IdeIcons.hpp"

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

struct ApplyGuard {
    bool& flag;
    explicit ApplyGuard(bool& flag) : flag(flag) { flag = true; }
    ~ApplyGuard() { flag = false; }
};

// Actions the explorer runs over the whole selection. The rest run on one row.
bool Batchable(std::string_view action) { return action == "Delete"; }

constexpr int kAddKeys = jadefx::Key::ModControl | jadefx::Key::ModSuper;
constexpr int kSelectKeys = kAddKeys | jadefx::Key::ModShift;

// JadeFX's selected row, for the rows the explorer paints itself.
const jadefx::Color kSelectedRow = jadefx::Color::rgb8(232, 240, 254);
const jadefx::Color kSelectedHover = jadefx::Color::rgb8(210, 227, 252);
constexpr double kSelectionBar = 3;

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
class InsertButton : public jadefx::StackPane {
public:
    InsertButton() {
        setAlignment(jadefx::Pos::Center);
        setCursor(jadefx::Cursor::Pointer);
        if (std::shared_ptr<jadefx::ImageView> icon = icon_file("plus-small.png")) {
            icon->setMouseTransparent(true);
            icon->setPrefSize(16, 16);
            icon->setMinSize(16, 16);
            getChildren().add(std::move(icon));
        } else {
            auto plus = jadefx::make<jadefx::Label>("+");
            plus->setMouseTransparent(true);
            plus->setAlignment(jadefx::Pos::Center);
            plus->setTextFill(jadefx::Color::rgb8(95, 99, 104));
            getChildren().add(std::move(plus));
        }
        setOnMouseEntered([this](const jadefx::MouseEvent&) {
            setBackground(jadefx::Color::rgb8(232, 240, 254));
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
        setStyle("padding: 0 3px; border-width: 0; border-radius: 4px; background-color: #ffffff;");
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

    root_item_ = jadefx::make<jadefx::TreeItem>(root_.name(root_.id()));
    root_item_->setExpanded(true);
    if (const char* type = root_.class_name()) {
        if (std::shared_ptr<jadefx::ImageView> icon = icon_view(type)) {
            root_item_->setGraphic(std::move(icon));
        }
    }

    tree_ = jadefx::make<jadefx::TreeView>(root_item_);
    tree_->setShowRoot(false);
    tree_->setFixedCellSize(24);
    tree_->setOnContextMenuRequested([this](jadefx::TreeItem& item, const jadefx::MouseEvent& event) {
        show_menu(item, event.x, event.y);
    });
    tree_->setOnItemActivated([this](jadefx::TreeItem& item) { return activate(item); });
    tree_->setOnSelectionChanged([this](jadefx::TreeItem* item) { tree_picked(item); });
    // Row clicks bubble here after the row has selected itself.
    tree_->setOnMouseClicked([this](const jadefx::MouseEvent& event) { clicked(event); });
    insert_button_ = jadefx::make<InsertButton>();
    insert_button_->setOnMouseClicked([this](const jadefx::MouseEvent&) { open_insert(); });
    tree_->setHoverAccessory(insert_button_);
    Fill(*tree_);
    getChildren().add(tree_);
    // After the tree, so it draws over the rows and is hit first.
    rename_field_ = jadefx::make<RenameField>([this] { finish_rename(false); });
    rename_field_->setOnAction([this](jadefx::ActionEvent&) { finish_rename(true); });
    getChildren().add(rename_field_);
    sync();
}

bool IdeExplorer::find_id(const jadefx::TreeItem* item, engine_core::InstanceId& id) const {
    if (item == nullptr) {
        return false;
    }
    if (root_item_ && item == root_item_.get()) {
        id = root_.id();
        return true;
    }
    for (const auto& entry : items_) {
        if (entry.second.get() == item) {
            id = entry.first;
            return true;
        }
    }
    return false;
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
    for (engine_core::InstanceId id : selected_) {
        if (existing_row(id) != nullptr && offers(id, action)) {
            ids.push_back(id);
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
    // The tree already moved its row here. A right-click on a selected row
    // keeps the rest of the selection.
    pick_pending_ = false;
    if (!is_selected(id)) {
        select_only(id);
    }
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
    const bool row = item != nullptr && find_id(item, id);
    if (row) {
        pick_pending_ = false;
        const int mods = modifiers();
        click_select(id, mods);
        if ((mods & kSelectKeys) != 0) {
            // A click that edits the selection does not start a rename pair.
            forget_clicks();
            return;
        }
    }
    if (at < 0 || !row) {
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

int IdeExplorer::modifiers() const {
    if (host_.modifiers) {
        return host_.modifiers();
    }
    const jadefx::Scene* scene = getScene();
    return scene != nullptr ? scene->modifierMask() : 0;
}

void IdeExplorer::tree_picked(jadefx::TreeItem* item) {
    // A rebuild, or the explorer's own select, is not a pick. A row that went
    // away leaves the selection as it is.
    engine_core::InstanceId id = 0;
    if (picking_ || applying_ || item == nullptr || !find_id(item, id)) {
        return;
    }
    // A row click lands here first. clicked() then takes it with the modifiers.
    pick_pending_ = true;
    pick_id_ = id;
}

void IdeExplorer::click_select(engine_core::InstanceId id, int mods) {
    const bool add = (mods & kAddKeys) != 0;
    const bool range = (mods & jadefx::Key::ModShift) != 0;
    if (range && anchor_ != 0 && existing_row(anchor_) != nullptr) {
        std::vector<engine_core::InstanceId> ids;
        if (add) {
            ids = selected_;
        }
        for (engine_core::InstanceId next : row_range(anchor_, id)) {
            if (std::find(ids.begin(), ids.end(), next) == ids.end()) {
                ids.push_back(next);
            }
        }
        write_selection(std::move(ids));
        return;
    }
    if (add) {
        std::vector<engine_core::InstanceId> ids = selected_;
        const auto found = std::find(ids.begin(), ids.end(), id);
        if (found != ids.end()) {
            ids.erase(found);
        } else {
            ids.push_back(id);
        }
        anchor_ = id;
        write_selection(std::move(ids));
        return;
    }
    select_only(id);
}

std::vector<engine_core::InstanceId> IdeExplorer::row_range(engine_core::InstanceId from,
                                                            engine_core::InstanceId to) const {
    int first = tree_->getRow(existing_row(from));
    int last = tree_->getRow(existing_row(to));
    if (first < 0 || last < 0) {
        return {to};
    }
    if (first > last) {
        std::swap(first, last);
    }
    std::unordered_map<const jadefx::TreeItem*, engine_core::InstanceId> ids;
    ids.reserve(items_.size());
    for (const auto& entry : items_) {
        ids.emplace(entry.second.get(), entry.first);
    }
    std::vector<engine_core::InstanceId> out;
    for (int row = first; row <= last; ++row) {
        const auto found = ids.find(tree_->getTreeItem(row));
        if (found != ids.end()) {
            out.push_back(found->second);
        }
    }
    return out;
}

void IdeExplorer::select_only(engine_core::InstanceId id) {
    anchor_ = id;
    write_selection({id});
}

void IdeExplorer::write_selection(std::vector<engine_core::InstanceId> ids) {
    selected_ = ids;
    root_.selection().set(std::move(ids));
}

bool IdeExplorer::is_selected(engine_core::InstanceId id) const {
    return std::find(selected_.begin(), selected_.end(), id) != selected_.end();
}

void IdeExplorer::pull_selection() {
    if (root_.selection().revision() != selection_seen_) {
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
        if (!is_selected(anchor_)) {
            anchor_ = selected_.empty() ? 0 : selected_.back();
        }
    }
    // The tree's own row stays on a selected instance: the last one picked
    // that has a row here, or none.
    jadefx::TreeItem* current = tree_->getSelectedItem();
    jadefx::TreeItem* next = nullptr;
    for (auto it = selected_.rbegin(); it != selected_.rend(); ++it) {
        jadefx::TreeItem* row = existing_row(*it);
        if (row == nullptr) {
            continue;
        }
        if (row == current) {
            return;
        }
        if (next == nullptr) {
            next = row;
        }
    }
    if (next == current) {
        return;
    }
    ApplyGuard guard(picking_);
    if (next != nullptr) {
        tree_->select(next);
    } else {
        tree_->clearSelection();
    }
}

void IdeExplorer::paint_selection() {
    const jadefx::TreeItem* own = tree_->getSelectedItem();
    std::vector<jadefx::Node*> cells;
    for (engine_core::InstanceId id : selected_) {
        jadefx::TreeItem* row = existing_row(id);
        if (row == nullptr || row == own) {
            continue;
        }
        if (cells.empty()) {
            // One cell per shown row, top to bottom, including rows scrolled away.
            cells = tree_->getElementsByClassName("tree-cell");
        }
        const int index = tree_->getRow(row);
        if (index < 0 || static_cast<std::size_t>(index) >= cells.size()) {
            continue;
        }
        auto* cell = dynamic_cast<jadefx::Region*>(cells[static_cast<std::size_t>(index)]);
        if (cell == nullptr || !cell->isVisible() || cell->getHeight() <= 0) {
            continue;
        }
        // The cell shows this row's name when the order is what it looks like.
        bool matches = false;
        for (jadefx::Node* label : cell->getElementsByClassName("tree-cell-label")) {
            auto* text = dynamic_cast<jadefx::Label*>(label);
            matches = text != nullptr && text->getText() == row->getValue();
        }
        if (!matches) {
            continue;
        }
        cell->setBackground(cell->isHovered() ? kSelectedHover : kSelectedRow);
        for (jadefx::Node* found : cell->getElementsByClassName("selection-bar")) {
            if (auto* bar = dynamic_cast<jadefx::Region*>(found)) {
                bar->setBackground(tree_->getSelectionBarColor());
                bar->setVisible(true);
                bar->performLayout(0, 0, kSelectionBar, cell->getHeight());
            }
        }
    }
}

void IdeExplorer::begin_rename(engine_core::InstanceId id) {
    finish_rename(false);
    forget_clicks();
    const std::shared_ptr<jadefx::TreeItem> row = row_ptr(id);
    if (!row || !tree_ || !rename_field_ || getScene() == nullptr) {
        return;
    }
    {
        // Renaming one row of a selection keeps the rest selected.
        ApplyGuard guard(picking_);
        tree_->select(row.get());
    }
    if (!is_selected(id)) {
        select_only(id);
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
    if (!row || tree_->getSelectedItem() != row.get() || !rename_field_->isFocused()) {
        finish_rename(false);
        return;
    }
    // The tree marks the cell drawing its selected row, and the cell's label holds the name.
    jadefx::Node* cell = nullptr;
    for (jadefx::Node* candidate : tree_->getElementsByClassName("tree-cell")) {
        if (candidate->isSelected() && candidate->isVisible() && candidate->getHeight() > 0) {
            cell = candidate;
            break;
        }
    }
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
    if (!insert_popup_) {
        insert_popup_ = std::make_unique<InsertPopup>();
        insert_popup_->setOnCreate([this](const std::string& name) { create_child(name); });
    }
    insert_popup_->show(*insert_button_);
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
    const std::shared_ptr<jadefx::TreeItem> row = row_ptr(made);
    if (!row || !tree_) {
        return;
    }
    tree_->select(row.get());
    pending_insert_.reset();
}

void IdeExplorer::layoutChildren() {
    sync();
    if (pick_pending_) {
        pick_pending_ = false;
        select_only(pick_id_);
    }
    pull_selection();
    poll_clicks();
    StackPane::layoutChildren();
    paint_selection();
    place_rename();
}

void IdeExplorer::sync() {
    if (applying_ || !tree_ || !root_item_) {
        return;
    }
    const bool pending = pending_insert_ && pending_insert_->done.load(std::memory_order_acquire);
    const engine_core::InstanceId made = pending ? pending_insert_->id.load(std::memory_order_relaxed) : 0;
    if (capture()) {
        ApplyGuard guard(applying_);
        apply(edit_weight() > kInPlaceEdits);
        committed_.ids = scratch_.ids;
        committed_.child_counts = scratch_.child_counts;
        committed_.labels = scratch_.labels;
    }
    if (pending) {
        finish_insert(made);
    }
}

bool IdeExplorer::capture() {
    read_ok_ = false;
    {
        engine_core::DataModelLock lock(root_, engine_core::DataModelLock::Read, kLockWait);
        if (!lock.owns()) {
            return false;
        }
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
    const Snapshot& snap = scratch_;
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
        const std::shared_ptr<jadefx::TreeItem> desired = row_ptr(scratch_.children[begin + i]);
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
            if (std::shared_ptr<jadefx::TreeItem> row = row_ptr(scratch_.children[begin + i])) {
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
        std::shared_ptr<jadefx::TreeItem> desired = has_desired ? row_ptr(scratch_.children[begin + index]) : nullptr;
        if (has_current && desired && kids[index].get() == desired.get()) {
            ++index;
            continue;
        }

        bool wanted_later = false;
        if (has_current) {
            for (std::uint32_t look = static_cast<std::uint32_t>(index); look < count; ++look) {
                const std::shared_ptr<jadefx::TreeItem> later = row_ptr(scratch_.children[begin + look]);
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
    const Snapshot& snap = scratch_;
    if (snap.ids.empty()) {
        return;
    }

    engine_core::InstanceId selected_id = 0;
    bool restore_selection = false;
    if (batch) {
        if (jadefx::TreeItem* selected = tree_->getSelectedItem()) {
            for (const auto& entry : items_) {
                if (entry.second.get() == selected) {
                    selected_id = entry.first;
                    restore_selection = true;
                    break;
                }
            }
        }
        tree_->setRoot(nullptr);
    }

    if (root_item_ && !snap.labels.empty() && root_item_->getValue() != snap.labels[0]) {
        root_item_->setValue(snap.labels[0]);
    }
    for (std::size_t i = 1; i < snap.ids.size(); ++i) {
        std::shared_ptr<jadefx::TreeItem>& row = items_[snap.ids[i]];
        if (!row) {
            row = jadefx::make<jadefx::TreeItem>(snap.labels[i]);
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
            if (jadefx::TreeItem* parent = row->getParent()) {
                parent->getChildren().removeIf(
                    [&](const std::shared_ptr<jadefx::TreeItem>& child) { return child.get() == row.get(); });
            }
        }
        items_.erase(id);
    }

    if (batch) {
        tree_->setRoot(root_item_);
        if (restore_selection) {
            const auto found = items_.find(selected_id);
            if (found != items_.end()) {
                tree_->select(found->second.get());
            }
        }
    }
}

}  // namespace ide
