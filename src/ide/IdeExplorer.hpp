#pragma once

#include "IdePane.hpp"
#include "DataModel.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ide {

// Hierarchy under one DataModel. The hidden tree root is that instance.
// Descendant rows stay aligned with parenting: adds, removes, reparents, and
// destroy. A burst is applied in one pass so the view rebuilds once, not once
// per instance.
class IdeExplorer : public IdePane {
public:
    IdeExplorer(engine_core::DataModel& root, std::string name);

protected:
    void layoutChildren() override;

private:
    // Preorder encoding of the live hierarchy. ids[0] is the explorer root.
    // children[child_begins[i] .. + child_counts[i]] are that node's direct children.
    struct Snapshot {
        std::vector<engine_core::InstanceId> ids;
        std::vector<std::uint32_t> child_counts;
        std::vector<std::uint32_t> child_begins;
        std::vector<engine_core::InstanceId> children;
        std::vector<const char*> labels;

        void clear();
        bool same_shape(const Snapshot& other) const;
    };

    void sync();
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
    std::shared_ptr<jadefx::TreeItem> root_item_;
    std::shared_ptr<jadefx::TreeView> tree_;
    std::unordered_map<engine_core::InstanceId, std::shared_ptr<jadefx::TreeItem>> items_;
    std::unordered_set<engine_core::InstanceId> seen_;
    std::vector<engine_core::InstanceId> pending_;
    Snapshot scratch_;
    Snapshot committed_;
    bool applying_ = false;
};

}  // namespace ide
