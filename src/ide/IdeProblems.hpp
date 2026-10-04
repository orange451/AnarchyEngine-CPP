#pragma once

#include "ChangeFlag.hpp"
#include "FindBar.hpp"
#include "IdePane.hpp"
#include "ProblemsModel.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace engine_core {
class Engine;
}

namespace ide {

struct ProblemsHost {
    // Opens the script and selects code points [column, column_end) of the 1-based line.
    std::function<void(std::uint32_t id, int line, int column, int column_end)> open;
};

// Every error, warning, and info message the script checker has found, in a
// pane that docks like any other. Each script with a problem is a row with its
// name, where it is, and how many errors and warnings it has; its problems are
// rows under it. A filter field and Errors, Warnings, and Info toggles narrow
// the list. Clicking a problem, or Enter on it, opens its script there. The
// list follows the checker as it publishes, rebuilt at most once a frame.
// While a playtest runs the checker rests, and the pane says so and keeps the
// last results.
class IdeProblems : public IdePane {
public:
    IdeProblems(engine_core::Engine& engine, ProblemsHost host);
    ~IdeProblems() override;

    SearchInput& filterInput() const { return *filter_; }
    FindButton& errorsToggle() const { return *errors_; }
    FindButton& warningsToggle() const { return *warnings_; }
    FindButton& infoToggle() const { return *info_; }
    jadefx::TreeView& tree() const { return *tree_; }
    const ProblemList& list() const { return list_; }
    std::string summary() const;
    bool playNoteShown() const;
    bool offNoticeShown() const;
    // How many times the rows have been rebuilt. For tests.
    int rebuilds() const { return rebuilds_; }

    // Gathers the published problems and rebuilds now, instead of on a later layout.
    void refresh();
    // Opens what a row points at. False for an item that is not a row of this pane.
    bool openRow(const jadefx::TreeItem* item);

protected:
    void layoutChildren() override;
    void onOpen() override { filter_->focusAll(); }

private:
    struct Target {
        std::uint32_t id = 0;
        int line = 0;
        int column = 0;
        int column_end = 0;
    };

    ProblemFilter filter() const;
    void rebuild();
    void update_toggles();
    // Keeps the play note and the analysis-off notice, in that order, as the
    // only children of notices_ that apply right now.
    void update_notices();
    void clicked(const jadefx::MouseEvent& event);

    engine_core::Engine& engine_;
    ProblemsHost host_;
    std::uint64_t hook_ = 0;
    ChangeFlag changed_;
    std::shared_ptr<SearchInput> filter_;
    std::shared_ptr<FindButton> errors_;
    std::shared_ptr<FindButton> warnings_;
    std::shared_ptr<FindButton> info_;
    std::shared_ptr<jadefx::Label> summary_;
    std::shared_ptr<jadefx::Label> play_note_;
    std::shared_ptr<jadefx::Label> off_notice_;
    std::shared_ptr<jadefx::VBox> notices_;
    std::shared_ptr<jadefx::TreeView> tree_;
    std::shared_ptr<jadefx::TreeItem> root_;
    std::unordered_map<const jadefx::TreeItem*, Target> targets_;
    std::unordered_set<std::uint32_t> collapsed_;
    std::vector<ProblemSource> sources_;
    ProblemList list_;
    // What the last rebuild showed, to rebuild only when something changed.
    ProblemFilter shown_filter_;
    bool shown_playing_ = false;
    bool shown_enabled_ = true;
    bool built_once_ = false;
    int rebuilds_ = 0;
};

}  // namespace ide
