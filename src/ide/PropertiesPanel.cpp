#include "PropertiesPanel.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "SelectionService.hpp"
#include "TextUndoStack.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace ide {
namespace {

using engine_core::InstanceId;

// Same wait as the explorer: a busy simulation step must not freeze the shell.
constexpr std::chrono::milliseconds kLockWait(1);

constexpr double kPad = 6;
constexpr double kRowHeight = 24;
constexpr double kRowGap = 2;
constexpr double kIndent = 10;
constexpr double kAxisGap = 4;
constexpr double kClearWidth = 24;

constexpr const char* kFieldStyle =
    "padding: 0 4px; border-width: 1px; border-style: solid; border-radius: 3px; "
    "border-color: var(--ide-field-border-color); background-color: var(--ide-field-color);";
constexpr const char* kReadOnlyStyle =
    "padding: 0 4px; border-width: 1px; border-style: solid; border-radius: 3px; "
    "border-color: var(--ide-properties-readonly-border-color); "
    "background-color: var(--ide-properties-readonly-color); color: var(--ide-muted-text-color);";
constexpr const char* kButtonStyle = "padding: 0 4px; border-radius: 3px;";
// The line under the rows: a refused edit, or what to click while picking a reference.
constexpr const char* kErrorStyle = "color: var(--ide-properties-error-color);";
constexpr const char* kHintStyle = "color: var(--ide-properties-hint-color);";
// A reference's value is a button that reads like a field. It turns blue while it waits for a pick.
constexpr const char* kPickingStyle =
    "padding: 0 4px; border-width: 1px; border-style: solid; border-radius: 3px; "
    "border-color: var(--ide-properties-picking-border-color); background-color: var(--ide-properties-picking-color);";

// One replace for the span that differs, so the stack sees a paste or a
// selection overwrite as one step. Byte ends are pulled to UTF-8 boundaries.
void RecordChange(TextUndoStack& stack, const std::string& next) {
    const std::string& previous = stack.text();
    if (next == previous) {
        return;
    }
    std::size_t start = 0;
    while (start < previous.size() && start < next.size() && previous[start] == next[start]) {
        ++start;
    }
    std::size_t previous_end = previous.size();
    std::size_t next_end = next.size();
    while (previous_end > start && next_end > start && previous[previous_end - 1] == next[next_end - 1]) {
        --previous_end;
        --next_end;
    }
    while (start > 0 && (static_cast<unsigned char>(previous[start]) & 0xC0u) == 0x80u) {
        --start;
    }
    while (previous_end < previous.size() && next_end < next.size() &&
           (static_cast<unsigned char>(previous[previous_end]) & 0xC0u) == 0x80u) {
        ++previous_end;
        ++next_end;
    }
    stack.replace(start, previous_end - start, next.substr(start, next_end - start));
}

// A value field. dirty is set by typing and cleared by show, a commit, or undo
// back to where the field started, so a blank mixed field that was only
// focused commits nothing.
class PropertyField : public jadefx::TextField {
public:
    PropertyField() {
        getClassList().add("properties-field");
        setPrefColumnCount(1);
        setStyle(kFieldStyle);
    }

    std::function<void()> on_cancel;
    TextUndoStack stack;
    bool dirty = false;
    bool was_focused = false;

    // Shows a value from the world. Typing so far is dropped.
    void show(const std::string& text) {
        mute_ = true;
        if (getText() != text) {
            setText(text);
        }
        mute_ = false;
        stack.reset(text);
        dirty = false;
    }

    void set_read_only(bool read_only) {
        setEditable(!read_only);
        setStyle(read_only ? kReadOnlyStyle : kFieldStyle);
    }

    // The focused field's own undo. False when its stack has nothing that way.
    bool undo_step(bool redo) {
        if (!isEditable() || !(redo ? stack.can_redo() : stack.can_undo())) {
            return false;
        }
        if (redo) {
            stack.redo();
        } else {
            stack.undo();
        }
        mute_ = true;
        setText(stack.text());
        positionCaret(stack.caret());
        mute_ = false;
        dirty = stack.can_undo();
        return true;
    }

protected:
    void handleKey(jadefx::KeyEvent& event) override {
        if (event.pressed && event.key == jadefx::Key::Escape && isEditable()) {
            event.consume();
            if (on_cancel) {
                on_cancel();
            }
            return;
        }
        jadefx::TextField::handleKey(event);
        note();
    }

