#pragma once

#include "ColorLiterals.hpp"
#include "CompletionPopup.hpp"
#include "LuauComplete.hpp"
#include "IdePane.hpp"
#include "LuaApi.hpp"
#include "TextSearch.hpp"
#include "ide/TextUndoStack.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {
class Engine;
}

namespace ide {
class ThemeListener;
}

namespace ide {

class FindBar;
class ScriptCodeArea;

// One Luau source, docked beside the scene view. Typing writes Source back
// onto the instance. While the simulation is stopped that write also becomes
// the authored place, so Stop keeps the edit. A double-click or Edit opens it.
// Cmd+F (Ctrl+F elsewhere) opens the find bar at the top right, and Cmd+Alt+F
// (Ctrl+H) opens it with replace. While it is open every match is highlighted,
// the current one more strongly, and Escape closes it. The scroll bar marks
// every match down its left half and every problem but a hint down its right.
class IdeScriptEditor : public IdePane {
    friend class ScriptCodeArea;

public:
    // Watches its script in analysis until the editor is destroyed.
    IdeScriptEditor(engine_core::Engine& engine, std::uint32_t id);
    ~IdeScriptEditor() override;

    std::uint32_t instanceId() const { return id_; }

    // Titles the tab with the script's name and the .lua suffix.
    void setTitleText(const std::string& name);

    bool isLoaded() const { return loaded_; }
    std::string text() const;
    // The primary selection's text. Empty when nothing is selected.
    std::string selectedText() const;
    // Typed text the script's Source does not have yet. flush() writes it.
    bool hasUnflushedText() const { return dirty_; }

    void focus();
    // Puts the caret at the start of a 1-based line and scrolls to it.
    void showLine(int line);
    // Selects code points [column, column_end) of a 1-based line and scrolls to
    // them. Before the source has loaded, the selection waits for it.
    void showRange(int line, int column, int column_end);

    // The find bar. replace shows its replace row and puts the focus there when
    // there is something to find. Without it, a bar that was closed opens with
    // replace hidden. A selection on one line becomes the find text,
    // and otherwise the name under the caret does.
    void openFind(bool replace);
    void closeFind();
    bool findOpen() const;
    // node is part of the find bar, whose fields keep their own keys, undo included.
    bool findOwns(const jadefx::Node* node) const;
    FindBar* findBar() const { return find_bar_.get(); }
    // Replaces what query finds in the buffer with replacement, as one undo step,
    // on every line, or only on the 1-based line when line is above 0. The write
    // reaches Source the way typing does. Returns how many matches it replaced.
    int replaceMatches(const SearchQuery& query, const std::string& replacement, int line = 0);
    // Writes the buffer to the instance. While stopped, captures the place.
    void flush();
    // The document stack Ctrl/Cmd-Z edits. Null until the shell binds one.
    void bindUndo(TextUndoStack* stack);
    // Copies the stack into the buffer after InputRouter has undone or redone it.
    void applyUndoText();
    // After Stop restores the place, put this buffer back when it differs.
    void reapply();

protected:
    void layoutChildren() override;
    void onOpen() override;
    void onClose() override;
    // Leaving a scene closes the color picker, whose key hook is on that scene.
    void sceneChanged(jadefx::Scene* previous) override;

private:
    struct Commit;

