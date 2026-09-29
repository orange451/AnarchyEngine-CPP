#include "IdeScriptEditor.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "FindBar.hpp"
#include "IdeTheme.hpp"
#include "LuaSource.hpp"
#include "LuauComplete.hpp"
#include "LuauHighlight.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptMarks.hpp"
#include "ScriptPairs.hpp"
#include "TextWrap.hpp"
#include "Utf8.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

namespace ide {
namespace {

constexpr std::chrono::milliseconds kSaveDelay(50);
constexpr std::chrono::milliseconds kLockWait(5);
// The most matches the find bar counts and highlights, as in VS Code.
constexpr std::size_t kFindLimit = 19999;

struct EditorFont {
    EditorFont() {
        const char* paths[] = {
            "/System/Library/Fonts/Menlo.ttc",
            "/System/Library/Fonts/Supplemental/Courier New.ttf",
            "C:/Windows/Fonts/consola.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        };
        for (const char* path : paths) {
            if (jadefx::Font::loadFile("Editor Mono", path)) {
                return;
            }
        }
    }
};

const EditorFont& editor_font() {
    static const EditorFont font;
    return font;
}

// The problem banner under the editor. Its colors are added for each severity.
constexpr const char* kBannerStyle = "font-size: 12px; ";

void define_styles(jadefx::StyleClassedTextArea& area) {
    jadefx::TextStyle keyword;
    keyword.hasFill = true;
    keyword.fill = theme_color("--ide-syntax-keyword-color");
    keyword.bold = true;
    area.defineStyleClass("keyword", keyword);

    jadefx::TextStyle builtin;
    builtin.hasFill = true;
    builtin.fill = theme_color("--ide-syntax-builtin-color");
    builtin.bold = true;
    area.defineStyleClass("builtin", builtin);

    // Instance, Vector3, Enum: types, so they read apart from functions like print.
    jadefx::TextStyle datatype;
    datatype.hasFill = true;
    datatype.fill = theme_color("--ide-syntax-datatype-color");
    datatype.bold = true;
    area.defineStyleClass("datatype", datatype);

    jadefx::TextStyle comment;
    comment.hasFill = true;
    comment.fill = theme_color("--ide-syntax-comment-color");
    area.defineStyleClass("comment", comment);

    jadefx::TextStyle stringStyle;
    stringStyle.hasFill = true;
    stringStyle.fill = theme_color("--ide-syntax-string-color");
    area.defineStyleClass("string", stringStyle);

    jadefx::TextStyle number;
    number.hasFill = true;
    number.fill = theme_color("--ide-syntax-number-color");
    area.defineStyleClass("number", number);

    // Every match of the find bar, and the one it is on.
    jadefx::TextStyle match;
    match.hasBackground = true;
    match.background = theme_color("--ide-find-match-color");
    area.defineStyleClass("find-match", match);

    jadefx::TextStyle current;
    current.hasBackground = true;
    current.background = theme_color("--ide-find-current-color");
    area.defineStyleClass("find-current", current);
}

std::string lua_title(const std::string& name) {
    if (name.empty()) {
        return "Script.lua";
    }
    return name + ".lua";
}

bool NameChar(char32_t code) {
    return (code >= U'A' && code <= U'Z') || (code >= U'a' && code <= U'z') || code == U'_' ||
           (code >= U'0' && code <= U'9') || code >= 0x80;
}

// The name the caret at column touches in line, as code points [start, end).
// Empty when the caret is between two characters that are not part of a name.
jadefx::IndexRange name_around(const std::u32string& line, int column) {
    const int length = static_cast<int>(line.size());
    int start = std::clamp(column, 0, length);
    int end = start;
    while (start > 0 && NameChar(line[static_cast<std::size_t>(start - 1)])) {
        --start;
    }
    while (end < length && NameChar(line[static_cast<std::size_t>(end)])) {
        ++end;
    }
    return jadefx::IndexRange{start, end};
}

}  // namespace

class ScriptCodeArea : public jadefx::CodeArea {
public:
    IdeScriptEditor* editor = nullptr;

    void handleKey(jadefx::KeyEvent& event) override;
    void handleText(jadefx::TextEvent& event) override;
    bool applyPair(char unit);
    bool applyEnter();
    bool applyComment();
    void handleMousePressed(const jadefx::MouseEvent& event) override;
    void handleMouseMoved(const jadefx::MouseEvent& event) override;
    void handleScroll(jadefx::ScrollEvent& event) override;

    void tickHover();
    void dismissHover();
    // Replaces the squiggles. Returns true when the set changed.
    bool setProblems(std::vector<ScriptMark> marks);
    const std::vector<ScriptMark>& problems() const { return problems_; }
    const ScriptMark* problemAt(int index) const;

private:
    void armHover(int index, double x, double y);
    void showHover();
    void showTip(const std::string& title, const std::string& detail, const std::string& summary, const jadefx::Color& titleFill);
    bool sameWord(int index) const;
    bool sameProblem(int index) const;

    std::vector<ScriptMark> problems_;