    void handleText(jadefx::TextEvent& event) override {
        jadefx::TextField::handleText(event);
        note();
    }

private:
    void note() {
        if (mute_ || getText() == stack.text()) {
            return;
        }
        RecordChange(stack, getText());
        dirty = true;
    }

    bool mute_ = false;
};

// A Color3 value. on_pick runs when the chooser closes on a pick, so a drag
// through the chooser is one undo step. Escape closes without one. A mixed row
// draws no color, as a mixed field shows no text, and any pick in it counts,
// even of the color it keeps out of sight.
class PropertyColor : public jadefx::ColorPicker {
public:
    PropertyColor() {
        getClassList().add("properties-color");
        getColorChooser().setShowAlpha(false);
        // While the chooser is open, only the chooser changes the value.
        setOnValueChanged([this] { touched_ = touched_ || isShowing(); });
    }

    bool mixed = false;
    std::function<void()> on_pick;

protected:
    void popupShowing() override {
        if (!isShowing()) {
            before_ = getValue().toHex();
            touched_ = false;
            escaped_ = false;
        }
        jadefx::ColorPicker::popupShowing();
    }

    bool handlePopupKey(jadefx::KeyEvent& event) override {
        if (event.pressed && event.key == jadefx::Key::Escape) {
            escaped_ = true;
        }
        return jadefx::ColorPicker::handlePopupKey(event);
    }

    void popupHidden() override {
        jadefx::ColorPicker::popupHidden();
        const bool picked = touched_ && !escaped_ && (mixed || getValue().toHex() != before_);
        touched_ = false;
        if (picked && on_pick) {
            on_pick();
        }
    }

    void renderValue(jadefx::UiRenderer& renderer, float opacity, float x, float y, float width,
                     float height) override {
        if (!mixed || isShowing()) {
            jadefx::ColorPicker::renderValue(renderer, opacity, x, y, width, height);
        }
    }

private:
    std::string before_;
    bool touched_ = false;
    bool escaped_ = false;
};

// A write handed to the simulation thread. done is set after result.
struct PendingEdit {
    EditResult result;
    std::atomic<bool> done{false};
};

}  // namespace

// The docked page. Rows are placed by hand, so a layout pass is also the
// moment the panel reads the world.
class PropertiesPane : public IdePane {
public:
    PropertiesPane() : IdePane("Properties", true) {
        setIconFile("Properties.png");
        setPrefWidth(9999999);
        setMinSize(150, 80);
        setStyle("background-color: var(--ide-panel-color);");
        getClassList().add("properties-pane");
    }

    std::weak_ptr<PropertiesPanel::Impl> owner;

    // The padded box the rows fill, in this pane's coordinates.
    double inner_left() const { return contentLeft(); }
    double inner_top() const { return contentTop(); }
    double inner_width() const { return contentWidth(); }
    double inner_height() const { return contentHeight(); }

protected:
    void layoutChildren() override;
    void handleScroll(jadefx::ScrollEvent& event) override;
};

// One row's widgets. Which editors exist depends on the row's kind.
struct RowView {
    PropertyRow row;
    // The instances a commit from this row writes: the selection it was shown for.
    std::vector<InstanceId> ids;
    std::shared_ptr<jadefx::Label> name;
    std::shared_ptr<PropertyField> field;
    std::shared_ptr<PropertyField> axes[3];
    std::shared_ptr<jadefx::CheckBox> check;
    std::shared_ptr<PropertyColor> color;
    std::shared_ptr<jadefx::Button> pick;
    std::shared_ptr<jadefx::Button> clear;
    std::shared_ptr<jadefx::Tooltip> tip;
    bool tip_installed = false;

