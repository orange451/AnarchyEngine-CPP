#pragma once

#include "IdePane.hpp"
#include "InsertPopup.hpp"
#include "DataModel.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ide {

// Filled on the simulation thread after an explorer insert. id stays 0 when
// the class could not be created. done is set after id.
struct InsertResult {
    std::atomic<engine_core::InstanceId> id{0};
    // Why nothing was made, such as a full place. Written before done.
    std::string error;
    std::atomic<bool> done{false};
};

// The shell runs an action. enabled is false when the item should be
// shown but not clickable, such as Paste with an empty clipboard.
// run_many runs Delete or Cut once over every selected instance, so it is one
// undo step. Without it, they run on one instance at a time.
// insert creates class_name under parent and reports the new id through result.
// The explorer runs Rename itself and hands the typed name to rename.
// move puts ids under parent in the order given, each last among its children,
// as one undo step. Without it, rows do not drag.
struct ExplorerHost {
    std::function<void(engine_core::InstanceAction action, engine_core::InstanceId id)> run;
    std::function<void(engine_core::InstanceAction action, const std::vector<engine_core::InstanceId>& ids)>
        run_many;
    std::function<bool(engine_core::InstanceAction action)> enabled;
    // A short message for the person, such as why an action did nothing.
    std::function<void(std::string text)> notice;
    std::function<void(std::string class_name, engine_core::InstanceId parent, std::shared_ptr<InsertResult> result)>
        insert;
    std::function<void(engine_core::InstanceId id, std::string name)> rename;
    std::function<void(const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId parent)> move;
};

// Hierarchy under one DataModel. The hidden tree root is that instance.
// Descendant rows stay aligned with parenting: adds, removes, reparents, and
// destroy. A burst is applied in one pass so the view rebuilds once, not once
// per instance.
// A right-click opens that instance's context actions. A double-click runs the
// primary one, which for a script is Edit.
// Rename puts a text field over the row's name with the whole name selected.
// Enter applies it. Escape, an empty name, or focus moving anywhere else
// cancels. A second click on the same row, half a second or more after the
// first, runs Rename too.
// Dragging a row moves it, and every other selected row with it when it is
// selected. Released on the middle of a row, they go in that instance.
// Released on its top or bottom edge, they go in that row's parent. Either
// way they go last among the children: sibling order is the order instances
// arrived in, not where the line was drawn. A row never goes inside itself, and Escape cancels.
// Hovering a row shows + on its right. That opens a searchable list of classes
// Instance.new can create, and the chosen class is parented under the row.
// A right-click on the empty space under the rows does the same for the
// explorer root itself.
//
// The selection is the world's SelectionService, so every explorer and every
// script share it. The tree is in Multiple mode: a click selects one row, Ctrl
// (Cmd on macOS) and a click adds or removes a row, and Shift and a click
// selects the rows from the last clicked one. A right-click on a selected row
// keeps the rest. A Selection:Set from a script shows on the next frame.
//
// The header above the tree holds the filter field, with a divider
// under it. The field filters the tree. A row shows when its Name contains the
// typed text, ignoring case, or when a row under it does. While filtering, the
// branches leading to matches are open. An empty field shows every row again,
// with the branches opened or closed as they were before. The × at the
// field's right end empties it, and is greyed out while there is nothing to clear. Escape in the field, or the X, moves the keys
// to the tree. Escape there clears the selection, as it does anywhere in the studio.
class IdeExplorer : public IdePane {
public:
    IdeExplorer(engine_core::DataModel& root, std::string name, ExplorerHost host);

    // Runs action on the selection when those instances offer it and it is
    // enabled. Only Delete and Cut run on more than one. False when nothing
    // selected offers the action.
    bool run_on_selection(engine_core::InstanceAction action);
    // Opens the branches above each selected row and scrolls the tree until the
    // tree's own selected row is in view. When the filter hides a selected
    // row, the filter is cleared first. False when nothing is selected.
    bool reveal_selection();

protected:
    void layoutChildren() override;

private:
    // Preorder encoding of the live hierarchy. ids[0] is the explorer root.
    // children[child_begins[i] .. + child_counts[i]] are that node's direct children.
    // labels[i] is that instance's Name, copied while the read lock is held.
    struct Snapshot {
        std::vector<engine_core::InstanceId> ids;
        std::vector<std::uint32_t> child_counts;
        std::vector<std::uint32_t> child_begins;
        std::vector<engine_core::InstanceId> children;
        std::vector<std::string> labels;
        // Class name of ids[i], copied while the read lock is held.
        std::vector<std::string> classes;

        void clear();
        bool same_shape(const Snapshot& other) const;
    };