    bool read_source(std::string& text, std::string& name, bool& alive, std::uint32_t* world = nullptr) const;
    void show_source(std::string text);
    void load();
    void paint();
    void refresh_marks();
    // Every find match and problem, as bands on the scroll bar.
    void refresh_scroll_marks();
    void note_text();
    void push(const std::string& text);
    void refresh_completion(bool force);
    // Shows Luau's list for the last keystroke, once it has arrived.
    void take_luau_list();
    // Before an accept reads the popup: waits for that list and shows it, so
    // what is accepted matches the text as it is now.
    void settle_luau_list();
    void dismiss_completion();
    void accept_completion(bool parentheses);
    void move_completion(int delta);
    void place_completion();
    bool completion_open() const;
    bool completion_commits_name();
    bool completion_commits_quote(char quote, bool unclosed_only = true);
    // Enter and Tab accept when the highlighted name would change the text.
    // A finished name keeps those keys, so a newline or indent still works.
    bool completion_key_accepts();
    std::vector<engine_core::LuaNode> world() const;
    // A swatch after each Color3 literal. A click on one opens the color picker on it.
    void refresh_color_swatches();
    void open_color_picker(std::size_t index);
    // Ends the picker: keep writes one undo step for the whole session, and
    // otherwise the literal goes back to how it was.
    void close_color_picker(bool keep);
    // scene is where the picker's key hook and popup are: this editor's scene,
    // the one it just left, or null when that scene is going away.
    void close_color_picker(bool keep, jadefx::Scene* scene);
    void write_color(const engine_core::Color3& color);
    // Finds the bar's query in text, the buffer, again. paint calls it, so the
    // matches always belong to the text on screen.
    void refresh_find(const std::string& text);
    // The find text or a toggle changed: select the first match from the selection on.
    void find_changed();
    void step_find(int direction);
    void select_find(std::size_t index);
    void replace_find();
    void replace_find_all();
    // The match the selection covers exactly, or -1.
    int current_find() const;
    void show_find_count();
    void place_find_bar();
    // Replaces matches, which search found in text, with one edit and one undo step.
    int apply_replacements(const TextSearch& search, const std::string& text, const std::vector<TextMatch>& matches,
                           const std::string& replacement);

    engine_core::Engine& engine_;
    std::uint32_t id_ = 0;
    std::shared_ptr<jadefx::CodeArea> area_;
    std::shared_ptr<jadefx::Label> status_;
    CompletionPopup completion_;
    // Luau's list for the last keystroke, on its way.
    std::optional<PendingCompletion> luau_list_;
    std::shared_ptr<Commit> commit_;
    std::string shown_name_;
    // DataModel::authored_revision and tree_revision when reapply last read
    // the script, and tree_revision when play last read its name. Neither
    // reads again until one moves.
    std::uint64_t seen_authored_ = ~std::uint64_t{0};
    std::uint64_t seen_tree_ = ~std::uint64_t{0};
    std::uint64_t seen_title_tree_ = ~std::uint64_t{0};
    TextUndoStack* undo_stack_ = nullptr;
    bool mute_undo_ = false;
    // A replace is writing the buffer. It is not typing, so it does not open completion.
    bool replacing_ = false;
    bool loading_ = false;
    bool loaded_ = false;
    bool missing_ = false;
    bool dirty_ = false;
    // world_generation the buffer last matched. Stop bumps it.
    std::uint32_t world_ = 0;
    std::chrono::steady_clock::time_point dirty_at_{};

    // The literal the picker is editing, as it was when the picker opened.
    struct ColorEdit {
        Color3Literal literal;
        std::string original;
        int key_hook = 0;
    };
    std::vector<Color3Literal> color_literals_;
    std::vector<std::shared_ptr<jadefx::Pane>> color_swatches_;
    std::shared_ptr<jadefx::ColorChooser> color_chooser_;
    std::unique_ptr<ColorEdit> color_edit_;

    struct PendingRange {
        int line = 0;
        int column = 0;
        int column_end = 0;
    };
    std::shared_ptr<FindBar> find_bar_;
    // What the bar's query finds in the buffer, and at most kFindLimit of it.
    std::vector<TextMatch> find_matches_;
    bool find_capped_ = false;
    std::string find_error_;
    // The current match paint last drew, so a selection that moves only repaints when it changes.
    int painted_find_ = -1;
    std::optional<PendingRange> pending_range_;
    // Defines the syntax colors again when the theme changes.
    std::unique_ptr<ThemeListener> theme_listener_;
};

}  // namespace ide
