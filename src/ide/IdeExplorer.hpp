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
// insert creates class_name under parent and reports the new id through result.
struct ExplorerHost {
    std::function<void(std::string_view action, engine_core::InstanceId id)> run;
    std::function<bool(std::string_view action)> enabled;
    std::function<void(std::string class_name, engine_core::InstanceId parent, std::shared_ptr<InsertResult> result)>
        insert;
};

// Hierarchy under one DataModel. The hidden tree root is that instance.
// Descendant rows stay aligned with parenting: adds, removes, reparents, and
// destroy. A burst is applied in one pass so the view rebuilds once, not once
// per instance.
// A right-click opens that instance's context actions. A double-click runs the
// primary one, which for a script is Edit.
// Hovering a row shows + on its right. That opens a searchable list of classes
// Instance.new can create, and the chosen class is parented under the row.
class IdeExplorer : public IdePane {
public:
    IdeExplorer(engine_core::DataModel& root, std::string name, ExplorerHost host);

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

    void sync();
    bool find_id(const jadefx::TreeItem* item, engine_core::InstanceId& id) const;
    bool actions_for(engine_core::InstanceId id, std::vector<engine_core::ContextAction>& out) const;
    void show_menu(jadefx::TreeItem& item, double x, double y);
    bool activate(jadefx::TreeItem& item);
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
};

}  // namespace ide