    // True when the rows changed.
    bool sync();
    // The rows the tree shows: every captured row, or the ones the filter keeps.
    const Snapshot& shown() const { return filter_.empty() ? scratch_ : filtered_; }
    // Reads the filter field, and starts or ends filtering when it changed.
    void poll_filter();
    // Takes the focus from the filter field and gives it to the tree.
    void leave_filter();
    // Lays the clear button over the filter field's right end. Disabled while the field is empty.
    void place_clear();
    // Fills filtered_ from scratch_ with the matches and the rows above them.
    void filter_rows();
    void open_matches();
    // Opens the branches above each selected row, then place_reveal scrolls to one.
    void open_selection();
    void place_reveal();
    bool find_id(const jadefx::TreeItem* item, engine_core::InstanceId& id) const;
    bool actions_for(engine_core::InstanceId id, std::vector<engine_core::ContextAction>& out) const;
    // The class whose rule decides what goes under insert_parent_, for the class list.
    std::string insert_holder() const;
    bool offers(engine_core::InstanceId id, engine_core::InstanceAction action) const;
    void run(engine_core::InstanceAction action, engine_core::InstanceId id);
    void show_menu(jadefx::TreeItem& item, double x, double y);
    bool activate(jadefx::TreeItem& item);
    void dropped(const jadefx::TreeDrop& drop);
    // Seconds from the scene clock. Negative when the tree is not in a scene.
    double now() const;
    void clicked(const jadefx::MouseEvent& event);
    // Starts a slow click's rename once no double-click can follow it, and
    // forgets the last click when focus leaves the tree.
    void poll_clicks();
    void forget_clicks();
    // The tree's selection changed from a click, a key, or a call: that is
    // now the service's selection.
    void tree_selected();
    void write_selection(std::vector<engine_core::InstanceId> ids);
    bool is_selected(engine_core::InstanceId id) const;
    // Reads the service when it changed, and puts its rows into the tree when
    // either the service or the rows changed.
    void pull_selection(bool rows_changed);
    void begin_rename(engine_core::InstanceId id);
    void finish_rename(bool apply);
    // Lays the field over the renamed row's name. Cancels when that row is
    // gone, no longer selected, scrolled out of view, or the field lost focus.
    void place_rename();
    void open_insert();
    // Opens the class list under anchor. The chosen class goes under insert_parent_.
    void show_insert(jadefx::Node& anchor);
    void show_root_insert(double x, double y);
    InsertPopup& ensure_insert_popup();
    void create_child(const std::string& class_name);
    void finish_insert(engine_core::InstanceId made);
    // Copies the live hierarchy. False when the lock is busy or nothing changed.
    bool capture();
    void read_hierarchy(Snapshot& snap);
    // Orders one parent's children in snap: in class_rank's clusters, each A
    // to Z by name. Services keep the place's order, ahead of the rest.
    void sort_children(Snapshot& snap, std::uint32_t begin, std::uint32_t count);
    // How many row inserts, removals, moves, and order edits this snapshot needs.
    std::size_t edit_weight() const;
    void apply(bool batch);
    void set_children(jadefx::TreeItem& item, std::uint32_t begin, std::uint32_t count, bool batch);
    jadefx::TreeItem* existing_row(engine_core::InstanceId id) const;
    std::shared_ptr<jadefx::TreeItem> row_ptr(engine_core::InstanceId id) const;
    // The instance's row when it is in the tree, not held back by the filter.
    jadefx::TreeItem* shown_row(engine_core::InstanceId id) const;

    engine_core::DataModel& root_;
    ExplorerHost host_;
    std::shared_ptr<jadefx::TreeItem> root_item_;
    std::shared_ptr<jadefx::TreeView> tree_;
    std::shared_ptr<jadefx::Menu> menu_;
    std::unordered_map<engine_core::InstanceId, std::shared_ptr<jadefx::TreeItem>> items_;
    // items_ the other way round, so a click finds its instance without a scan.
    std::unordered_map<const jadefx::TreeItem*, engine_core::InstanceId> item_ids_;
    std::unordered_set<engine_core::InstanceId> seen_;
    std::vector<engine_core::InstanceId> pending_;
    // sort_children's keys, kept to reuse their storage.
    struct ChildKey {
        int rank;
        std::string name;
        engine_core::InstanceId id;
    };
    std::vector<ChildKey> child_keys_;
    Snapshot scratch_;
    Snapshot committed_;
    Snapshot filtered_;
    std::shared_ptr<jadefx::TextField> filter_field_;
    std::shared_ptr<jadefx::Label> filter_clear_;
    // The field's text as last read, and that text in lower case. Empty when not filtering.
    std::string filter_typed_;
    std::string filter_;
    bool filter_dirty_ = false;
    // Which rows were open when filtering started. Put back when it ends.
    std::unordered_map<engine_core::InstanceId, bool> open_before_filter_;
    bool reveal_wanted_ = false;
    engine_core::InstanceId reveal_id_ = 0;
    bool applying_ = false;
    // True when capture read the live tree, including a read that changed nothing.
    bool read_ok_ = false;
    // DataModel::tree_revision when scratch_ was read. The tree is read again
    // only when it moved.
    std::uint64_t read_revision_ = 0;
    engine_core::InstanceId insert_parent_ = 0;
    std::shared_ptr<InsertResult> pending_insert_;
    std::shared_ptr<jadefx::Node> insert_button_;
    std::unique_ptr<InsertPopup> insert_popup_;

    // Hidden until a rename. Kept as a child so it is styled every frame.
    std::shared_ptr<jadefx::TextField> rename_field_;
    bool renaming_ = false;
    engine_core::InstanceId rename_id_ = 0;
    std::string rename_from_;
    // The last single click on a row. pairs is false once that click already
    // started a rename, so the next one starts a new pair.
    bool click_held_ = false;
    bool click_pairs_ = false;
    engine_core::InstanceId click_id_ = 0;
    double click_at_ = 0;
    // The selection as last read from the service, and that read's revision.
    std::vector<engine_core::InstanceId> selected_;
    std::uint64_t selection_seen_ = ~std::uint64_t{0};
    // Set while the explorer copies the service into the tree.
    bool picking_ = false;
    // A slow click waits out the double-click window before it renames.
    bool slow_pending_ = false;
    engine_core::InstanceId slow_id_ = 0;
    double slow_at_ = 0;
};

}  // namespace ide
