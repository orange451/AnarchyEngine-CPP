#include "IdeShaderEditor.hpp"

#include "EditorFont.hpp"
#include "FindBar.hpp"
#include "GlslHighlight.hpp"
#include "IdeTheme.hpp"
#include "LockWaits.hpp"
#include "Utf8.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "Shader.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <optional>
#include <utility>

namespace ide {
namespace {

// As the CSS editor: Source is written half a second after the last keystroke.
constexpr std::chrono::milliseconds kSaveDelay(500);
// The most matches the find bar counts and highlights, as the script editor.
constexpr std::size_t kFindLimit = 19999;

bool NameChar(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= U'0' && c <= U'9') || c == U'_';
}

// The name the caret at column touches in line, as code points [start, end).
jadefx::IndexRange NameAround(const std::u32string& line, int column) {
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

// The script editor's syntax colors, so a shader reads like a script.
void DefineStyles(jadefx::StyleClassedTextArea& area) {
    const auto define = [&area](const char* name, const char* color, bool bold) {
        jadefx::TextStyle style;
        style.hasFill = true;
        style.fill = theme_color(color);
        style.bold = bold;
        area.defineStyleClass(name, style);
    };
    define("keyword", "--ide-syntax-keyword-color", true);
    define("builtin", "--ide-syntax-builtin-color", false);
    define("datatype", "--ide-syntax-datatype-color", false);
    define("comment", "--ide-syntax-comment-color", false);
    define("number", "--ide-syntax-number-color", false);
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

// The line "<line>: <message>" names, or 0.
int ErrorLine(const std::string& error) {
    const std::size_t colon = error.find(':');
    if (colon == std::string::npos || colon == 0) {
        return 0;
    }
    return std::max(0, std::atoi(error.substr(0, colon).c_str()));
}

}  // namespace

// Enter keeps the indent, and { brings its }, as in the CSS editor.
class IdeShaderEditor::Area : public jadefx::CodeArea {
public:
    IdeShaderEditor* editor = nullptr;

    // Runs work as one undo step, as the editor's Replace All needs.
    void asOneStep(const std::function<void()>& work) { transact(false, work); }

    void handleKey(jadefx::KeyEvent& event) override {
        if (editor != nullptr && (event.pressed || event.repeat)) {
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
                if (event.key == jadefx::Key::Escape && !event.shift && !event.shortcut()) {
                    editor->closeFind();
                    event.consume();
                    return;
                }
            }
        }
        const bool plain_enter = (event.pressed || event.repeat) &&
                                 (event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter) &&
                                 !event.shift && !event.shortcut() && !event.alt;
        if (plain_enter && applyEnter()) {
            event.consume();
            return;
        }
        jadefx::CodeArea::handleKey(event);
    }

    void handleText(jadefx::TextEvent& event) override {
        if (event.text == "{" && applyBrace()) {
            event.consume();
            return;
        }
        jadefx::CodeArea::handleText(event);
    }

private:
    bool single() const { return isEditable() && selections().size() == 1 && selection().empty(); }

    bool applyEnter() {
        if (!single()) {
            return false;
        }
        const std::string indent = isInsertSpacesForTab() ? std::string(static_cast<std::size_t>(getTabSize()), ' ')
                                                          : std::string("\t");
        const CssEnter enter = enter_glsl(getText(), caretPosition(), indent);
        transact(false, [&] {
            replaceText(enter.begin, enter.end, enter.text);
            moveTo(enter.caret);
        });
        return true;
    }

    bool applyBrace() {
        if (!single()) {
            return false;
        }
        const int at = caretPosition();
        const CssBrace brace = brace_glsl(getText(), at);
        if (!brace.insert) {
            return false;
        }
        transact(false, [&] {
            replaceText(at, at, brace.text);
            moveTo(brace.caret);
        });
        return true;
    }
};

struct IdeShaderEditor::Commit {
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<std::uint64_t> acked{0};
    std::uint32_t id = 0;
};

IdeShaderEditor::IdeShaderEditor(engine_core::Engine& engine, std::uint32_t id)
    : IdePane("Shader.glsl", true), engine_(engine), id_(id), commit_(std::make_shared<Commit>()) {
    setIconFile("Shader.png");
    commit_->id = id;
    area_ = std::make_shared<Area>();
    area_->editor = this;
    area_->getClassList().add("ide-script");
    (void)editor_mono_family();
    DefineStyles(*area_);
    area_->setStyle("font-family: \"" + editor_font_family() + "\";");
    themeListener_ = std::make_unique<ThemeListener>([this] {
        DefineStyles(*area_);
        area_->setStyle("font-family: \"" + editor_font_family() + "\";");
        const std::string error = shownError_;
        shownError_.clear();
        markError(error);
    });
    area_->setOnPlainTextChange([this](const jadefx::PlainTextChange&) { noteText(); });
    Fill(*area_);
    getChildren().add(area_);

    FindBar::Actions find;
    find.changed = [this] { find_changed(); };
    find.step = [this](int direction) { step_find(direction); };
    find.replace = [this] { replace_find(); };
    find.replace_all = [this] { replace_find_all(); };
    find.close = [this] { closeFind(); };
    find_bar_ = jadefx::make<FindBar>(std::move(find));
    find_bar_->setVisible(false);
    // After the text, so it draws over it and is hit first.
    getChildren().add(find_bar_);
    load();
}

IdeShaderEditor::~IdeShaderEditor() = default;

std::string IdeShaderEditor::text() const { return area_ ? area_->getText() : std::string(); }

void IdeShaderEditor::focus() {
    if (area_) {
        area_->requestFocus();
    }
}

void IdeShaderEditor::showLine(int line) {
    if (!area_ || line < 1) {
        return;
    }
    const int paragraph = std::min(line, area_->paragraphCount()) - 1;
    area_->moveTo(paragraph, 0);
    area_->showPosition(area_->caretPosition());
}

void IdeShaderEditor::setTitleText(const std::string& name) {
    shownName_ = name;
    setTitle(name + ".glsl");
}

void IdeShaderEditor::onOpen() { focus(); }

void IdeShaderEditor::onClose() { flush(); }

void IdeShaderEditor::layoutChildren() {
    if (!loaded_) {
        load();
    } else if (dirty_ && std::chrono::steady_clock::now() - dirtyAt_ >= kSaveDelay) {
        flush();
    } else if (!dirty_) {
        reapply();
    }
    if (findOpen()) {
        find_bar_->poll();
        // A click or an arrow key can put the selection on a match or take it off one.
        if (current_find() != painted_find_) {
            paint(area_->getText());
        }
    }
    StackPane::layoutChildren();
    place_find_bar();
    // The source saves after a pause and can change in the place, so the editor checks again next frame.
    markLayoutDirty(LayoutDirt::Arrange);
}

bool IdeShaderEditor::readSource(std::string& text, std::string& name, std::string& error, bool& alive,
                                 std::uint32_t* world) const {
    alive = false;
    engine_core::DataModel& game = engine_.datamodel();
    engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionLockWait);
    if (!lock.owns()) {
        return false;
    }
    const auto* shader = dynamic_cast<const engine_core::Shader*>(game.instance(id_));
    if (shader == nullptr) {
        return true;
    }
    alive = true;
    text = shader->source();
    error = shader->error();
    name = game.name(id_);
    if (world != nullptr) {
        *world = game.world_generation();
    }
    return true;
}

