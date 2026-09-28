#pragma once

#include "TextSearch.hpp"

#include "jadefx/jadefx.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ide {

// A square button with an icon from resources/icons, or a few letters, as in VS
// Code's find widget. A click runs its action and leaves the focus where it was.
// A toggle flips its checked state first and shows it with a tinted box.
class FindButton : public jadefx::Label {
public:
    // icon is a file under resources/icons. When it is missing, or text is given
    // instead of an icon, the button shows text.
    FindButton(const std::string& icon, const std::string& text, const std::string& tip, bool toggle = false);

    void setOnAction(std::function<void()> action) { action_ = std::move(action); }
    void fire();
    bool isChecked() const { return checked_; }
    void setChecked(bool checked);

private:
    std::function<void()> action_;
    bool toggle_ = false;
    bool checked_ = false;
};

// A find or replace field. Toggles sit inside its right end, the way VS Code puts
// Match Case, Match Whole Word, and Use Regular Expression in the find field.
// Keys the field does not use bubble up to the bar that holds it.
class SearchInput : public jadefx::StackPane {
public:
    explicit SearchInput(std::string prompt);

    const char* getElementType() const override { return "search-input"; }

    jadefx::TextField& field() const { return *field_; }
    const std::string& text() const { return field_->getText(); }
    void addToggle(const std::shared_ptr<FindButton>& toggle);
    // A red border, for a regex that does not compile.
    void setInvalid(bool invalid);
    // Focuses the field and selects its text.
    void focusAll();

protected:
    void layoutChildren() override;

private:
    std::shared_ptr<jadefx::TextField> field_;
    std::vector<std::shared_ptr<FindButton>> toggles_;
    bool invalid_ = false;
};

// The three toggles of a find field, and the query they make with its text.
struct SearchToggles {
    std::shared_ptr<FindButton> match_case;
    std::shared_ptr<FindButton> whole_word;
    std::shared_ptr<FindButton> regex;

    // Adds the three to input. changed runs after any of them flips.
    void attach(SearchInput& input, const std::function<void()>& changed);
    SearchQuery query(const std::string& pattern) const;
    // Alt+C, Alt+W, and Alt+R (Cmd+Alt on macOS) flip them. True when the key was one.
    bool handleKey(const jadefx::KeyEvent& event);
};

// The find shortcuts, as in VS Code. Cmd on macOS and Ctrl elsewhere: F finds in
// the script, and Shift+F in every script. Replace is Cmd+Alt+F on macOS and
// Ctrl+H elsewhere, and in every script Shift+H.
enum class FindChord { None, Find, Replace, FindInScripts, ReplaceInScripts };
FindChord find_chord(const jadefx::KeyEvent& event);

// The CSS the find bar and the Search pane share. Each sets it on itself.
extern const char* const kFindStylesheet;

// The find and replace widget that hangs from a script editor's top right, as in
// VS Code. The first row is the find field with its toggles, where the current
// match is among all of them, previous, next, and close. The second row, which
// starts hidden and the chevron on the left shows and hides, is the replace
// field with Replace and Replace All. Enter goes to the next match and Shift+Enter to the previous one.
// In the replace field Enter replaces and Cmd+Enter (Ctrl+Enter elsewhere)
// replaces all. Escape closes the bar.
class FindBar : public jadefx::HBox {
public:
    struct Actions {
        // The find text or a toggle changed.
        std::function<void()> changed;
        // 1 for the next match, -1 for the previous one.
        std::function<void(int)> step;
        std::function<void()> replace;
        std::function<void()> replace_all;
        std::function<void()> close;
    };

    explicit FindBar(Actions actions);

    const char* getElementType() const override { return "find-bar"; }

    SearchQuery query() const;
    std::string replacement() const { return replace_->text(); }
    void setFindText(const std::string& text);
    void focusFind();
    void focusReplace();
    bool replaceShown() const { return replace_shown_; }
    void setReplaceShown(bool shown);
    // "2 of 6", "? of 6" when the selection is not a match, or "No results".
    // current is 0-based, or -1. error is a regex that did not compile.
    void showCount(int current, int total, bool capped, const std::string& error);
    // A script that cannot change keeps the replace buttons dimmed.
    void setReplaceEnabled(bool enabled);
    // node is one of the bar's fields or buttons.
    bool owns(const jadefx::Node* node) const;
    // Reads the find field and runs changed when its text is new since the last read.
    // The editor calls this from layout, so typing, paste, and cut all count.
    void poll();
    SearchInput& findInput() const { return *find_; }
    SearchInput& replaceInput() const { return *replace_; }

    // The bar's widest, and its gap from the editor's right edge, where the scroll bar is.
    static constexpr double kMaxWidth = 440;
    static constexpr double kRightGap = 14;

protected:
    void layoutChildren() override;
    void handleKey(jadefx::KeyEvent& event) override;

private:
    void notify_changed();

    Actions actions_;
    std::shared_ptr<FindButton> chevron_;
    std::shared_ptr<SearchInput> find_;
    std::shared_ptr<SearchInput> replace_;
    SearchToggles toggles_;
    std::shared_ptr<jadefx::Label> count_;
    std::shared_ptr<jadefx::VBox> rows_;
    std::shared_ptr<jadefx::HBox> replace_row_;
    std::shared_ptr<FindButton> replace_one_;
    std::shared_ptr<FindButton> replace_every_;
    std::string polled_;
    bool replace_shown_ = false;
};

}  // namespace ide
