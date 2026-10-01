#include "FindBar.hpp"

#include "IdeIcons.hpp"
#include "NodeClasses.hpp"

#include <algorithm>
#include <utility>

namespace ide {
namespace {

// A button's square, the gap between the toggles in a field, and their inset from its right edge.
constexpr double kButton = 22;
constexpr double kToggle = 20;
constexpr double kToggleGap = 1;
constexpr double kToggleInset = 3;
// "12 of 345" fits, so the buttons after it do not move while typing.
constexpr double kCountWidth = 76;
constexpr double kRowGap = 2;

#if defined(__APPLE__)
constexpr const char* kToggleKeys = "Cmd+Alt+";
constexpr const char* kReplaceAllKey = "Cmd+Enter";
#else
constexpr const char* kToggleKeys = "Alt+";
constexpr const char* kReplaceAllKey = "Ctrl+Enter";
#endif

// The find and replace text. Enter and Tab are not the field's: the bar moves
// between matches or replaces on Enter, and Tab moves between its two fields.
// Neither is Cmd+Alt with a key, which flips a toggle and would otherwise be
// taken as Cmd+C.
class SearchField : public jadefx::TextField {
protected:
    void handleKey(jadefx::KeyEvent& event) override {
        if (event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter || event.key == jadefx::Key::Tab ||
            (event.alt && event.shortcut())) {
            return;
        }
        TextField::handleKey(event);
    }
};

}  // namespace

// Laid out like VS Code's find widget. Colors are the theme's; see resources/themes/light.css.
const char* const kFindStylesheet = R"CSS(
.find-bar {
    background-color: var(--ide-find-bar-color);
    border-width: 0 1px 1px 1px;
    border-style: solid;
    border-color: var(--ide-find-bar-border-color);
    border-radius: 0 0 6px 6px;
    box-shadow: 0px 4px 14px 0px var(--ide-find-bar-shadow-color);
    padding: 4px 6px 4px 2px;
    spacing: 2px;
}
.search-field {
    background-color: var(--ide-field-color);
    color: var(--ide-search-field-text-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--ide-search-field-border-color);
    border-radius: 2px;
    font-size: 13px;
    --outline-color: rgba(0, 0, 0, 0);
}
.search-field:focus {
    border-color: var(--ide-search-field-focus-color);
}
.search-field.invalid {
    border-color: var(--ide-search-field-invalid-color);
}
.find-button {
    border-width: 1px;
    border-style: solid;
    border-color: rgba(0, 0, 0, 0);
    border-radius: 3px;
    color: var(--ide-find-button-text-color);
    font-size: 12px;
}
.find-button image-view {
    image-color: currentColor;
}
.find-button:hover {
    background-color: var(--ide-find-button-hover-color);
}
.find-button:active {
    background-color: var(--ide-find-button-pressed-color);
}
.find-button:checked {
    background-color: var(--ide-find-button-checked-color);
    border-color: var(--ide-find-button-checked-border-color);
}
.find-button:disabled {
    background-color: rgba(0, 0, 0, 0);
    opacity: 0.4;
}
.find-count {
    color: var(--ide-search-status-color);
    font-size: 12px;
    padding: 0 4px;
}
.find-count.empty {
    color: var(--ide-search-status-error-color);
}
)CSS";

FindChord find_chord(const jadefx::KeyEvent& event) {
    if (!event.pressed || event.repeat || !event.shortcut()) {
        return FindChord::None;
    }
    if (event.key == jadefx::Key::F && event.shift && !event.alt) {
        return FindChord::FindInScripts;
    }
    if (event.key == jadefx::Key::H && event.shift && !event.alt) {
        return FindChord::ReplaceInScripts;
    }
    if (event.key == jadefx::Key::F && !event.shift) {
#if defined(__APPLE__)
        return event.alt ? FindChord::Replace : FindChord::Find;
#else
        return event.alt ? FindChord::None : FindChord::Find;
#endif
    }
#if !defined(__APPLE__)
    if (event.key == jadefx::Key::H && !event.shift && !event.alt) {
        return FindChord::Replace;
    }
#endif
    return FindChord::None;
}