void IdeShaderEditor::load() {
    std::string text;
    std::string name;
    std::string error;
    bool alive = false;
    if (!readSource(text, name, error, alive, &world_)) {
        return;
    }
    if (!alive) {
        missing_ = true;
        area_->setEditable(false);
        return;
    }
    if (missing_) {
        // Undo brought it back, with the same id.
        missing_ = false;
        area_->setEditable(true);
    }
    loading_ = true;
    area_->setText(text);
    area_->forgetHistory();
    area_->moveTo(0);
    loading_ = false;
    paint(text);
    loaded_ = true;
    seenAuthored_ = engine_.datamodel().authored_revision();
    seenTree_ = engine_.datamodel().tree_revision();
    setTitleText(name);
    markError(error);
}

void IdeShaderEditor::paint(const std::string& text) {
    refresh_find(text);
    const int current = current_find();
    painted_find_ = current;
    // The highlighter's spans, cut where find matches start and end. A match adds
    // its class after the token's, so it keeps the token's color.
    jadefx::StyleSpansBuilder builder;
    std::size_t next = 0;
    int at = 0;
    for (const GlslSpan& span : highlight_glsl(text)) {
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
    refresh_scroll_marks();
}

void IdeShaderEditor::refresh_scroll_marks() {
    std::vector<jadefx::ScrollMark> marks;
    marks.reserve(find_matches_.size() + 1);
    // Find matches down the left half of the bar, the error down the right.
    const jadefx::Color found = theme_color("--ide-find-scroll-color");
    for (const TextMatch& match : find_matches_) {
        marks.push_back({match.start, match.end, found, jadefx::ScrollMarkLane::Left});
    }
    if (errorEnd_ > errorStart_) {
        marks.push_back({errorStart_, errorEnd_, theme_color("--error-color"), jadefx::ScrollMarkLane::Right});
    }
    area_->setScrollMarks(std::move(marks));
}

bool IdeShaderEditor::findOpen() const { return find_bar_ && find_bar_->isVisible(); }

void IdeShaderEditor::place_find_bar() {
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

void IdeShaderEditor::openFind(bool replace) {
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
            const jadefx::IndexRange name = NameAround(area_->getParagraph(at.paragraph).content(), at.column);
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
    if (replace || !was_open) {
        find_bar_->setReplaceShown(replace);
    }
    if (!seed.empty()) {
        find_bar_->setFindText(seed);
    }
    paint(area_->getText());
    if (replace && !find_bar_->query().pattern.empty()) {
        find_bar_->focusReplace();
    } else {
        find_bar_->focusFind();
    }
}

void IdeShaderEditor::closeFind() {
    if (!findOpen()) {
        return;
    }
    find_bar_->setVisible(false);
    find_bar_->performLayout(0, 0, 0, 0);
    if (jadefx::Scene* scene = getScene()) {
        scene->releaseFocus(find_bar_.get());
    }
    paint(area_->getText());
    focus();
}

void IdeShaderEditor::refresh_find(const std::string& text) {
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

int IdeShaderEditor::current_find() const {
    if (!area_ || find_matches_.empty() || area_->selections().size() != 1) {
        return -1;
    }
    const jadefx::IndexRange range = area_->selection();
    for (std::size_t i = 0; i < find_matches_.size(); ++i) {
        if (find_matches_[i].start == range.start && find_matches_[i].end == range.end) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void IdeShaderEditor::show_find_count() {
    if (findOpen()) {
        find_bar_->showCount(current_find(), static_cast<int>(find_matches_.size()), find_capped_, find_error_);
    }
}

void IdeShaderEditor::find_changed() {
    // As the find text grows, the match stays where the last one started.
    const int from = area_->selection().start;
    paint(area_->getText());
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

void IdeShaderEditor::select_find(std::size_t index) {
    if (index >= find_matches_.size()) {
        return;
    }
    const TextMatch& match = find_matches_[index];
    area_->selectRange(match.start, match.end);
    area_->showPosition(match.start);
    paint(area_->getText());
}

void IdeShaderEditor::step_find(int direction) {
    if (find_matches_.empty()) {
        return;
    }
    const std::size_t count = find_matches_.size();
    const int current = current_find();
    std::size_t index = 0;
    if (current >= 0) {
        index = (static_cast<std::size_t>(current) + count + (direction > 0 ? 1 : count - 1)) % count;
    } else if (direction > 0) {
        const int from = area_->selection().end;
        for (std::size_t i = 0; i < count; ++i) {
            if (find_matches_[i].start >= from) {
                index = i;
                break;
            }
        }
    } else {
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

void IdeShaderEditor::replace_find() {
    if (!findOpen() || !area_->isEditable()) {
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
    area_->replaceText(match.start, match.end, inserted);
    const int after = match.start + CodePoints(inserted);
    area_->moveTo(after);
    for (std::size_t i = 0; i < find_matches_.size(); ++i) {
        if (find_matches_[i].start >= after) {
            select_find(i);
            return;
        }
    }
    show_find_count();
}

void IdeShaderEditor::replace_find_all() {
    if (!findOpen() || !area_->isEditable()) {
        return;
    }
    const TextSearch search(find_bar_->query());
    if (!search.ready()) {
        return;
    }
    apply_replacements(search, search.find_all(area_->getText()), find_bar_->replacement());
}

int IdeShaderEditor::replaceAll(const std::string& query, const std::string& replacement) {
    if (!area_ || !loaded_ || missing_ || !area_->isEditable()) {
        return 0;
    }
    SearchQuery wanted;
    wanted.pattern = query;
    const TextSearch search(wanted);
    if (!search.ready()) {
        return 0;
    }
    return apply_replacements(search, search.find_all(area_->getText()), replacement);
}

int IdeShaderEditor::apply_replacements(const TextSearch& search, const std::vector<TextMatch>& matches,
                                        const std::string& replacement) {
    if (matches.empty()) {
        return 0;
    }
    const std::string text = area_->getText();
    // Last first, so each match's place is still right; one undo step for all.
    area_->asOneStep([&] {
        for (std::size_t i = matches.size(); i-- > 0;) {
            area_->replaceText(matches[i].start, matches[i].end, search.expand(text, matches[i], replacement));
        }
    });
    return static_cast<int>(matches.size());
}

void IdeShaderEditor::markError(const std::string& error) {
    if (error == shownError_ && loaded_) {
        return;
    }
    shownError_ = error;
    const int line = ErrorLine(error);
    errorLine_ = line;
    std::vector<jadefx::TextMark> marks;
    errorStart_ = 0;
    errorEnd_ = 0;
    const std::string text = area_->getText();
    if (line > 0) {
        // The line's code points, start to end, found by its newlines.
        std::size_t begin = 0;
        for (int at = 1; at < line && begin != std::string::npos; ++at) {
            begin = text.find('\n', begin);
            begin = begin == std::string::npos ? begin : begin + 1;
        }
        if (begin != std::string::npos) {
            std::size_t end = text.find('\n', begin);
            end = end == std::string::npos ? text.size() : end;
            const int start = CodePointsBefore(text, begin);
            const int stop = std::max(start + 1, CodePointsBefore(text, end));
            jadefx::TextMark mark;
            mark.start = start;
            mark.end = stop;
            mark.severity = jadefx::TextMarkSeverity::Error;
            marks.push_back(mark);
            errorStart_ = start;
            errorEnd_ = stop;
        }
    }
    area_->setTextMarks(std::move(marks));
    refresh_scroll_marks();
}

void IdeShaderEditor::noteText() {
    if (loading_) {
        return;
    }
    paint(area_->getText());
    dirty_ = true;
    dirtyAt_ = std::chrono::steady_clock::now();
}

void IdeShaderEditor::flush() {
    if (!area_ || missing_ || !loaded_) {
        return;
    }
    dirty_ = false;
    push(area_->getText());
}

void IdeShaderEditor::push(const std::string& text) {
    const std::uint64_t gen = commit_->epoch.fetch_add(1, std::memory_order_relaxed) + 1;
    engine_.on_simulation([commit = commit_, text, gen](engine_core::DataModel& game) {
        if (auto* shader = dynamic_cast<engine_core::Shader*>(game.instance(commit->id))) {
            if (shader->source() != text) {
                // One place waypoint for the buffer, not one per keystroke.
                std::optional<std::string> recording;
                if (!game.simulation_running()) {
                    recording = game.history().try_begin_recording("Edit Shader");
                }
                shader->set_source(text);
                if (recording) {
                    game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
                }
            }
        }
        commit->acked.store(gen, std::memory_order_release);
    });
}

void IdeShaderEditor::reapply() {
    // Every Source, name, and tree change moves one of these, and so does Stop.
    const engine_core::DataModel& game = engine_.datamodel();
    const std::uint64_t authored = game.authored_revision();
    const std::uint64_t tree = game.tree_revision();
    if (authored == seenAuthored_ && tree == seenTree_) {
        return;
    }
    if (commit_->acked.load(std::memory_order_acquire) != commit_->epoch.load(std::memory_order_relaxed)) {
        return;
    }
    if (missing_) {
        loaded_ = false;
        load();
        return;
    }
    std::string text;
    std::string name;
    std::string error;
    bool alive = false;
    std::uint32_t world = 0;
    if (!readSource(text, name, error, alive, &world)) {
        return;
    }
    seenAuthored_ = authored;
    seenTree_ = tree;
    if (!alive) {
        missing_ = true;
        area_->setEditable(false);
        return;
    }
    if (name != shownName_) {
        setTitleText(name);
    }
    const bool stopped = world != world_;
    world_ = world;
    if (text != area_->getText()) {
        if (stopped) {
            // Stop restored the authored place. The buffer holds edits made during
            // play, so it wins and becomes the place's Source.
            flush();
            return;
        }
        // An undo, a redo, or a script changed Source under an idle buffer.
        showSource(std::move(text));
    }
    markError(error);
}

void IdeShaderEditor::showSource(std::string text) {
    const int caret = area_->caretPosition();
    loading_ = true;
    area_->suspendUndo();
    area_->setText(text);
    area_->resumeUndo();
    area_->forgetHistory();
    area_->moveTo(std::min(caret, area_->length()));
    loading_ = false;
    paint(text);
    shownError_.clear();
}

}  // namespace ide