    std::vector<jadefx::Node*> nodes() const {
        std::vector<jadefx::Node*> out;
        for (jadefx::Node* node : {static_cast<jadefx::Node*>(name.get()), static_cast<jadefx::Node*>(field.get()),
                                   static_cast<jadefx::Node*>(axes[0].get()), static_cast<jadefx::Node*>(axes[1].get()),
                                   static_cast<jadefx::Node*>(axes[2].get()), static_cast<jadefx::Node*>(check.get()),
                                   static_cast<jadefx::Node*>(color.get()), static_cast<jadefx::Node*>(pick.get()),
                                   static_cast<jadefx::Node*>(clear.get())}) {
            if (node != nullptr) {
                out.push_back(node);
            }
        }
        return out;
    }

    std::vector<PropertyField*> fields() const {
        std::vector<PropertyField*> out;
        if (field) {
            out.push_back(field.get());
        }
        for (const auto& axis : axes) {
            if (axis) {
                out.push_back(axis.get());
            }
        }
        return out;
    }
};

struct PropertiesPanel::Impl : std::enable_shared_from_this<PropertiesPanel::Impl> {
    engine_core::DataModel* world = nullptr;
    engine_core::SelectionService* selection = nullptr;
    engine_core::ChangeHistoryService* history = nullptr;
    PropertiesRun run;

    std::shared_ptr<PropertiesPane> pane;
    std::shared_ptr<jadefx::Label> empty;
    std::shared_ptr<jadefx::Label> headers[2];
    std::shared_ptr<jadefx::Label> status_label;
    std::vector<std::shared_ptr<RowView>> rows;
    PropertySheet sheet;
    bool force = true;
    bool polling = false;
    std::string status;
    std::vector<std::shared_ptr<PendingEdit>> pending;

    bool picking = false;
    std::string pick_name;
    std::vector<InstanceId> pick_ids;
    std::uint64_t pick_seen = 0;

    double scroll = 0;
    double content = 0;

    void build() {
        pane = jadefx::make<PropertiesPane>();
        empty = jadefx::make<jadefx::Label>("No selection");
        empty->getClassList().add("properties-empty");
        empty->setStyle("color: var(--ide-muted-text-color);");
        pane->getChildren().add(empty);
        const char* titles[2] = {"Instance", "Data"};
        for (int index = 0; index < 2; ++index) {
            headers[index] = jadefx::make<jadefx::Label>(titles[index]);
            headers[index]->getClassList().add("properties-group");
            headers[index]->setStyle(
                "padding: 0 6px; background-color: var(--ide-properties-group-color); "
                "color: var(--ide-properties-group-text-color);");
            headers[index]->setVisible(false);
            pane->getChildren().add(headers[index]);
        }
        status_label = jadefx::make<jadefx::Label>("");
        status_label->getClassList().add("properties-status");
        status_label->setStyle(kErrorStyle);
        status_label->setVisible(false);
        pane->getChildren().add(status_label);
    }

    // ---- Reading ---------------------------------------------------------

    void poll() {
        if (world == nullptr || selection == nullptr || polling) {
            return;
        }
        polling = true;
        collect_pending();
        check_focus();
        if (picking) {
            check_pick();
        }
        read();
        polling = false;
    }

    void read() {
        std::uint64_t revision = 0;
        const std::vector<InstanceId> ids = selection->get(revision);
        PropertySheet next;
        {
            engine_core::DataModelLock lock(*world, engine_core::DataModelLock::Read, kLockWait);
            if (!lock.owns()) {
                return;
            }
            // Undo writes the world back under this lock. Wait for it to finish.
            if (history != nullptr && history->applying_undo_redo()) {
                return;
            }
            next = read_sheet(*world, ids);
        }
        if (next == sheet && !force) {
            return;
        }
        force = false;
        apply(std::move(next));
    }

    void collect_pending() {
        for (auto it = pending.begin(); it != pending.end();) {
            if (!(*it)->done.load(std::memory_order_acquire)) {
                ++it;
                continue;
            }
            if ((*it)->result.rejected) {
                status = (*it)->result.error;
            }
            it = pending.erase(it);
            force = true;
        }
    }

    // ---- Rows ------------------------------------------------------------