FindButton::FindButton(const std::string& icon, const std::string& text, const std::string& tip, bool toggle)
    : Label(""), toggle_(toggle) {
    getClassList().add("find-button");
    setAlignment(jadefx::Pos::Center);
    // A click leaves the focus in the field, so typing carries on.
    setFocusTraversable(false);
    setCursor(jadefx::Cursor::Pointer);
    setMinSize(kButton, kButton);
    setPrefSize(kButton, kButton);
    setMaxSize(kButton, kButton);
    std::shared_ptr<jadefx::ImageView> view = icon.empty() ? nullptr : icon_graphic(icon);
    if (view) {
        setGraphic(std::move(view));
    } else {
        setText(text);
    }
    if (!tip.empty()) {
        jadefx::Tooltip::install(this, jadefx::make<jadefx::Tooltip>(tip));
    }
    setOnMouseClicked([this](const jadefx::MouseEvent& event) {
        if (event.button == 0) {
            fire();
        }
    });
}

void FindButton::fire() {
    if (isDisabled()) {
        return;
    }
    if (toggle_) {
        setChecked(!checked_);
    }
    if (action_) {
        action_();
    }
}

void FindButton::setChecked(bool checked) {
    checked_ = checked;
    setPseudoState("checked", checked);
}

SearchInput::SearchInput(std::string prompt) {
    getClassList().add("search-input");
    setAlignment(jadefx::Pos::TopLeft);
    field_ = jadefx::make<SearchField>();
    field_->getClassList().add("search-field");
    field_->setPromptText(std::move(prompt));
    field_->setStyle("width: 100%; padding: 3px 6px 3px 6px;");
    getChildren().add(field_);
}

void SearchInput::addToggle(const std::shared_ptr<FindButton>& toggle) {
    toggle->setMinSize(kToggle, kToggle);
    toggle->setPrefSize(kToggle, kToggle);
    toggle->setMaxSize(kToggle, kToggle);
    toggles_.push_back(toggle);
    // After the field, so a toggle draws over it and is hit first.
    getChildren().add(toggle);
    // The text stops short of the toggles.
    const double room = kToggleInset + static_cast<double>(toggles_.size()) * (kToggle + kToggleGap) + 2;
    field_->setStyle("width: 100%; padding: 3px " + std::to_string(static_cast<int>(room)) + "px 3px 6px;");
}

void SearchInput::setInvalid(bool invalid) {
    if (invalid_ == invalid) {
        return;
    }
    invalid_ = invalid;
    set_class(*field_, "invalid", invalid);
}

void SearchInput::focusAll() {
    field_->requestFocus();
    field_->selectAll();
}

void SearchInput::layoutChildren() {
    StackPane::layoutChildren();
    double x = field_->getX() + field_->getWidth() - kToggleInset;
    const double y = field_->getY() + (field_->getHeight() - kToggle) * 0.5;
    for (auto it = toggles_.rbegin(); it != toggles_.rend(); ++it) {
        x -= kToggle;
        (*it)->performLayout(x, y, kToggle, kToggle);
        x -= kToggleGap;
    }
}

void SearchToggles::attach(SearchInput& input, const std::function<void()>& changed) {
    const std::string keys = kToggleKeys;
    match_case = jadefx::make<FindButton>("", "Aa", "Match Case (" + keys + "C)", true);
    whole_word = jadefx::make<FindButton>("", "ab", "Match Whole Word (" + keys + "W)", true);
    regex = jadefx::make<FindButton>("", ".*", "Use Regular Expression (" + keys + "R)", true);
    for (const std::shared_ptr<FindButton>& toggle : {match_case, whole_word, regex}) {
        toggle->setOnAction(changed);
        input.addToggle(toggle);
    }
}

SearchQuery SearchToggles::query(const std::string& pattern) const {
    SearchQuery query;
    query.pattern = pattern;
    query.match_case = match_case && match_case->isChecked();
    query.whole_word = whole_word && whole_word->isChecked();
    query.regex = regex && regex->isChecked();
    return query;
}

