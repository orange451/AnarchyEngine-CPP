#pragma once

#include "IdePane.hpp"
#include "Project.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace ide {

// What the Conflicts window needs from the studio.
struct ConflictsHost {
    // Applies the picked rows. The studio checks again and refreshes the window.
    std::function<void(const std::vector<engine_core::DiskChoice>&)> apply;
    // Checks the disk again now.
    std::function<void()> refresh;
    // Shows the instance with this GUID in the explorers.
    std::function<void(const std::string& guid)> select;
    // The class of the instance with this GUID, for its icon. Empty for none.
    std::function<std::string(const std::string& guid)> class_of;
};

class SideToggle;

// The rows a check found where the disk and the studio changed the same thing,
// in a pane that docks like any other. Each instance is a row with its name,
// where it is, and how many rows it has; its rows are under it, one per
// property, showing the studio's value and the disk's. A row picks IDE or Disk
// with its toggle, an instance's toggle picks for all its rows, and All IDE and
// All Disk pick for every row. Apply applies the picked rows together; a row
// with no pick stays. Clicking a row, away from its toggle, shows its instance.
class IdeConflicts : public IdePane {
public:
    explicit IdeConflicts(ConflictsHost host);
    ~IdeConflicts() override;

    // The rows to show. A row that is the same as one shown before, by GUID,
    // key, and both values, keeps its pick.
    void setConflicts(std::vector<engine_core::SaveConflict> rows);
    const std::vector<engine_core::SaveConflict>& conflicts() const { return rows_; }
    // Why the list could not be checked, over the list. Empty hides it.
    void setProblem(const std::string& text);
    // Apply is off during a test, and the footer says to stop it.
    void setApplyEnabled(bool enabled);

    // A row's pick: none, IDE (false), or Disk (true).
    std::optional<bool> pick(std::size_t index) const;
    void choose(std::size_t index, bool disk);
    void chooseInstance(const std::string& guid, bool disk);
    void chooseAll(bool disk);
    // The picked rows, in list order.
    std::vector<engine_core::DiskChoice> choices() const;
    void apply();

    // "4 conflicts in 3 instances", or what an empty list means.
    std::string summary() const;
    // "3 of 4 chosen".
    std::string chosenText() const;
    jadefx::TreeView& tree() const { return *tree_; }
    // Shows the instance a group or row item is about. False for no such item.
    bool showInstance(const jadefx::TreeItem* item);

private:
    using Identity = std::tuple<std::string, std::string, std::string, std::string>;
    static Identity identity(const engine_core::SaveConflict& row);

    void rebuild();
    // The toggles and the counts, after a pick changes.
    void sync();
    void set_pick(std::size_t index, std::optional<bool> pick);
    void clicked(const jadefx::MouseEvent& event);

    ConflictsHost host_;
    std::vector<engine_core::SaveConflict> rows_;
    std::map<Identity, bool> picks_;
    std::shared_ptr<jadefx::Label> summary_;
    std::shared_ptr<jadefx::Label> problem_;
    std::shared_ptr<jadefx::VBox> top_;
    std::shared_ptr<jadefx::Button> all_ide_;
    std::shared_ptr<jadefx::Button> all_disk_;
    std::shared_ptr<jadefx::TreeView> tree_;
    std::shared_ptr<jadefx::TreeItem> root_;
    std::shared_ptr<jadefx::Label> chosen_;
    std::shared_ptr<jadefx::Button> refresh_;
    std::shared_ptr<jadefx::Button> apply_;
    // Each row's toggle, by row index, and each group's, by GUID.
    std::vector<std::shared_ptr<SideToggle>> row_toggles_;
    std::map<std::string, std::shared_ptr<SideToggle>> group_toggles_;
    // The GUID each tree item is about.
    std::map<const jadefx::TreeItem*, std::string> items_;
    bool apply_enabled_ = true;
};

}  // namespace ide