    static bool same_layout(const PropertySheet& a, const PropertySheet& b) {
        if (a.rows.size() != b.rows.size()) {
            return false;
        }
        for (std::size_t index = 0; index < a.rows.size(); ++index) {
            if (!a.rows[index].same_slot(b.rows[index]) || a.rows[index].writable != b.rows[index].writable) {
                return false;
            }
        }
        return true;
    }

    void apply(PropertySheet next) {
        if (next.ids != sheet.ids) {
            // Typing aimed at the old selection lands there, as if focus had left.
            for (const auto& view : rows) {
                finish_typing(*view);
            }
        }
        if (!same_layout(next, sheet) || rows.size() != next.rows.size()) {
            rebuild_rows(next);
        }
        sheet = std::move(next);
        for (std::size_t index = 0; index < rows.size() && index < sheet.rows.size(); ++index) {
            rows[index]->ids = sheet.ids;
            show_row(*rows[index], sheet.rows[index]);
        }
        empty->setVisible(sheet.ids.empty());
    }

    void finish_typing(RowView& view) {
        for (PropertyField* field : view.fields()) {
            if (field->dirty) {
                commit_field(view, *field);
            }
        }
        // Closing keeps the color picked so far, for the instances it was picked for.
        if (view.color && view.color->isShowing()) {
            view.color->hide();
        }
    }