bool SearchToggles::handleKey(const jadefx::KeyEvent& event) {
    if (!event.pressed || event.shift || !event.alt || !match_case) {
        return false;
    }
#if defined(__APPLE__)
    // Alt alone types a character on macOS.
    if (!event.meta) {
        return false;
    }
#else
    if (event.shortcut()) {
        return false;
    }
#endif
    if (event.key == jadefx::Key::C) {
        match_case->fire();
    } else if (event.key == jadefx::Key::W) {
        whole_word->fire();
    } else if (event.key == jadefx::Key::R) {
        regex->fire();
    } else {
        return false;
    }
    return true;
}

FindBar::FindBar(Actions actions) : actions_(std::move(actions)) {
    getClassList().add("find-bar");
    setStylesheet(kFindStylesheet);
    setAlignment(jadefx::Pos::TopLeft);

    chevron_ = jadefx::make<FindButton>("FindCollapsed.png", ">", "Toggle Replace");
    chevron_->setMinSize(16, kButton);
    chevron_->setPrefSize(16, kButton);
    chevron_->setMaxSize(16, 100000);
    chevron_->setOnAction([this] { setReplaceShown(!replace_shown_); });

    find_ = jadefx::make<SearchInput>("Find");
    find_->setStyle("width: 100%;");
    toggles_.attach(*find_, [this] { notify_changed(); });
    count_ = jadefx::make<jadefx::Label>("No results");
    count_->getClassList().add("find-count");
    count_->setAlignment(jadefx::Pos::CenterLeft);
    count_->setMouseTransparent(true);
    count_->setMinSize(kCountWidth, kButton);
    count_->setPrefSize(kCountWidth, kButton);
    count_->setMaxSize(kCountWidth, kButton);
    auto previous = jadefx::make<FindButton>("FindPrevious.png", "^", "Previous Match (Shift+Enter)");
    previous->setOnAction([this] {
        if (actions_.step) {
            actions_.step(-1);
        }
    });
    auto next = jadefx::make<FindButton>("FindNext.png", "v", "Next Match (Enter)");
    next->setOnAction([this] {
        if (actions_.step) {
            actions_.step(1);
        }
    });
    auto close = jadefx::make<FindButton>("FindClose.png", "×", "Close (Escape)");
    close->setOnAction([this] {
        if (actions_.close) {
            actions_.close();
        }
    });
    auto find_row = jadefx::make<jadefx::HBox>();
    find_row->setSpacing(kRowGap);
    find_row->setAlignment(jadefx::Pos::CenterLeft);
    find_row->setStyle("width: 100%;");
    find_row->getChildren().add(find_);
    find_row->getChildren().add(count_);
    find_row->getChildren().add(previous);
    find_row->getChildren().add(next);
    find_row->getChildren().add(close);

    replace_ = jadefx::make<SearchInput>("Replace");
    replace_->setStyle("width: 100%;");
    replace_one_ = jadefx::make<FindButton>("Replace.png", "R", "Replace (Enter)");
    replace_one_->setOnAction([this] {
        if (actions_.replace) {
            actions_.replace();
        }
    });
    replace_every_ = jadefx::make<FindButton>("ReplaceAll.png", "A", std::string("Replace All (") + kReplaceAllKey + ")");
    replace_every_->setOnAction([this] {
        if (actions_.replace_all) {
            actions_.replace_all();
        }
    });
    // Holds the room of the count and the third button, so both fields are one width.
    auto pad = jadefx::make<jadefx::Pane>();
    pad->setMouseTransparent(true);
    const double room = kCountWidth + kButton + kRowGap;
    pad->setMinSize(room, 1);
    pad->setPrefSize(room, 1);
    pad->setMaxSize(room, 1);
    replace_row_ = jadefx::make<jadefx::HBox>();
    replace_row_->setSpacing(kRowGap);
    replace_row_->setAlignment(jadefx::Pos::CenterLeft);
    replace_row_->setStyle("width: 100%;");
    replace_row_->getChildren().add(replace_);
    replace_row_->getChildren().add(replace_one_);
    replace_row_->getChildren().add(replace_every_);
    replace_row_->getChildren().add(pad);

    rows_ = jadefx::make<jadefx::VBox>();
    rows_->setSpacing(4);
    rows_->setStyle("width: 100%;");
    // The replace row joins the rows when it is shown.
    rows_->getChildren().add(find_row);
    getChildren().add(chevron_);
    getChildren().add(rows_);
}

SearchQuery FindBar::query() const { return toggles_.query(find_->text()); }