    std::shared_ptr<jadefx::VBox> tip_;
    int hover_index_ = -1;
    int hover_begin_ = -1;
    int hover_end_ = -1;
    double anchor_x_ = 0;
    double anchor_y_ = 0;
    std::chrono::steady_clock::time_point hover_since_{};
    bool hover_waiting_ = false;
    // Luau's hover for the name under the pointer, on its way.
    std::optional<PendingHover> hover_answer_;
};

struct IdeScriptEditor::Commit {
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<std::uint64_t> acked{0};
    engine_core::InstanceId id = 0;
};

IdeScriptEditor::IdeScriptEditor(engine_core::Engine& engine, std::uint32_t id)
    : IdePane("Script.lua", true), engine_(engine), id_(id), commit_(std::make_shared<Commit>()) {
    setIconFile("Script.png");
    // Checked against the tree as it is now, not as it was at its last check.
    engine_.analysis().watch(id_);
    commit_->id = id;
    auto area = std::make_shared<ScriptCodeArea>();
    area->editor = this;
    area_ = area;
    area_->getClassList().add("ide-script");
    // Load before the area is laid out. The stylesheet asks for this family.
    (void)editor_font();
    define_styles(*area_);
    theme_listener_ = std::make_unique<ThemeListener>([this] {
        define_styles(*area_);
        refresh_scroll_marks();
    });
    area_->setOnPlainTextChange([this](const jadefx::PlainTextChange& change) {
        if (!loading_ && !mute_undo_ && undo_stack_ != nullptr) {
            if (!undo_stack_->record_change(change.position, change.removed, change.inserted)) {
                // The stack lost track of the buffer. Restart it from what is on screen so
                // an undo can never roll the script back to text it does not hold.
                undo_stack_->reset(area_->getText());
            }
        }
        note_text();
    });
    area_->setOnMouseExited([this](const jadefx::MouseEvent&) {
        if (area_) {
            static_cast<ScriptCodeArea*>(area_.get())->dismissHover();
        }
    });
    completion_.setOnAccept([this] { accept_completion(true); });

    FindBar::Actions find;
    find.changed = [this] { find_changed(); };
    find.step = [this](int direction) { step_find(direction); };
    find.replace = [this] { replace_find(); };
    find.replace_all = [this] { replace_find_all(); };
    find.close = [this] { closeFind(); };
    find_bar_ = jadefx::make<FindBar>(std::move(find));
    find_bar_->setVisible(false);

    status_ = jadefx::make<jadefx::Label>("");
    status_->setAlignment(jadefx::Pos::CenterLeft);
    status_->setMouseTransparent(true);
    status_->setVisible(false);
    status_->setPadding(jadefx::Insets{3, 8, 3, 8});
    status_->setStyle(kBannerStyle);
    status_->setMinSize(0, 0);
    status_->setPrefHeight(0);
    status_->setMaxSize(100000, 0);

    auto column = jadefx::make<jadefx::BorderPane>();
    Fill(*column);
    column->setCenter(area_);
    column->setBottom(status_);
    getChildren().add(column);
    // After the text, so it draws over it and is hit first.
    getChildren().add(find_bar_);
    load();
}

void IdeScriptEditor::setTitleText(const std::string& name) {
    shown_name_ = name;
    setTitle(lua_title(name));
}

std::string IdeScriptEditor::text() const { return area_ ? area_->getText() : std::string(); }

std::string IdeScriptEditor::selectedText() const { return area_ ? area_->selectedText() : std::string(); }

void IdeScriptEditor::focus() {
    if (area_) {
        area_->requestFocus();
    }
}

void IdeScriptEditor::showLine(int line) {
    if (!area_ || line < 1) {
        return;
    }
    const int paragraph = std::min(line, area_->paragraphCount()) - 1;
    area_->moveTo(paragraph, 0);
    area_->showPosition(area_->caretPosition());
}

void IdeScriptEditor::showRange(int line, int column, int column_end) {
    if (!area_ || line < 1) {
        return;
    }
    if (!loaded_) {
        pending_range_ = PendingRange{line, column, column_end};
        return;
    }
    const int paragraph = std::min(line, area_->paragraphCount()) - 1;
    const int length = area_->getParagraph(paragraph).length();
    const int start = area_->absolutePosition(paragraph, std::clamp(column, 0, length));
    const int end = area_->absolutePosition(paragraph, std::clamp(column_end, 0, length));
    area_->selectRange(start, std::max(start, end));
    area_->showPosition(start);
}

bool IdeScriptEditor::findOpen() const { return find_bar_ && find_bar_->isVisible(); }

bool IdeScriptEditor::findOwns(const jadefx::Node* node) const { return find_bar_ && find_bar_->owns(node); }

void IdeScriptEditor::openFind(bool replace) {
    if (!area_ || !find_bar_) {
        return;
    }
    // One line of selection, or the name under the caret, is what to find.
    std::string seed;
    if (area_->selections().size() == 1) {
        const jadefx::IndexRange range = area_->selection();
        if (!range.empty()) {
            seed = area_->getText(range.start, range.end);
            if (seed.find('\n') != std::string::npos) {
                seed.clear();
            }
        } else {
            const jadefx::TextPos at = area_->position(range.start);
            const jadefx::IndexRange name = name_around(area_->getParagraph(at.paragraph).content(), at.column);
            if (!name.empty()) {
                // Selected, so it is the match the bar starts on.
                const int start = area_->absolutePosition(at.paragraph, name.start);
                const int end = area_->absolutePosition(at.paragraph, name.end);
                seed = area_->getText(start, end);
                area_->selectRange(start, end);
            }
        }
    }
    const bool was_open = findOpen();
    find_bar_->setVisible(true);
    find_bar_->setReplaceEnabled(area_->isEditable());
    // Find opens with replace hidden. Once open, the bar keeps what the chevron chose.
    if (replace || !was_open) {
        find_bar_->setReplaceShown(replace);
    }
    if (!seed.empty()) {
        find_bar_->setFindText(seed);
    }
    if (!was_open) {
        paint();
    }
    if (replace && !find_bar_->query().pattern.empty()) {
        find_bar_->focusReplace();
    } else {
        find_bar_->focusFind();
    }
}

void IdeScriptEditor::closeFind() {
    if (!findOpen()) {
        return;
    }
    find_bar_->setVisible(false);
    find_bar_->performLayout(0, 0, 0, 0);
    if (jadefx::Scene* scene = getScene()) {
        scene->releaseFocus(find_bar_.get());
    }
    paint();
    focus();
}

void IdeScriptEditor::refresh_find(const std::string& text) {
    find_matches_.clear();
    find_capped_ = false;
    find_error_.clear();
    if (!findOpen()) {
        return;
    }
    const TextSearch search(find_bar_->query());
    find_error_ = search.error();
    if (search.ready()) {
        find_matches_ = search.find_all(text, kFindLimit + 1);
        if (find_matches_.size() > kFindLimit) {
            find_matches_.resize(kFindLimit);
            find_capped_ = true;
        }
    }
}

int IdeScriptEditor::current_find() const {
    if (!area_ || find_matches_.empty() || area_->selections().size() != 1) {
        return -1;
    }
    const jadefx::IndexRange range = area_->selection();
    auto it = std::lower_bound(find_matches_.begin(), find_matches_.end(), range.start,
                               [](const TextMatch& match, int start) { return match.start < start; });
    for (; it != find_matches_.end() && it->start == range.start; ++it) {
        if (it->end == range.end) {
            return static_cast<int>(it - find_matches_.begin());
        }
    }
    return -1;
}

void IdeScriptEditor::show_find_count() {
    if (findOpen()) {
        find_bar_->showCount(current_find(), static_cast<int>(find_matches_.size()), find_capped_, find_error_);
    }
}

void IdeScriptEditor::find_changed() {
    if (!area_) {
        return;
    }
    // As the find text grows, the match stays where the last one started.
    const int from = area_->selection().start;
    paint();
    if (find_matches_.empty()) {
        return;
    }
    std::size_t index = 0;
    for (std::size_t i = 0; i < find_matches_.size(); ++i) {
        if (find_matches_[i].start >= from) {
            index = i;
            break;
        }
    }
    select_find(index);
}

void IdeScriptEditor::select_find(std::size_t index) {
    if (!area_ || index >= find_matches_.size()) {
        return;
    }
    const TextMatch& match = find_matches_[index];
    area_->selectRange(match.start, match.end);
    area_->showPosition(match.start);
    if (painted_find_ != static_cast<int>(index)) {
        paint();
    } else {
        show_find_count();
    }
}

void IdeScriptEditor::step_find(int direction) {
    if (!area_ || find_matches_.empty()) {
        return;
    }
    const std::size_t count = find_matches_.size();
    const int current = current_find();
    std::size_t index = 0;
    if (current >= 0) {
        index = (static_cast<std::size_t>(current) + count + (direction > 0 ? 1 : count - 1)) % count;
    } else if (direction > 0) {
        // The first match after the selection, or the first of all.
        const int from = area_->selection().end;
        for (std::size_t i = 0; i < count; ++i) {
            if (find_matches_[i].start >= from) {
                index = i;
                break;
            }
        }
    } else {
        // The last match before the selection, or the last of all.
        const int before = area_->selection().start;
        index = count - 1;
        for (std::size_t i = count; i-- > 0;) {
            if (find_matches_[i].start < before) {
                index = i;
                break;
            }
        }
    }
    select_find(index);
}

void IdeScriptEditor::replace_find() {
    if (!area_ || !findOpen() || !area_->isEditable()) {
        return;
    }
    const int current = current_find();
    // The first press only goes to a match; the next replaces it, as in VS Code.
    if (current < 0) {
        step_find(1);
        return;
    }
    const TextSearch search(find_bar_->query());
    if (!search.ready()) {
        return;
    }
    const std::string text = area_->getText();
    const TextMatch match = find_matches_[static_cast<std::size_t>(current)];
    const std::string inserted = search.expand(text, match, find_bar_->replacement());
    dismiss_completion();
    replacing_ = true;
    area_->replaceText(match.start, match.end, inserted);
    replacing_ = false;
    const int after = match.start + CodePoints(inserted);
    area_->moveTo(after);
    // The edit repainted, so the matches are the new text's. An empty match
    // replaced with nothing is still there; the next one is past it.
    if (find_matches_.empty()) {
        show_find_count();
        return;
    }
    const bool stuck = match.start == match.end && inserted.empty();
    std::size_t index = 0;
    for (std::size_t i = 0; i < find_matches_.size(); ++i) {
        if (stuck ? find_matches_[i].start > after : find_matches_[i].start >= after) {
            index = i;
            break;
        }
    }
    select_find(index);
}

void IdeScriptEditor::replace_find_all() {
    if (!area_ || !findOpen() || !area_->isEditable()) {
        return;
    }
    const TextSearch search(find_bar_->query());
    if (!search.ready()) {
        return;
    }
    const std::string text = area_->getText();
    // Every match, past the limit the bar counts to.
    const std::vector<TextMatch> matches = search.find_all(text);
    if (!matches.empty()) {
        apply_replacements(search, text, matches, find_bar_->replacement());
    }
}

int IdeScriptEditor::replaceMatches(const SearchQuery& query, const std::string& replacement, int line) {
    if (!area_ || !loaded_ || missing_ || !area_->isEditable()) {
        return 0;
    }
    const TextSearch search(query);
    if (!search.ready()) {
        return 0;
    }
    const std::string text = area_->getText();
    std::vector<TextMatch> matches = search.find_all(text);
    if (line > 0) {
        matches.erase(std::remove_if(matches.begin(), matches.end(),
                                     [line](const TextMatch& match) { return match.line != line - 1; }),
                      matches.end());
    }
    if (matches.empty()) {
        return 0;
    }
    return apply_replacements(search, text, matches, replacement);
}

int IdeScriptEditor::apply_replacements(const TextSearch& search, const std::string& text,
                                        const std::vector<TextMatch>& matches, const std::string& replacement) {
    // The caret keeps its place in the text around the replacements.
    const int caret = area_->caretPosition();
    int moved = caret;
    for (const TextMatch& match : matches) {
        if (caret <= match.start) {
            break;
        }
        const int inserted = CodePoints(search.expand(text, match, replacement));
        if (caret >= match.end) {
            moved += inserted - (match.end - match.start);
            continue;
        }
        moved = match.start + (moved - caret) + inserted;
        break;
    }
    // One edit from the first match to the last, so one undo puts them all back.
    const std::string span = search.replace_span(text, matches, replacement);
    dismiss_completion();
    replacing_ = true;
    area_->replaceText(matches.front().start, matches.back().end, span);
    replacing_ = false;
    area_->moveTo(std::clamp(moved, 0, area_->length()));
    return static_cast<int>(matches.size());
}

void IdeScriptEditor::bindUndo(TextUndoStack* stack) {
    undo_stack_ = stack;
    // The constructor loads the source before this is bound. Seed the stack with it,
    // or its baseline is empty and undo wipes the script.
    if (undo_stack_ != nullptr && area_ && loaded_) {
        undo_stack_->reset(area_->getText());
    }
}

void IdeScriptEditor::applyUndoText() {
    if (!area_ || undo_stack_ == nullptr || area_->getText() == undo_stack_->text()) {
        return;
    }
    mute_undo_ = true;
    area_->suspendUndo();
    area_->setText(undo_stack_->text());
    area_->moveTo(undo_stack_->caret());
    area_->resumeUndo();
    mute_undo_ = false;
    dirty_ = true;
    dirty_at_ = std::chrono::steady_clock::now();
    paint();
}

IdeScriptEditor::~IdeScriptEditor() {
    if (color_edit_ && getScene() != nullptr && !getScene()->isTearingDown()) {
        getScene()->removeKeyHook(color_edit_->key_hook);
        getScene()->hidePopup(color_chooser_.get());
    }
    engine_.analysis().unwatch(id_);
}

void IdeScriptEditor::onOpen() { focus(); }

void IdeScriptEditor::onClose() { flush(); }

void IdeScriptEditor::layoutChildren() {
    if (!loaded_) {
        load();
    } else if (dirty_ && std::chrono::steady_clock::now() - dirty_at_ >= kSaveDelay) {
        flush();
    } else if (!dirty_ && engine_.paused() && !engine_.datamodel().simulation_running()) {
        // Edit mode, after Stop has restored the place. A paused test is still
        // the play session, so its source stays as the session left it.
        reapply();
    } else if (const std::uint64_t tree = engine_.datamodel().tree_revision(); tree != seen_title_tree_) {
        // During play only a rename changes what the tab shows.
        std::string text;
        std::string name;
        bool alive = false;
        if (read_source(text, name, alive)) {
            seen_title_tree_ = tree;
            if (alive && name != shown_name_) {
                setTitleText(name);
            }
        }
    }
    take_luau_list();
    if (completion_open()) {
        place_completion();
        if (area_) {
            static_cast<ScriptCodeArea*>(area_.get())->dismissHover();
        }
    } else if (area_) {
        static_cast<ScriptCodeArea*>(area_.get())->tickHover();
    }
    refresh_marks();
    // A press outside the picker closes it in the scene; the color it chose stays.
    if (color_edit_ && color_chooser_ && getScene() != nullptr && !getScene()->isPopupShowing(color_chooser_.get())) {
        close_color_picker(true);
    }
    if (findOpen()) {
        find_bar_->poll();
        // A click or an arrow key can put the selection on a match or take it off one.
        if (current_find() != painted_find_) {
            paint();
        }
    }
    StackPane::layoutChildren();
    place_find_bar();
}

void IdeScriptEditor::place_find_bar() {
    if (!find_bar_) {
        return;
    }
    if (!findOpen()) {
        find_bar_->performLayout(0, 0, 0, 0);
        return;
    }
    const double right = contentLeft() + contentWidth() - FindBar::kRightGap;
    const double width = std::max(0.0, std::min(FindBar::kMaxWidth, right - contentLeft() - 8));
    const double height = find_bar_->measuredHeight(width, -1);
    find_bar_->performLayout(right - width, contentTop(), width, height);
}

bool IdeScriptEditor::read_source(std::string& text, std::string& name, bool& alive, std::uint32_t* world) const {
    alive = false;
    engine_core::DataModel& game = engine_.datamodel();
    engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kLockWait);
    if (!lock.owns()) {
        return false;
    }
    const engine_core::DataModel* object = game.instance(id_);
    const auto* source = dynamic_cast<const engine_core::LuaSource*>(object);
    if (source == nullptr) {
        return true;
    }
    alive = true;
    text = source->source();
    name = game.name(id_);
    if (world != nullptr) {
        *world = game.world_generation();
    }
    return true;
}

void IdeScriptEditor::load() {
    if (!area_) {
        return;
    }
    std::string text;
    std::string name;
    bool alive = false;
    if (!read_source(text, name, alive, &world_)) {
        return;
    }
    if (!alive) {
        missing_ = true;
        area_->setEditable(false);
        return;
    }
    if (missing_) {
        // Undo brought the script back, with the same id.
        missing_ = false;
        area_->setEditable(true);
    }
    loading_ = true;
    area_->setText(std::move(text));
    area_->forgetHistory();
    if (undo_stack_ != nullptr) {
        undo_stack_->reset(area_->getText());
    }
    area_->moveTo(0);
    loading_ = false;
    paint();
    loaded_ = true;
    setTitleText(name);
    if (pending_range_) {
        const PendingRange range = *pending_range_;
        pending_range_.reset();
        showRange(range.line, range.column, range.column_end);
    }
}

void IdeScriptEditor::paint() {
    if (!area_) {
        return;
    }
    const std::string text = area_->getText();
    refresh_find(text);
    const int current = current_find();
    painted_find_ = current;
    // The highlighter's spans, cut where find matches start and end. A match adds
    // its class after the token's, so it keeps the token's color.
    jadefx::StyleSpansBuilder builder;
    std::size_t next = 0;
    int at = 0;
    for (const LuauSpan& span : highlight_luau(text)) {
        int left = span.length;
        while (left > 0) {
            while (next < find_matches_.size() && find_matches_[next].end <= at) {
                ++next;
            }
            int piece = left;
            const char* mark = nullptr;
            if (next < find_matches_.size()) {
                const TextMatch& match = find_matches_[next];
                if (match.start > at) {
                    piece = std::min(left, match.start - at);
                } else {
                    piece = std::min(left, match.end - at);
                    mark = static_cast<int>(next) == current ? "find-current" : "find-match";
                }
            }
            jadefx::TextStyle style;
            if (span.style != nullptr) {
                style.styleClass = span.style;
            }
            if (mark != nullptr) {
                style.styleClass = style.styleClass.empty() ? std::string(mark) : style.styleClass + " " + mark;
            }
            builder.add(style, piece);
            at += piece;
            left -= piece;
        }
    }
    show_find_count();
    area_->suspendUndo();
    area_->setStyleSpans(0, builder.create());
    area_->resumeUndo();
    refresh_color_swatches();
    refresh_scroll_marks();
}

void IdeScriptEditor::refresh_scroll_marks() {
    if (!area_) {
        return;
    }
    const auto& problems = static_cast<const ScriptCodeArea*>(area_.get())->problems();
    std::vector<jadefx::ScrollMark> marks;
    marks.reserve(find_matches_.size() + problems.size());
    // Find matches down the left half of the bar, problems down the right.
    const jadefx::Color found = theme_color("--ide-find-scroll-color");
    for (const TextMatch& match : find_matches_) {
        marks.push_back({match.start, match.end, found, jadefx::ScrollMarkLane::Left});
    }
    // Hints stay off the bar. Errors come last, so they draw over the rest.
    const std::pair<engine_core::Severity, const char*> severities[] = {
        {engine_core::Severity::Information, "--info-color"},
        {engine_core::Severity::Warning, "--warning-color"},
        {engine_core::Severity::Error, "--error-color"},
    };
    for (const auto& [severity, name] : severities) {
        const jadefx::Color color = theme_color(name);
        for (const ScriptMark& mark : problems) {
            if (mark.severity == severity) {
                marks.push_back({mark.start, mark.end, color, jadefx::ScrollMarkLane::Right});
            }
        }
    }
    area_->setScrollMarks(std::move(marks));
}

namespace {

// Room for the swatch and a little space before the text it marks.
constexpr double kSwatchSize = 10;
constexpr double kSwatchSlot = 14;

jadefx::Color to_jadefx(const engine_core::Color3& color) { return jadefx::Color::rgba(color.r, color.g, color.b, 1.f); }

engine_core::Color3 to_color3(const jadefx::Color& color) { return engine_core::Color3{color.r, color.g, color.b}; }

}  // namespace

void IdeScriptEditor::refresh_color_swatches() {
    color_literals_ = find_color3_literals(area_->getText());
    std::vector<jadefx::StyledTextArea::InlineNode> nodes;
    nodes.reserve(color_literals_.size());
    for (std::size_t i = 0; i < color_literals_.size(); ++i) {
        if (i == color_swatches_.size()) {
            // The slot takes the click and holds the square, with the gap before it.
            auto slot = std::make_shared<jadefx::Pane>();
            slot->getClassList().add("script-swatch");
            slot->setPrefSize(kSwatchSlot, kSwatchSize);
            slot->setPadding(jadefx::Insets{0, 0, 0, kSwatchSlot - kSwatchSize});
            // A click on it leaves the focus in the text, so Ctrl/Cmd+Z stays the text's undo.
            slot->setFocusTraversable(false);
            slot->setStyle("cursor: pointer;");
            auto square = std::make_shared<jadefx::Pane>();
            square->setPrefSize(kSwatchSize, kSwatchSize);
            square->setFocusTraversable(false);
            square->setStyle("border-width: 1px; border-style: solid; border-color: var(--ide-swatch-border-color); "
                             "border-radius: 2px;");
            slot->getChildren().add(square);
            slot->setOnMouseClicked([this, i](const jadefx::MouseEvent&) { open_color_picker(i); });
            color_swatches_.push_back(slot);
        }
        const std::shared_ptr<jadefx::Pane>& slot = color_swatches_[i];
        static_cast<jadefx::Pane&>(*slot->getChildren()[0]).setBackground(to_jadefx(color_literals_[i].color));
        nodes.push_back({color_literals_[i].end, slot});
    }
    area_->setInlineNodes(std::move(nodes));
}

void IdeScriptEditor::open_color_picker(std::size_t index) {
    jadefx::Scene* scene = getScene();
    if (scene == nullptr || index >= color_literals_.size() || index >= color_swatches_.size()) {
        return;
    }
    close_color_picker(true);
    const Color3Literal& literal = color_literals_[index];
    color_edit_ = std::make_unique<ColorEdit>();
    color_edit_->literal = literal;
    color_edit_->original = area_->getText(literal.start, literal.end);
    if (!color_chooser_) {
        // A Color3 has no alpha, and a script's colors have no use for a recent list.
        color_chooser_ = std::make_shared<jadefx::ColorChooser>();
        color_chooser_->setShowAlpha(false);
        color_chooser_->setShowRecentColors(false);
        color_chooser_->setOnValueChanged([this] { write_color(to_color3(color_chooser_->getValue())); });
    }
    color_chooser_->setOriginalValue(to_jadefx(literal.color));
    color_chooser_->setValue(to_jadefx(literal.color));
    // Enter keeps the color and Escape puts the literal back, as in a ColorPicker.
    color_edit_->key_hook = scene->addKeyHook([this](jadefx::KeyEvent& event) {
        if (!event.pressed || !color_edit_) {
            return;
        }
        if (event.key == jadefx::Key::Escape || event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter) {
            color_chooser_->commitEdits();
            close_color_picker(event.key != jadefx::Key::Escape);
            event.consume();
        }
    });
    jadefx::PopupOptions options;
    options.owner = color_swatches_[index].get();
    scene->showPopupNear(color_chooser_, color_swatches_[index].get(), jadefx::Side::Bottom, options);
}

void IdeScriptEditor::write_color(const engine_core::Color3& color) {
    if (!color_edit_ || !area_) {
        return;
    }
    Color3Literal& literal = color_edit_->literal;
    const std::string text = format_color3_literal(literal, color);
    const std::string current = area_->getText(literal.start, literal.end);
    if (text == current) {
        return;
    }
    // The caret keeps its place in the text around the literal.
    int caret = area_->caretPosition();
    const int grown = static_cast<int>(text.size()) - (literal.end - literal.start);
    if (caret >= literal.end) {
        caret += grown;
    } else if (caret > literal.start) {
        caret = literal.start;
    }
    // Each change while the picker is open is one step of a single edit, recorded when it closes.
    mute_undo_ = true;
    area_->replaceText(literal.start, literal.end, text);
    mute_undo_ = false;
    literal.end = literal.start + static_cast<int>(text.size());
    area_->moveTo(caret);
}

void IdeScriptEditor::close_color_picker(bool keep) { close_color_picker(keep, getScene()); }

void IdeScriptEditor::sceneChanged(jadefx::Scene* previous) {
    IdePane::sceneChanged(previous);
    if (color_edit_ && previous != nullptr) {
        // The hook stays on the scene it was added to. Left there, it would take
        // that scene's Enter and Escape, and reach this editor after it is gone.
        close_color_picker(true, previous->isTearingDown() ? nullptr : previous);
    }
}

void IdeScriptEditor::close_color_picker(bool keep, jadefx::Scene* scene) {
    if (!color_edit_) {
        return;
    }
    const std::unique_ptr<ColorEdit> edit = std::move(color_edit_);
    if (scene != nullptr) {
        scene->removeKeyHook(edit->key_hook);
        if (color_chooser_ && scene->isPopupShowing(color_chooser_.get())) {
            scene->hidePopup(color_chooser_.get());
        }
    }
    // Back to the text, so the next Ctrl/Cmd+Z undoes the color as one step. Not in
    // a scene this editor is leaving.
    if (scene != nullptr && scene == getScene()) {
        area_->requestFocus();
    }
    const Color3Literal& literal = edit->literal;
    const std::string current = area_->getText(literal.start, literal.end);
    if (!keep) {
        if (current != edit->original) {
            mute_undo_ = true;
            area_->replaceText(literal.start, literal.end, edit->original);
            mute_undo_ = false;
        }
        return;
    }
    if (current != edit->original && undo_stack_ != nullptr &&
        !undo_stack_->record_change(literal.start, edit->original, current)) {
        undo_stack_->reset(area_->getText());
    }
}

namespace {

void show_banner(jadefx::Label& status, const ScriptProblemSummary& summary) {
    if (summary.text.empty()) {
        if (!status.isVisible()) {
            return;
        }
        status.setVisible(false);
        status.setText("");
        status.setPrefHeight(0);
        status.setMaxSize(100000, 0);
        status.setStyle(kBannerStyle);
        return;
    }
    if (status.isVisible() && status.getText() == summary.text) {
        return;
    }
    status.setVisible(true);
    status.setText(summary.text);
    status.setPrefHeight(22);
    status.setMaxSize(100000, 28);
    const char* colors = "color: var(--ide-banner-text-color); background-color: var(--ide-banner-color);";
    if (summary.blocks_compile || summary.severity == engine_core::Severity::Error) {
        colors = "color: var(--ide-error-text-color); background-color: var(--ide-banner-error-color);";
    } else if (summary.severity == engine_core::Severity::Warning) {
        colors = "color: var(--ide-warning-text-color); background-color: var(--ide-banner-warning-color);";
    }
    status.setStyle(std::string(kBannerStyle) + colors);
}

}  // namespace

void IdeScriptEditor::refresh_marks() {
    // Publishing here is the UI thread. The engine render thread does not run this.
    engine_.analysis().pump();
    if (!area_ || !status_ || !loaded_ || missing_) {
        return;
    }
    const std::optional<std::string> checked = engine_.analysis().analyzed_source(id_);
    std::vector<engine_core::Diagnostic> diagnostics;
    if (checked && *checked == area_->getText()) {
        diagnostics = engine_.analysis().diagnostics(id_);
    }
    auto* code = static_cast<ScriptCodeArea*>(area_.get());
    if (code->setProblems(marks_for(area_->getText(), diagnostics))) {
        code->dismissHover();
        refresh_scroll_marks();
    }
    show_banner(*status_, summarize_problems(diagnostics));
}

void IdeScriptEditor::note_text() {
    if (loading_ || !area_) {
        return;
    }
    static_cast<ScriptCodeArea*>(area_.get())->dismissHover();
    paint();
    if (!completion_.accepting() && !replacing_) {
        refresh_completion(false);
    }
    dirty_ = true;
    dirty_at_ = std::chrono::steady_clock::now();
}

void IdeScriptEditor::push(const std::string& text) {
    if (!commit_) {
        return;
    }
    const std::uint64_t gen = commit_->epoch.fetch_add(1, std::memory_order_relaxed) + 1;
    std::shared_ptr<Commit> commit = commit_;
    engine_.on_simulation([commit, text, gen](engine_core::DataModel& game) {
        if (auto* source = dynamic_cast<engine_core::LuaSource*>(game.instance(commit->id))) {
            if (source->source() != text) {
                // One place waypoint for the buffer, not one per keystroke.
                // Play leaves this off the edit stack unless a recording is already open.
                std::optional<std::string> recording;
                if (!game.simulation_running()) {
                    recording = game.history().try_begin_recording("Edit Script");
                }
                source->set_source(text);
                if (recording) {
                    game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
                }
                if (!game.simulation_running()) {
                    game.capture_place();
                }
            }
        }
        commit->acked.store(gen, std::memory_order_release);
    });
}

void IdeScriptEditor::flush() {
    if (!area_ || missing_ || !loaded_) {
        return;
    }
    dirty_ = false;
    push(area_->getText());
}

void IdeScriptEditor::reapply() {
    if (!area_ || !loaded_ || !commit_) {
        return;
    }
    // In edit mode every Source, name, and tree change moves one of these, and
    // so does Stop. Unmoved, the script is as the last pass here read it.
    const engine_core::DataModel& game = engine_.datamodel();
    const std::uint64_t authored = game.authored_revision();
    const std::uint64_t tree = game.tree_revision();
    if (authored == seen_authored_ && tree == seen_tree_) {
        return;
    }
    if (missing_) {
        // Loads again once the script is back, as after an undo of its delete.
        loaded_ = false;
        load();
        if (loaded_) {
            seen_authored_ = authored;
            seen_tree_ = tree;
        }
        return;
    }
    if (commit_->acked.load(std::memory_order_acquire) != commit_->epoch.load(std::memory_order_relaxed)) {
        return;
    }
    std::string text;
    std::string name;
    bool alive = false;
    std::uint32_t world = 0;
    if (!read_source(text, name, alive, &world)) {
        return;
    }
    seen_authored_ = authored;
    seen_tree_ = tree;
    if (!alive) {
        missing_ = true;
        area_->setEditable(false);
        return;
    }
    if (name != shown_name_) {
        setTitleText(name);
    }
    const bool stopped = world != world_;
    world_ = world;
    if (text == area_->getText()) {
        return;
    }
    if (stopped) {
        // Stop restored the authored place. The buffer holds edits made during
        // play, so it wins and becomes the place's source.
        flush();
    } else if (!dirty_) {
        // The Source changed under an idle buffer in edit mode: a place undo or
        // redo, or another writer. Show it rather than writing the buffer over it.
        show_source(std::move(text));
    }
}

void IdeScriptEditor::show_source(std::string text) {
    const int caret = area_->caretPosition();
    loading_ = true;
    area_->suspendUndo();
    area_->setText(std::move(text));
    area_->resumeUndo();
    area_->forgetHistory();
    // Keystroke undo would rewrite the text the place history just set.
    if (undo_stack_ != nullptr) {
        undo_stack_->reset(area_->getText());
    }
    area_->moveTo(std::min(caret, area_->length()));
    loading_ = false;
    paint();
    dismiss_completion();
}

std::vector<engine_core::LuaNode> IdeScriptEditor::world() const {
    const std::string text = area_ ? area_->getText() : std::string();
    return completion_world(engine_, id_, area_ ? &text : nullptr);
}

bool IdeScriptEditor::completion_open() const { return completion_.isOpen(); }

bool IdeScriptEditor::completion_commits_name() const { return completion_.commitsName(); }

bool IdeScriptEditor::completion_commits_quote(char quote, bool unclosed_only) const {
    return completion_.commitsQuote(quote, unclosed_only);
}

bool IdeScriptEditor::completion_key_accepts() const { return completion_.keyAccepts(); }

void IdeScriptEditor::dismiss_completion() { completion_.dismiss(); }

void IdeScriptEditor::move_completion(int delta) { completion_.move(delta); }

void IdeScriptEditor::accept_completion(bool parentheses) {
    if (!area_) {
        completion_.dismiss();
        return;
    }
    const std::optional<CompletionEdit> edit = completion_.take(parentheses, area_->getText());
    if (!edit) {
        return;
    }
    area_->replaceText(edit->begin, edit->end, edit->text);
    area_->moveTo(edit->caret);
    area_->requestFocus();
    completion_.finish();
}

void IdeScriptEditor::place_completion() {
    if (!area_ || !completion_.isOpen()) {
        return;
    }
    const jadefx::TextBounds bounds = area_->caretBounds();
    if (!bounds.valid) {
        completion_.dismiss();
        return;
    }
    completion_.moveTo(*area_, bounds.x, bounds.y, bounds.height);
}

void IdeScriptEditor::refresh_completion(bool force) {
    if (!area_ || loading_ || missing_ || completion_.accepting()) {
        return;
    }
    if (area_->selections().size() > 1) {
        completion_.dismiss();
        return;
    }
    const jadefx::TextBounds bounds = area_->caretBounds();
    if (!bounds.valid) {
        completion_.dismiss();
        return;
    }
    const std::string text = area_->getText();
    const int caret = area_->caretPosition();
    const std::vector<engine_core::LuaNode> place = world();
    // What needs Luau's types shows on a later frame, so typing never waits on
    // the type checker. Until then the popup keeps the rows that still fit.
    CompletionList now;
    luau_list_ = ask_completion(now, engine_.analysis(), text, caret, place, id_, true, force, "editor");
    if (!luau_list_) {
        completion_.present(std::move(now), force, *area_, bounds.x, bounds.y, bounds.height);
        return;
    }
    completion_.narrow(luau_list_->plan.frame, *area_, bounds.x, bounds.y, bounds.height);
    luau_list_->shown = completion_.isOpen();
    luau_list_->dismissals = completion_.dismissals();
}

void IdeScriptEditor::take_luau_list() {
    if (!luau_list_) {
        return;
    }
    std::optional<CompletionList> list = take_completion(*luau_list_);
    if (!list) {
        return;
    }
    PendingCompletion pending = std::move(*luau_list_);
    luau_list_.reset();
    if (!area_ || loading_ || missing_ || completion_.accepting() || completion_.isOpen() != pending.shown ||
        completion_.dismissals() != pending.dismissals || area_->caretPosition() != pending.caret) {
        return;
    }
    const jadefx::TextBounds bounds = area_->caretBounds();
    if (!bounds.valid || area_->getText() != pending.source) {
        return;
    }
    completion_.present(std::move(*list), pending.force, *area_, bounds.x, bounds.y, bounds.height);
}


void ScriptCodeArea::handleKey(jadefx::KeyEvent& event) {
    if (editor == nullptr || (!event.pressed && !event.repeat)) {
        jadefx::CodeArea::handleKey(event);
        return;
    }
    if (event.shortcut() && event.key == jadefx::Key::Space) {
        editor->refresh_completion(true);
        event.consume();
        return;
    }
    // Ctrl+/ with or without Shift, so Ctrl+? toggles too.
    if (event.shortcut() && !event.alt && event.key == jadefx::Key::Slash && applyComment()) {
        event.consume();
        return;
    }
    const FindChord chord = find_chord(event);
    if (chord == FindChord::Find || chord == FindChord::Replace) {
        editor->openFind(chord == FindChord::Replace);
        event.consume();
        return;
    }
    if (editor->findOpen()) {
        if (event.key == jadefx::Key::F3 || (event.shortcut() && !event.alt && event.key == jadefx::Key::G)) {
            editor->step_find(event.shift ? -1 : 1);
            event.consume();
            return;
        }
        if (event.key == jadefx::Key::Escape && !event.shift && !event.shortcut() && !editor->completion_open()) {
            editor->closeFind();
            event.consume();
            return;
        }
    }
    const bool plain_enter = (event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter) && !event.shift &&
                             !event.shortcut();
    if (editor->completion_open()) {
        if ((event.key == jadefx::Key::Up || event.key == jadefx::Key::Down) && !event.shortcut()) {
            editor->move_completion(event.key == jadefx::Key::Down ? 1 : -1);
            event.consume();
            return;
        }
        if ((event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter || event.key == jadefx::Key::Tab) &&
            !event.shift && !event.shortcut()) {
            if (editor->completion_key_accepts()) {
                editor->accept_completion(true);
                event.consume();
                return;
            }
            editor->dismiss_completion();
        }
        if (event.key == jadefx::Key::Escape) {
            editor->dismiss_completion();
            event.consume();
            return;
        }
        if (event.key == jadefx::Key::Left || event.key == jadefx::Key::Right || event.key == jadefx::Key::Home ||
            event.key == jadefx::Key::End || event.key == jadefx::Key::PageUp || event.key == jadefx::Key::PageDown) {
            editor->dismiss_completion();
        }
    }
    if (plain_enter && applyEnter()) {
        event.consume();
        return;
    }
    jadefx::CodeArea::handleKey(event);
}

bool ScriptCodeArea::applyPair(char unit) {
    if (!isEditable() || selections().size() != 1) {
        return false;
    }
    const jadefx::IndexRange range = selection();
    const PairResult pair =
        pair_luau(getText(), range.start, range.end, static_cast<char32_t>(static_cast<unsigned char>(unit)));
    if (pair.action == PairAction::None) {
        return false;
    }
    if (pair.action == PairAction::Skip) {
        dismissHover();
        if (editor != nullptr) {
            editor->dismiss_completion();
        }
        moveTo(range.end + 1);
        return true;
    }
    if (pair.action == PairAction::Insert) {
        const int caret = range.start;
        std::string both;
        both.push_back(pair.open);
        both.push_back(pair.close);
        transact(false, [&] {
            replaceText(caret, caret, both);
            moveTo(caret + 1);
        });
        return true;
    }
    const std::string selected = getText(range.start, range.end);
    std::string wrapped;
    wrapped.push_back(pair.open);
    wrapped += selected;
    wrapped.push_back(pair.close);
    replaceText(range.start, range.end, wrapped);
    return true;
}

bool ScriptCodeArea::applyEnter() {
    if (!isEditable() || selections().size() != 1) {
        return false;
    }
    const jadefx::IndexRange range = selection();
    if (!range.empty()) {
        return false;
    }
    const EnterResult result = enter_luau(getText(), range.start, getTabSize(), isInsertSpacesForTab());
    if (!result.insert) {
        return false;
    }
    transact(false, [&] {
        replaceText(result.begin, result.end, result.text);
        moveTo(result.caret);
    });
    return true;
}

bool ScriptCodeArea::applyComment() {
    if (!isEditable() || selections().size() != 1) {
        return false;
    }
    const jadefx::IndexRange range = selection();
    const int caret = caretPosition();
    const int anchor = caret == range.start ? range.end : range.start;
    const CommentResult result = comment_luau(getText(), anchor, caret);
    if (!result.change) {
        return false;
    }
    dismissHover();
    editor->dismiss_completion();
    transact(false, [&] {
        replaceText(result.begin, result.end, result.text);
        selectRange(result.anchor, result.caret);
    });
    return true;
}

void ScriptCodeArea::handleText(jadefx::TextEvent& event) {
    if (editor != nullptr && event.text.size() == 1) {
        const char unit = event.text[0];
        const bool name_key = unit == '.' || unit == ':' || unit == '(';
        const bool quote_key = unit == '"' || unit == '\'';
        const bool one = selections().size() == 1;
        const jadefx::IndexRange range = one ? selection() : jadefx::IndexRange{};
        const bool steps_over = one && range.empty() && quote_key &&
                                source_code_point(getText(), range.start) ==
                                    static_cast<char32_t>(static_cast<unsigned char>(unit));
        if (name_key && editor->completion_commits_name()) {
            editor->accept_completion(false);
        } else if (quote_key && editor->completion_commits_quote(unit, !steps_over)) {
            editor->accept_completion(false);
            if (steps_over) {
                event.consume();
                return;
            }
        }
        if (one && applyPair(unit)) {
            event.consume();
            return;
        }
    }
    jadefx::CodeArea::handleText(event);
}

void ScriptCodeArea::handleMousePressed(const jadefx::MouseEvent& event) {
    dismissHover();
    if (editor != nullptr) {
        editor->dismiss_completion();
    }
    jadefx::CodeArea::handleMousePressed(event);
}

void ScriptCodeArea::handleMouseMoved(const jadefx::MouseEvent& event) {
    jadefx::CodeArea::handleMouseMoved(event);
    if (editor == nullptr) {
        return;
    }
    // Below the last line nothing is under the pointer, even where it lines up with a word on that line.
    const jadefx::CharacterHit where = hit(event.x, event.y);
    if (!where.valid || !where.onLine) {
        dismissHover();
        return;
    }
    int index = where.characterIndex;
    if (index < 0) {
        index = where.insertionIndex > 0 ? where.insertionIndex - 1 : where.insertionIndex;
    }
    armHover(index, event.x, event.y);
}

void ScriptCodeArea::handleScroll(jadefx::ScrollEvent& event) {
    dismissHover();
    jadefx::CodeArea::handleScroll(event);
    if (editor != nullptr && editor->completion_open()) {
        editor->place_completion();
    }
}

namespace {

constexpr std::chrono::milliseconds kHoverDelay(500);
// The hover tip's widest, and its distance from the window's edges.
constexpr double kTipMaxWidth = 560;
constexpr double kTipMargin = 8;
constexpr double kTipPaddingX = 8;
// The IDE's text font, from the scene rule in IdeLayout's stylesheet. A new
// Label's own font is 16px, which is larger than the text around it.
constexpr const char* kTipFontFamily = "Open Sans";
constexpr float kTipFontSize = 13.f;

int WordStart(std::string_view text, int index) {
    int cursor = index;
    while (cursor > 0 && NameChar(CodePointAt(text, cursor - 1))) {
        --cursor;
    }
    return cursor;
}

}  // namespace

bool ScriptCodeArea::setProblems(std::vector<ScriptMark> marks) {
    if (marks == problems_) {
        return false;
    }
    problems_ = std::move(marks);
    std::vector<jadefx::TextMark> visual;
    visual.reserve(problems_.size());
    for (const ScriptMark& mark : problems_) {
        jadefx::TextMark text;
        text.start = mark.start;
        text.end = mark.end;
        switch (mark.severity) {
        case engine_core::Severity::Error:
            text.severity = jadefx::TextMarkSeverity::Error;
            break;
        case engine_core::Severity::Warning:
            text.severity = jadefx::TextMarkSeverity::Warning;
            break;
        case engine_core::Severity::Information:
            text.severity = jadefx::TextMarkSeverity::Information;
            break;
        case engine_core::Severity::Hint:
            text.severity = jadefx::TextMarkSeverity::Hint;
            break;
        }
        visual.push_back(text);
    }
    setTextMarks(std::move(visual));
    return true;
}

const ScriptMark* ScriptCodeArea::problemAt(int index) const {
    const ScriptMark* fallback = nullptr;
    for (const ScriptMark& mark : problems_) {
        if (!mark_covers(mark, index)) {
            continue;
        }
        if (mark.code == "Syntax") {
            return &mark;
        }
        if (fallback == nullptr ||
            (mark.severity == engine_core::Severity::Error && fallback->severity != engine_core::Severity::Error)) {
            fallback = &mark;
        }
    }
    return fallback;
}

bool ScriptCodeArea::sameProblem(int index) const {
    const ScriptMark* here = problemAt(index);
    const ScriptMark* there = problemAt(hover_index_);
    if (here == nullptr || there == nullptr) {
        return false;
    }
    return here->start == there->start && here->end == there->end && here->code == there->code;
}

bool ScriptCodeArea::sameWord(int index) const {
    if (hover_index_ < 0 || index < 0) {
        return false;
    }
    const std::string text = getText();
    if (!NameChar(CodePointAt(text, hover_index_)) || !NameChar(CodePointAt(text, index))) {
        return false;
    }
    return WordStart(text, hover_index_) == WordStart(text, index);
}

void ScriptCodeArea::dismissHover() {
    hover_answer_.reset();
    hover_index_ = -1;
    hover_begin_ = -1;
    hover_end_ = -1;
    hover_waiting_ = false;
    if (!tip_) {
        return;
    }
    jadefx::Scene* scene = getScene();
    if (scene != nullptr && !scene->isTearingDown() && scene->isPopupShowing(tip_.get())) {
        scene->hidePopup(tip_.get());
    }
}

void ScriptCodeArea::armHover(int index, double x, double y) {
    if (editor != nullptr && editor->completion_open()) {
        dismissHover();
        return;
    }
    if (hover_waiting_ && sameProblem(index)) {
        hover_index_ = index;
        return;
    }
    if (hover_waiting_ && problemAt(index) == nullptr && sameWord(index)) {
        hover_index_ = index;
        return;
    }
    if (tip_ && getScene() != nullptr && getScene()->isPopupShowing(tip_.get()) && index >= hover_begin_ &&
        index < hover_end_) {
        hover_index_ = index;
        return;
    }
    if (tip_ && getScene() != nullptr && getScene()->isPopupShowing(tip_.get())) {
        jadefx::Scene* scene = getScene();
        if (scene != nullptr && !scene->isTearingDown()) {
            scene->hidePopup(tip_.get());
        }
    }
    hover_index_ = index;
    hover_begin_ = -1;
    hover_end_ = -1;
    anchor_x_ = x;
    anchor_y_ = y;
    hover_since_ = std::chrono::steady_clock::now();
    hover_waiting_ = true;
}

void ScriptCodeArea::showTip(const std::string& title, const std::string& detail, const std::string& summary,
                             const jadefx::Color& titleFill) {
    if (title.empty() && detail.empty()) {
        hover_waiting_ = false;
        return;
    }
    if (!tip_) {
        tip_ = jadefx::make<jadefx::VBox>();
        tip_->setMouseTransparent(true);
        tip_->setSpacing(2);
        tip_->setPadding(jadefx::Insets{6, kTipPaddingX, 6, kTipPaddingX});
        tip_->setStyle(
            "background-color: var(--ide-popup-color); border-style: solid; border-width: 1px; "
            "border-color: var(--ide-popup-border-color); box-shadow: 0 2px 8px var(--ide-popup-shadow-color);");
    }
    tip_->getChildren().clear();
    const jadefx::Color body_fill = theme_color("--ide-popup-detail-text-color");
    // A Label draws one line and cuts off what does not fit, so long text is
    // wrapped here, one Label per line. The tip is never wider than
    // kTipMaxWidth, or than the window less a margin.
    double wrap_width = kTipMaxWidth;
    if (const jadefx::Scene* scene = getScene(); scene != nullptr && scene->getWidth() > 0) {
        wrap_width = std::min(wrap_width, scene->getWidth() - kTipMargin * 2);
    }
    wrap_width = std::max(wrap_width - kTipPaddingX * 2, 80.0);
    // Measured and drawn in the same font: each line pins it, so a style cannot change it later.
    const jadefx::Font font(kTipFontFamily, kTipFontSize);
    const auto measure = [&font](const std::string& line) { return static_cast<double>(font.measureWidth(line)); };
    auto add_line = [&](const std::string& text, const jadefx::Color& fill) {
        if (text.empty()) {
            return;
        }
        for (const std::string& line : WrapText(text, wrap_width, measure)) {
            auto label = jadefx::make<jadefx::Label>(line);
            label->setFont(font);
            label->setTextFill(fill);
            label->setMouseTransparent(true);
            tip_->getChildren().add(label);
        }
    };
    add_line(title, titleFill);
    add_line(detail, body_fill);
    add_line(summary, body_fill);

    jadefx::Scene* scene = getScene();
    if (scene == nullptr || scene->isTearingDown()) {
        return;
    }
    jadefx::PopupOptions options;
    options.owner = this;
    options.autoHide = true;
    scene->showPopup(tip_, anchor_x_, anchor_y_ + 18, -1, -1, options);
    double width = tip_->getWidth();
    double height = tip_->getHeight();
    if (width < 1) {
        width = 160;
    }
    if (height < 1) {
        height = 28;
    }
    double x = anchor_x_;
    double y = anchor_y_ + 18;
    if (scene->getWidth() > 0 && x + width > scene->getWidth()) {
        x = std::max(0.0, scene->getWidth() - width);
    }
    if (x < 0) {
        x = 0;
    }
    if (scene->getHeight() > 0 && y + height > scene->getHeight() && anchor_y_ > height + 4) {
        y = anchor_y_ - height - 4;
    }
    scene->movePopup(tip_.get(), x, y, width, height);
    hover_waiting_ = false;
}

void ScriptCodeArea::showHover() {
    if (editor == nullptr || hover_index_ < 0 || editor->completion_open()) {
        return;
    }
    if (const ScriptMark* mark = problemAt(hover_index_)) {
        hover_begin_ = mark->start;
        hover_end_ = std::max(mark->end, mark->start + 1);
        const char* title = "--ide-error-text-color";
        if (mark->severity == engine_core::Severity::Warning) {
            title = "--ide-warning-text-color";
        } else if (mark->severity != engine_core::Severity::Error) {
            title = "--ide-popup-text-color";
        }
        const std::string heading = mark->message.empty() ? mark->code : mark->message;
        showTip(heading, problem_detail(*mark), "", theme_color(title));
        return;
    }
    const std::string text = getText();
    const std::vector<engine_core::LuaNode> place = editor->world();
    // A name's type comes from Luau; tickHover shows it when it arrives.
    HoverInfo info;
    hover_answer_ = ask_hover(info, editor->engine_.analysis(), text, hover_index_, place, editor->id_);
    if (!info.found || info.title.empty()) {
        hover_waiting_ = false;
        return;
    }
    hover_begin_ = info.begin;
    hover_end_ = info.end;
    showTip(info.title, info.detail, info.summary, theme_color("--ide-popup-text-color"));
}

void ScriptCodeArea::tickHover() {
    if (hover_answer_) {
        if (const std::optional<HoverInfo> answered = take_hover(*hover_answer_)) {
            const std::string asked = std::move(hover_answer_->source);
            hover_answer_.reset();
            const HoverInfo& info = *answered;
            // Only while the pointer is still on that name and the text is as it was.
            if (info.found && !info.title.empty() && hover_index_ >= info.begin && hover_index_ < info.end &&
                (editor == nullptr || !editor->completion_open()) && getText() == asked) {
                hover_begin_ = info.begin;
                hover_end_ = info.end;
                showTip(info.title, info.detail, info.summary, theme_color("--ide-popup-text-color"));
            }
        }
    }
    if (!hover_waiting_ || hover_index_ < 0) {
        return;
    }
    if (editor != nullptr && editor->completion_open()) {
        dismissHover();
        return;
    }
    if (std::chrono::steady_clock::now() - hover_since_ < kHoverDelay) {
        return;
    }
    showHover();
}

}  // namespace ide