    void rebuild_rows(const PropertySheet& next) {
        std::vector<std::shared_ptr<RowView>> kept;
        std::vector<bool> used(rows.size(), false);
        for (const PropertyRow& row : next.rows) {
            std::shared_ptr<RowView> reuse;
            for (std::size_t index = 0; index < rows.size(); ++index) {
                if (!used[index] && rows[index]->row.same_slot(row) && rows[index]->row.writable == row.writable) {
                    used[index] = true;
                    reuse = rows[index];
                    break;
                }
            }
            kept.push_back(reuse ? reuse : make_view(row));
        }
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (used[index]) {
                continue;
            }
            RowView& gone = *rows[index];
            finish_typing(gone);
            if (picking && pick_name == gone.row.name) {
                cancel_pick();
            }
            for (jadefx::Node* node : gone.nodes()) {
                if (jadefx::Scene* scene = node->getScene()) {
                    scene->releaseFocus(node);
                }
                pane->getChildren().removeIf(
                    [node](const std::shared_ptr<jadefx::Node>& child) { return child.get() == node; });
            }
        }
        rows = std::move(kept);
    }

    std::shared_ptr<PropertyField> make_field(const std::shared_ptr<RowView>& view, bool read_only) {
        auto field = jadefx::make<PropertyField>();
        field->set_read_only(read_only);
        std::weak_ptr<Impl> weak_self = shared_from_this();
        std::weak_ptr<RowView> weak_view = view;
        PropertyField* raw = field.get();
        field->setOnAction([weak_self, weak_view, raw](jadefx::ActionEvent&) {
            const auto self = weak_self.lock();
            const auto row = weak_view.lock();
            if (self && row) {
                self->commit_field(*row, *raw);
                self->release(*raw);
            }
        });
        field->on_cancel = [weak_self, weak_view, raw]() {
            const auto self = weak_self.lock();
            const auto row = weak_view.lock();
            if (self && row) {
                self->cancel_field(*row, *raw);
            }
        };
        pane->getChildren().add(field);
        return field;
    }

    std::shared_ptr<RowView> make_view(const PropertyRow& row) {
        auto view = std::make_shared<RowView>();
        view->row = row;
        view->name = jadefx::make<jadefx::Label>(row.name);
        view->name->getClassList().add("properties-name");
        view->name->setStyle(row.writable ? "color: var(--ide-text-color);"
                                          : "color: var(--ide-properties-readonly-name-color);");
        pane->getChildren().add(view->name);
        std::weak_ptr<Impl> weak_self = shared_from_this();
        std::weak_ptr<RowView> weak_view = view;
        switch (row.kind) {
        case PropertyKind::String:
        case PropertyKind::Number:
        case PropertyKind::ReadOnlyText:
            view->field = make_field(view, !row.writable);
            break;
        case PropertyKind::Vector3:
            for (auto& axis : view->axes) {
                axis = make_field(view, !row.writable);
            }
            break;
        case PropertyKind::Bool:
            view->check = jadefx::make<jadefx::CheckBox>();
            view->check->getClassList().add("properties-check");
            view->check->setDisable(!row.writable);
            view->check->setOnAction([weak_self, weak_view](jadefx::ActionEvent&) {
                const auto self = weak_self.lock();
                const auto row_view = weak_view.lock();
                if (self && row_view) {
                    self->commit_check(*row_view);
                }
            });
            pane->getChildren().add(view->check);
            break;
        case PropertyKind::Color3:
            view->color = jadefx::make<PropertyColor>();
            view->color->setDisable(!row.writable);
            view->color->on_pick = [weak_self, weak_view]() {
                const auto self = weak_self.lock();
                const auto row_view = weak_view.lock();
                if (self && row_view) {
                    self->commit_color(*row_view);
                }
            };
            pane->getChildren().add(view->color);
            break;
        case PropertyKind::Ref:
            // The value is never typed in. Clicking it picks, and Clear sets nil.
            view->tip = jadefx::make<jadefx::Tooltip>("");
            view->pick = jadefx::make<jadefx::Button>("");
            view->pick->getClassList().add("properties-pick");
            view->pick->setStyle(row.writable ? kFieldStyle : kReadOnlyStyle);
            view->pick->setAlignment(jadefx::Pos::CenterLeft);
            view->pick->setDisable(!row.writable);
            view->pick->setOnAction([weak_self, weak_view](jadefx::ActionEvent&) {
                const auto self = weak_self.lock();
                const auto row_view = weak_view.lock();
                if (self && row_view) {
                    self->toggle_pick(*row_view);
                }
            });
            view->clear = jadefx::make<jadefx::Button>("x");
            view->clear->getClassList().add("properties-clear");
            view->clear->setStyle(kButtonStyle);
            view->clear->setDisable(!row.writable);
            view->clear->setOnAction([weak_self, weak_view](jadefx::ActionEvent&) {
                const auto self = weak_self.lock();
                const auto row_view = weak_view.lock();
                if (self && row_view) {
                    self->clear_ref(*row_view);
                }
            });
            pane->getChildren().add(view->pick);
            pane->getChildren().add(view->clear);
            break;
        }
        return view;
    }

    static std::string shown_text(const PropertyRow& row) {
        if (row.mixed) {
            return {};
        }
        switch (row.kind) {
        case PropertyKind::Number:
            return format_number(row.value.number);
        case PropertyKind::Ref:
            return row.label;
        default:
            return row.value.text;
        }
    }

    // Puts the row's value in its widgets. A field being typed in keeps its text.
    void show_row(RowView& view, const PropertyRow& row) {
        view.row = row;
        auto put = [](PropertyField& field, const std::string& text) {
            if (field.isFocused() && field.dirty) {
                return;
            }
            field.show(text);
        };
        switch (row.kind) {
        case PropertyKind::String:
        case PropertyKind::Number:
        case PropertyKind::ReadOnlyText:
            put(*view.field, shown_text(row));
            break;
        case PropertyKind::Vector3: {
            const float parts[3] = {row.value.vec.x, row.value.vec.y, row.value.vec.z};
            for (int axis = 0; axis < 3; ++axis) {
                put(*view.axes[axis], row.axis_mixed[axis] ? std::string() : format_float(parts[axis]));
            }
            break;
        }
        case PropertyKind::Bool:
            // Mixed is an indeterminate box, never an unchecked "false".
            view.check->setSelected(!row.mixed && row.value.flag);
            view.check->setIndeterminate(row.mixed);
            break;
        case PropertyKind::Color3:
            // An open chooser keeps the color being picked.
            if (view.color->isShowing()) {
                break;
            }
            view.color->mixed = row.mixed;
            view.color->setValue(jadefx::Color::rgba(row.value.color.r, row.value.color.g, row.value.color.b, 1.f));
            break;
        case PropertyKind::Ref:
            view.pick->setText(shown_text(row));
            view.tip->setText(row.mixed ? std::string() : row.path);
            if (!row.mixed && !row.path.empty()) {
                if (!view.tip_installed) {
                    jadefx::Tooltip::install(view.pick.get(), view.tip);
                    view.tip_installed = true;
                }
            } else if (view.tip_installed) {
                jadefx::Tooltip::uninstall(view.pick.get());
                view.tip_installed = false;
            }
            view.pick->setStyle(picking && pick_name == row.name ? kPickingStyle
                                : row.writable                  ? kFieldStyle
                                                                : kReadOnlyStyle);
            break;
        }
    }

    // ---- Writing ---------------------------------------------------------

    void submit(const std::vector<InstanceId>& ids, const PropertyEdit& edit) {
        if (world == nullptr || ids.empty()) {
            return;
        }
        status.clear();
        auto result = std::make_shared<PendingEdit>();
        pending.push_back(result);
        auto write = [ids, edit, result](engine_core::DataModel& game) {
            result->result = apply_edit(game, ids, edit);
            result->done.store(true, std::memory_order_release);
        };
        if (run) {
            run(std::move(write));
        } else {
            write(*world);
        }
        force = true;
    }

    int axis_of(const RowView& view, const PropertyField& field) const {
        for (int axis = 0; axis < 3; ++axis) {
            if (view.axes[axis].get() == &field) {
                return axis;
            }
        }
        return -1;
    }

    // Nothing typed is nothing to write: a blank mixed field stays mixed.
    void commit_field(RowView& view, PropertyField& field) {
        if (!field.dirty || !field.isEditable() || !view.row.writable) {
            field.dirty = false;
            return;
        }
        field.dirty = false;
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = view.row.kind;
        const std::string text = field.getText();
        switch (view.row.kind) {
        case PropertyKind::String:
            edit.value.text = text;
            break;
        case PropertyKind::Number:
            if (!parse_number(text, edit.value.number)) {
                status = view.row.name + " must be a number";
                field.show(shown_text(view.row));
                return;
            }
            break;
        case PropertyKind::Vector3: {
            double component = 0;
            const int axis = axis_of(view, field);
            if (axis < 0 || !parse_number(text, component)) {
                status = view.row.name + " must be a number";
                const float parts[3] = {view.row.value.vec.x, view.row.value.vec.y, view.row.value.vec.z};
                field.show(axis >= 0 && !view.row.axis_mixed[axis] ? format_float(parts[axis]) : std::string());
                return;
            }
            edit.axis = axis;
            const float value = static_cast<float>(component);
            edit.value.vec = {axis == 0 ? value : 0.f, axis == 1 ? value : 0.f, axis == 2 ? value : 0.f};
            break;
        }
        default:
            return;
        }
        field.stack.reset(text);
        submit(view.ids, edit);
    }

    void cancel_field(RowView& view, PropertyField& field) {
        field.dirty = false;
        show_row(view, view.row);
        release(field);
    }

    void release(PropertyField& field) {
        if (jadefx::Scene* scene = field.getScene()) {
            scene->releaseFocus(&field);
        }
        field.was_focused = false;
    }

    void commit_check(RowView& view) {
        if (!view.row.writable) {
            return;
        }
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = PropertyKind::Bool;
        // From mixed, a click checks every one.
        edit.value.flag = view.check->isSelected();
        view.check->setIndeterminate(false);
        submit(view.ids, edit);
    }

    void commit_color(RowView& view) {
        if (!view.row.writable) {
            return;
        }
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = PropertyKind::Color3;
        const jadefx::Color picked = view.color->getValue();
        edit.value.color = engine_core::Color3{picked.r, picked.g, picked.b};
        view.color->mixed = false;
        submit(view.ids, edit);
    }

    void clear_ref(RowView& view) {
        if (picking) {
            cancel_pick();
        }
        PropertyEdit edit;
        edit.property = view.row.name;
        edit.kind = PropertyKind::Ref;
        edit.value.ref = engine_core::DataModel::kNoParent;
        submit(view.ids, edit);
    }

    // Blur is a commit. Focus arriving starts that field's own undo fresh.
    void check_focus() {
        for (const auto& view : rows) {
            for (PropertyField* field : view->fields()) {
                const bool focused = field->isFocused();
                if (field->was_focused && !focused && field->dirty) {
                    commit_field(*view, *field);
                }
                if (focused && !field->was_focused && !field->dirty) {
                    field->stack.reset(field->getText());
                }
                field->was_focused = focused;
            }
        }
    }

    // ---- Pick ------------------------------------------------------------

    void toggle_pick(RowView& view) {
        if (picking && pick_name == view.row.name) {
            cancel_pick();
            return;
        }
        picking = true;
        pick_name = view.row.name;
        pick_ids = view.ids;
        pick_seen = selection->revision();
        status = "Click an instance in the Explorer to set " + pick_name + ". Click " + pick_name + " again to cancel.";
        force = true;
    }

    void cancel_pick() {
        picking = false;
        pick_name.clear();
        pick_ids.clear();
        status.clear();
        force = true;
    }

    // The explorer click lands in the selection. The last instance picked is
    // the value; then the selection goes back to what was being edited.
    void check_pick() {
        if (selection->revision() == pick_seen) {
            return;
        }
        std::uint64_t revision = 0;
        const std::vector<InstanceId> picked = selection->get(revision);
        pick_seen = revision;
        if (picked == pick_ids) {
            return;
        }
        const std::string name = pick_name;
        const std::vector<InstanceId> targets = pick_ids;
        cancel_pick();
        selection->set(targets);
        if (picked.empty()) {
            return;
        }
        PropertyEdit edit;
        edit.property = name;
        edit.kind = PropertyKind::Ref;
        edit.value.ref = picked.back();
        submit(targets, edit);
    }

    // ---- Layout ----------------------------------------------------------

    void layout() {
        if (!pane) {
            return;
        }
        const double left = pane->inner_left();
        const double top = pane->inner_top();
        const double width = std::max(0.0, pane->inner_width());
        const double height = std::max(0.0, pane->inner_height());
        const double inner = std::max(0.0, width - 2 * kPad);
        const double name_width = std::clamp(inner * 0.38, 64.0, 180.0);
        const double editor_x = left + kPad + name_width;
        const double editor_width = std::max(24.0, inner - name_width);

        double y = kPad - scroll;
        auto place = [&](jadefx::Node& node, double x, double w, double h) {
            const bool inside = y + h > 0 && y < height;
            node.setVisible(inside);
            if (inside) {
                node.performLayout(x, top + y, std::max(0.0, w), h);
            }
        };

        if (sheet.ids.empty()) {
            place(*empty, left + kPad, inner, kRowHeight);
            y += kRowHeight + kRowGap;
        } else {
            empty->setVisible(false);
        }
        headers[0]->setVisible(false);
        headers[1]->setVisible(false);
        PropertyGroup group = PropertyGroup::Instance;
        bool any = false;
        for (const auto& view : rows) {
            if (!any || view->row.group != group) {
                group = view->row.group;
                any = true;
                jadefx::Label& header = *headers[group == PropertyGroup::Instance ? 0 : 1];
                place(header, left, width, kRowHeight - 2);
                y += kRowHeight - 2 + kRowGap;
            }
            place(*view->name, left + kPad + kIndent, name_width - kIndent - 4, kRowHeight);
            switch (view->row.kind) {
            case PropertyKind::Vector3: {
                const double each = (editor_width - 2 * kAxisGap) / 3;
                for (int axis = 0; axis < 3; ++axis) {
                    place(*view->axes[axis], editor_x + axis * (each + kAxisGap), each, kRowHeight);
                }
                break;
            }
            case PropertyKind::Bool:
                place(*view->check, editor_x, std::min(editor_width, kRowHeight), kRowHeight);
                break;
            case PropertyKind::Color3:
                place(*view->color, editor_x, editor_width, kRowHeight);
                break;
            case PropertyKind::Ref: {
                const double display = std::max(24.0, editor_width - kClearWidth - kAxisGap);
                place(*view->pick, editor_x, display, kRowHeight);
                place(*view->clear, editor_x + display + kAxisGap, kClearWidth, kRowHeight);
                break;
            }
            default:
                place(*view->field, editor_x, editor_width, kRowHeight);
                break;
            }
            y += kRowHeight + kRowGap;
        }
        status_label->setText(status);
        // Pick instructions are a hint. Anything else is a refused edit.
        status_label->setStyle(picking ? kHintStyle : kErrorStyle);
        if (!status.empty()) {
            y += kRowGap;
            place(*status_label, left + kPad, inner, kRowHeight);
            y += kRowHeight;
        } else {
            status_label->setVisible(false);
        }
        content = y + scroll + kPad;
        const double limit = std::max(0.0, content - height);
        if (scroll > limit) {
            scroll = limit;
        }
    }

    void scroll_by(double delta) {
        const double limit = std::max(0.0, content - (pane ? pane->inner_height() : 0.0));
        scroll = std::clamp(scroll - delta * kRowHeight, 0.0, limit);
    }
};

