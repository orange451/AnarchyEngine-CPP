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
// the list. Clicking a problem, or Enter on it, opens its script there. Down
// from the filter walks into the list, Enter there opens the first problem,
// and Escape clears it. The list follows the checker as it publishes and the
// tree as scripts and their folders move or are renamed, rebuilt at most once
// a frame, and at most every 250 ms while a long check runs. While a playtest
// runs the checker rests, and the pane says so and keeps the last results.
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
    // The filter, the toggles, and the summary. Hidden while analysis is off.
    bool headerShown() const;
    // How many times the rows have been rebuilt. For tests.
    int rebuilds() const { return rebuilds_; }

    // Gathers the published problems and rebuilds now, instead of on a later layout.
    void refresh();
    // What a layout pass does before it lays out: follows the checker, the
    // tree, the filter, play, and analysis being on, and keeps the title, the
    // notices, and the rows up to date. The studio calls it once a frame too,
    // so the title keeps counting while the pane is a tab not showing, which
    // is never laid out. now is the scene's time in seconds. Cheap when
    // nothing changed, so a frame that runs it twice does the work once.
    void tick(double now);
    // Opens what a row points at. False for an item that is not a row of this pane.
    bool openRow(const jadefx::TreeItem* item);

protected:
    void layoutChildren() override;
    void filterKey(jadefx::KeyEvent& event) override;
    void handleKey(jadefx::KeyEvent& event) override;
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
    // Rebuilds now when the filter or a toggle moved since the last rebuild.
    void sync_filter();
    // A tree change alone: the cached scripts' names and paths read again, and
    // the rows rebuilt. Scripts that left the place, or went under Core, drop.
    void refresh_paths();
    void update_toggles();
    // Each toggle as wide as its word and count.
    void fit_toggles();
    // Keeps the play note and the analysis-off notice, in that order, as the
    // only children of notices_ that apply right now. While analysis is off the
    // header and the summary go too: there is nothing to filter or count.
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
    std::shared_ptr<jadefx::VBox> header_;
    std::shared_ptr<jadefx::VBox> top_;
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
    // DataModel::tree_revision when the names and paths were last read.
    std::uint64_t seen_tree_ = 0;
    // The scene time of the latest tick, and of the last rebuild, which a
    // long check batch spaces out.
    double now_ = 0;
    double built_at_ = 0;
};

}  // namespace ide
