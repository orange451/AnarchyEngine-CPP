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
    std::atomic<bool> done{false};
};

// The shell runs an action by name. enabled is false when the item should be
// shown but not clickable, such as Paste with an empty clipboard.
// run_many runs Delete or Cut once over every selected instance, so it is one
// undo step. Without it, they run on one instance at a time.
// insert creates class_name under parent and reports the new id through result.
// The explorer runs Rename itself and hands the typed name to rename.
// move puts ids under parent in the order given, each last among its children,
// as one undo step. Without it, rows do not drag.
struct ExplorerHost {
    std::function<void(std::string_view action, engine_core::InstanceId id)> run;
    std::function<void(std::string_view action, const std::vector<engine_core::InstanceId>& ids)> run_many;
    std::function<bool(std::string_view action)> enabled;
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
//
// The selection is the world's SelectionService, so every explorer and every
// script share it. The tree is in Multiple mode: a click selects one row, Ctrl
// (Cmd on macOS) and a click adds or removes a row, and Shift and a click
// selects the rows from the last clicked one. A right-click on a selected row
// keeps the rest. A Selection:Set from a script shows on the next frame.
class IdeExplorer : public IdePane {
public:
    IdeExplorer(engine_core::DataModel& root, std::string name, ExplorerHost host);

    // Runs action on the selection when those instances offer it and it is
    // enabled. Only Delete and Cut run on more than one. False when nothing
    // selected offers the action.
    bool run_on_selection(std::string_view action);

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
    bool find_id(const jadefx::TreeItem* item, engine_core::InstanceId& id) const;
    bool actions_for(engine_core::InstanceId id, std::vector<engine_core::ContextAction>& out) const;
    bool offers(engine_core::InstanceId id, std::string_view action) const;
    void run(const std::string& action, engine_core::InstanceId id);
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
    void create_child(const std::string& class_name);
    void finish_insert(engine_core::InstanceId made);
    // Copies the live hierarchy. False when the lock is busy or nothing changed.
    bool capture();
    void read_hierarchy(Snapshot& snap);
    // How many row inserts, removals, moves, and order edits this snapshot needs.
    std::size_t edit_weight() const;
    void apply(bool batch);
    void set_children(jadefx::TreeItem& item, std::uint32_t begin, std::uint32_t count, bool batch);
    jadefx::TreeItem* existing_row(engine_core::InstanceId id) const;
    std::shared_ptr<jadefx::TreeItem> row_ptr(engine_core::InstanceId id) const;

    engine_core::DataModel& root_;
    ExplorerHost host_;
    std::shared_ptr<jadefx::TreeItem> root_item_;
    std::shared_ptr<jadefx::TreeView> tree_;
    std::shared_ptr<jadefx::Menu> menu_;
    std::unordered_map<engine_core::InstanceId, std::shared_ptr<jadefx::TreeItem>> items_;
    std::unordered_set<engine_core::InstanceId> seen_;
    std::vector<engine_core::InstanceId> pending_;
    Snapshot scratch_;
    Snapshot committed_;
    bool applying_ = false;
    // True when capture read the live tree, including a read that changed nothing.
    bool read_ok_ = false;
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
