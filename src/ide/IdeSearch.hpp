#pragma once

#include "FindBar.hpp"
#include "IdePane.hpp"
#include "TextSearch.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine_core {
class DataModel;
class Engine;
}  // namespace engine_core

namespace ide {

// What the Search pane needs from the studio. An open editor's text is newer
// than its script's Source, so the pane reads and replaces through it.
struct SearchHost {
    // The text of the editor that has id open, or nothing when none has.
    std::function<std::optional<std::string>(std::uint32_t id)> editor_text;
    // Replaces in the editor that has id open, as one undo step there. line is
    // 1-based, or 0 for every line. Returns how many, or -1 when no editor has it.
    std::function<int(std::uint32_t id, const SearchQuery& query, const std::string& replacement, int line)>
        replace_in_editor;
    // Opens the script and selects code points [column, column_end) of the 1-based line.
    std::function<void(std::uint32_t id, int line, int column, int column_end)> open;
};

// One script's matches, a line at a time, in the order the explorer lists scripts.
struct ScriptHits {
    struct Line {
        // 1-based.
        int line = 0;
        std::string text;
        // Each match as code points [first, second) of text.
        std::vector<std::pair<int, int>> ranges;

        bool operator==(const Line& other) const {
            return line == other.line && text == other.text && ranges == other.ranges;
        }
    };

    std::uint32_t id = 0;
    std::string name;
    std::string class_name;
    // Its parent, from game down, such as game.Workspace.Folder.
    std::string path;
    std::vector<Line> lines;
    int matches = 0;

    bool operator==(const ScriptHits& other) const {
        return id == other.id && name == other.name && class_name == other.class_name && path == other.path &&
               lines == other.lines;
    }
};

// Find and replace across every Script and ModuleScript under game, in a pane
// that docks like any other. Cmd+Shift+F (Ctrl+Shift+F elsewhere) opens it, and
// Cmd+Shift+H with replace. The top is a find field with the editor's three
// toggles and, under the chevron, a replace field with Replace All. Below, each
// script with a match is a row with its name, where it is, and how many matches
// it has. Its lines with a match are rows under it, with the matches marked.
// Clicking a line, or Enter on it, opens the script there with the match
// selected. While replace is shown, a button on the row under the pointer
// replaces that line's matches, or the whole script's. The results follow edits
// as they happen, including text an editor has not written to Source yet.
class IdeSearch : public IdePane {
public:
    // At most this many matches are listed, across every script.
    static constexpr int kMatchLimit = 5000;

    IdeSearch(engine_core::Engine& engine, SearchHost host);

    SearchInput& findInput() const { return *find_; }
    SearchInput& replaceInput() const { return *replace_; }
    jadefx::TreeView& tree() const { return *tree_; }
    const std::vector<ScriptHits>& results() const { return results_; }
    // "3 results in 2 scripts", "No results", and so on.
    std::string summary() const;

    void setFindText(const std::string& text);
    void focusFind();
    // Shows the replace row and focuses it.
    void focusReplace();
    bool replaceShown() const { return replace_shown_; }
    void setReplaceShown(bool shown);
    // Searches every script now instead of on a later layout.
    void refresh();
    // Replaces every match in every script. With ask, a dialog confirms it first.
    void replaceAll(bool ask = true);
    // Replaces the matches of one script: on its 1-based line, or all of them for 0.
    void replaceIn(std::uint32_t id, int line);
    // Opens the script a result row points at. False for a row that is not a result.
    bool openRow(const jadefx::TreeItem* item);

protected:
    void layoutChildren() override;
    void handleKey(jadefx::KeyEvent& event) override;
    void onOpen() override { focusFind(); }

private:
    struct Target {
        std::uint32_t id = 0;
        // 0 for a script's row.
        int line = 0;
        int column = 0;
        int column_end = 0;
    };

    SearchQuery query() const;
    void rebuild();
    void show_summary(const std::string& error);
    void clicked(const jadefx::MouseEvent& event);
    // Writes replacements: through open editors, and into Source for the rest,
    // as one place undo step. id 0 is every script in the results.
    void replace(std::uint32_t id, int line);
    void accessory_clicked();
    const Target* target_of(const jadefx::TreeItem* item) const;

    engine_core::Engine& engine_;
    SearchHost host_;
    std::shared_ptr<FindButton> chevron_;
    std::shared_ptr<SearchInput> find_;
    std::shared_ptr<SearchInput> replace_;
    SearchToggles toggles_;
    std::shared_ptr<FindButton> replace_every_;
    std::shared_ptr<jadefx::VBox> rows_;
    std::shared_ptr<jadefx::HBox> replace_row_;
    std::shared_ptr<jadefx::Label> summary_;
    std::shared_ptr<jadefx::TreeView> tree_;
    std::shared_ptr<jadefx::TreeItem> root_;
    std::shared_ptr<FindButton> row_replace_;
    std::shared_ptr<jadefx::Tooltip> row_replace_tip_;
    std::shared_ptr<jadefx::Alert> alert_;
    std::unordered_map<const jadefx::TreeItem*, Target> targets_;
    // Scripts whose rows were closed, kept closed when the results change.
    std::unordered_set<std::uint32_t> collapsed_;
    std::vector<ScriptHits> results_;
    bool capped_ = false;
    std::string error_;
    // What the last search looked for, and when it ran.
    SearchQuery searched_;
    bool searched_once_ = false;
    double searched_at_ = 0;
    // The query the field and toggles ask for now, and when it last changed. The
    // search waits a moment for typing to pause.
    SearchQuery pending_;
    double changed_at_ = 0;
    // The kind of row the hover button last showed: a script's, or a line's.
    bool hover_script_ = false;
    bool replace_shown_ = false;
};

}  // namespace ide