void PropertiesPane::layoutChildren() {
    if (const auto impl = owner.lock()) {
        impl->poll();
        impl->layout();
    }
}

void PropertiesPane::handleScroll(jadefx::ScrollEvent& event) {
    if (const auto impl = owner.lock()) {
        impl->scroll_by(event.deltaY);
        event.consume();
    }
}

PropertiesPanel::PropertiesPanel() : impl_(std::make_shared<Impl>()) {
    impl_->build();
    impl_->pane->owner = impl_;
}

PropertiesPanel::~PropertiesPanel() {
    if (impl_ && impl_->pane) {
        impl_->pane->owner.reset();
    }
}

void PropertiesPanel::bind(engine_core::DataModel& world, engine_core::SelectionService& selection,
                           engine_core::ChangeHistoryService& history) {
    impl_->world = &world;
    impl_->selection = &selection;
    impl_->history = &history;
    impl_->force = true;
    impl_->poll();
}

void PropertiesPanel::set_runner(PropertiesRun run) { impl_->run = std::move(run); }

void PropertiesPanel::rebuild() {
    impl_->force = true;
    impl_->poll();
}

std::shared_ptr<IdePane> PropertiesPanel::dock_widget() const { return impl_->pane; }

bool PropertiesPanel::owns(const jadefx::Node* node) const {
    for (const jadefx::Node* cursor = node; cursor != nullptr; cursor = cursor->getParent()) {
        if (cursor == impl_->pane.get()) {
            return true;
        }
    }
    return false;
}

