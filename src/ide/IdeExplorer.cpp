#include "IdeExplorer.hpp"

#include "DataModelLock.hpp"
#include "IdeIcons.hpp"

#include <chrono>
#include <cstddef>
#include <utility>

namespace ide {
namespace {

// JadeFX rebuilds every visible row when a TreeItem is inserted or removed.
// A few of those rebuilds are cheap. Past this many edits in one frame, the
// rows are rewritten while the view is detached and rebuilt once.
constexpr std::size_t kInPlaceEdits = 8;

// The simulation thread can hold the DataModel lock for a whole step.
// This wait is short so a busy step does not freeze the shell.
constexpr std::chrono::milliseconds kLockWait(1);

struct ApplyGuard {
    bool& flag;
    explicit ApplyGuard(bool& flag) : flag(flag) { flag = true; }
    ~ApplyGuard() { flag = false; }
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

IdeExplorer::IdeExplorer(engine_core::DataModel& root, std::string name)
    : IdePane(std::move(name), true), root_(root) {
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
    Fill(*tree_);
    getChildren().add(tree_);
    sync();
}

void IdeExplorer::layoutChildren() {
    sync();
    StackPane::layoutChildren();
}

void IdeExplorer::sync() {
    if (applying_ || !tree_ || !root_item_) {
        return;
    }
    if (!capture()) {
        return;
    }
    ApplyGuard guard(applying_);
    apply(edit_weight() > kInPlaceEdits);
    committed_.ids = scratch_.ids;
    committed_.child_counts = scratch_.child_counts;
    committed_.labels = scratch_.labels;
}

bool IdeExplorer::capture() {
    {
        engine_core::DataModelLock lock(root_, engine_core::DataModelLock::Read, kLockWait);
        if (!lock.owns()) {
            return false;
        }
        read_hierarchy(scratch_);
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