void FindBar::setFindText(const std::string& text) {
    if (find_->text() == text) {
        return;
    }
    find_->field().setText(text);
    notify_changed();
}

void FindBar::focusFind() { find_->focusAll(); }

void FindBar::focusReplace() {
    if (!replace_shown_) {
        setReplaceShown(true);
    }
    replace_->focusAll();
}

void FindBar::setReplaceShown(bool shown) {
    if (replace_shown_ == shown || !replace_row_) {
        return;
    }
    replace_shown_ = shown;
    // A hidden row still takes room in a VBox, so the row leaves the rows and comes back.
    if (!shown) {
        const bool had_focus = replace_row_->isFocusWithin();
        const jadefx::HBox* row = replace_row_.get();
        rows_->getChildren().removeIf([row](const std::shared_ptr<jadefx::Node>& child) { return child.get() == row; });
        if (had_focus) {
            find_->focusAll();
        }
    } else {
        rows_->getChildren().add(replace_row_);
    }
    chevron_->setGraphic(icon_graphic(shown ? "FindExpanded.png" : "FindCollapsed.png"));
}

void FindBar::showCount(int current, int total, bool capped, const std::string& error) {
    std::string text;
    if (!error.empty()) {
        text = "Invalid regex";
    } else if (find_->text().empty()) {
        text = "No results";
    } else if (total == 0) {
        text = "No results";
    } else {
        const std::string of = std::to_string(total) + (capped ? "+" : "");
        text = (current >= 0 ? std::to_string(current + 1) : std::string("?")) + " of " + of;
    }
    if (count_->getText() != text) {
        count_->setText(text);
    }
    set_class(*count_, "empty", !find_->text().empty() && (total == 0 || !error.empty()));
    find_->setInvalid(!error.empty());
}

void FindBar::setReplaceEnabled(bool enabled) {
    replace_one_->setDisable(!enabled);
    replace_every_->setDisable(!enabled);
}

bool FindBar::owns(const jadefx::Node* node) const {
    for (const jadefx::Node* cursor = node; cursor != nullptr; cursor = cursor->getParent()) {
        if (cursor == this) {
            return true;
        }
    }
    return false;
}

void FindBar::poll() {
    if (find_->text() != polled_) {
        notify_changed();
    }
}

void FindBar::notify_changed() {
    polled_ = find_->text();
    if (actions_.changed) {
        actions_.changed();
    }
}

void FindBar::layoutChildren() {
    HBox::layoutChildren();
    // The chevron runs down the bar's whole left side, beside both rows.
    chevron_->performLayout(chevron_->getX(), contentTop(), chevron_->getWidth(), contentHeight());
}

void FindBar::handleKey(jadefx::KeyEvent& event) {
    // Keys bubble here from the fields.
    if (!event.pressed && !event.repeat) {
        HBox::handleKey(event);
        return;
    }
    const bool in_replace = replace_->field().isFocused();
    const bool enter = event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter;
    if (enter && in_replace) {
        if (event.shortcut()) {
            if (actions_.replace_all) {
                actions_.replace_all();
            }
        } else if (actions_.replace) {
            actions_.replace();
        }
        event.consume();
        return;
    }
    if (enter || event.key == jadefx::Key::F3 || (event.shortcut() && event.key == jadefx::Key::G)) {
        if (actions_.step) {
            actions_.step(event.shift ? -1 : 1);
        }
        event.consume();
        return;
    }
    if (event.key == jadefx::Key::Escape) {
        if (actions_.close) {
            actions_.close();
        }
        event.consume();
        return;
    }
    if (event.key == jadefx::Key::Tab && !event.shortcut()) {
        if (event.shift || in_replace) {
            find_->focusAll();
        } else if (replace_shown_) {
            replace_->focusAll();
        }
        event.consume();
        return;
    }
    const FindChord chord = find_chord(event);
    if (chord == FindChord::Find || chord == FindChord::Replace) {
        if (chord == FindChord::Replace) {
            focusReplace();
        } else {
            find_->focusAll();
        }
        event.consume();
        return;
    }
    if (toggles_.handleKey(event)) {
        event.consume();
        return;
    }
    HBox::handleKey(event);
}

}  // namespace ide