bool PropertiesPanel::field_undo(bool redo) {
    jadefx::Scene* scene = impl_->pane ? impl_->pane->getScene() : nullptr;
    jadefx::Node* focused = scene != nullptr ? scene->focusedNode() : nullptr;
    if (focused == nullptr) {
        return false;
    }
    for (const auto& view : impl_->rows) {
        for (PropertyField* field : view->fields()) {
            if (field == focused || field->isAncestorOf(focused)) {
                return field->undo_step(redo);
            }
        }
    }
    return false;
}

jadefx::Node* PropertiesPanel::editor(const std::string& property, int part) const {
    for (const auto& view : impl_->rows) {
        if (view->row.name != property) {
            continue;
        }
        switch (view->row.kind) {
        case PropertyKind::Vector3:
            return part >= 0 && part < 3 ? view->axes[part].get() : nullptr;
        case PropertyKind::Bool:
            return part == 0 ? view->check.get() : nullptr;
        case PropertyKind::Color3:
            return part == 0 ? view->color.get() : nullptr;
        case PropertyKind::Ref:
            return part == 0 ? static_cast<jadefx::Node*>(view->pick.get())
                   : part == 1 ? static_cast<jadefx::Node*>(view->clear.get())
                               : nullptr;
        default:
            return part == 0 ? view->field.get() : nullptr;
        }
    }
    return nullptr;
}

const PropertySheet& PropertiesPanel::sheet() const { return impl_->sheet; }

bool PropertiesPanel::picking() const { return impl_->picking; }

const std::string& PropertiesPanel::pick_property() const { return impl_->pick_name; }

const std::string& PropertiesPanel::status() const { return impl_->status; }

}  // namespace ide
