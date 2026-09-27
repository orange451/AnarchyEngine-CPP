#include "IdeScriptEditor.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "LuaSource.hpp"
#include "LuauComplete.hpp"
#include "LuauHighlight.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptMarks.hpp"
#include "ScriptPairs.hpp"
#include "TextWrap.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>

namespace ide {
namespace {

constexpr std::chrono::milliseconds kSaveDelay(50);
constexpr std::chrono::milliseconds kLockWait(5);

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
                loaded = true;
                return;
            }
        }
    }

    bool loaded = false;
};

const EditorFont& editor_font() {
    static const EditorFont font;
    return font;
}

void define_styles(jadefx::StyleClassedTextArea& area) {
    jadefx::TextStyle keyword;
    keyword.hasFill = true;
    keyword.fill = jadefx::Color::parse("#7a3e9d");
    keyword.bold = true;
    area.defineStyleClass("keyword", keyword);

    jadefx::TextStyle builtin;
    builtin.hasFill = true;
    builtin.fill = jadefx::Color::parse("#0b6e84");
    builtin.bold = true;
    area.defineStyleClass("builtin", builtin);

    // Instance, Vector3, Enum: types, so they read apart from functions like print.
    jadefx::TextStyle datatype;
    datatype.hasFill = true;
    datatype.fill = jadefx::Color::parse("#a3470a");
    datatype.bold = true;
    area.defineStyleClass("datatype", datatype);

    jadefx::TextStyle comment;
    comment.hasFill = true;
    comment.fill = jadefx::Color::parse("#6a737d");
    area.defineStyleClass("comment", comment);

    jadefx::TextStyle stringStyle;
    stringStyle.hasFill = true;
    stringStyle.fill = jadefx::Color::parse("#0a7d33");
    area.defineStyleClass("string", stringStyle);

    jadefx::TextStyle number;
    number.hasFill = true;
    number.fill = jadefx::Color::parse("#0550ae");
    area.defineStyleClass("number", number);
}

std::string lua_title(const std::string& name) {
    if (name.empty()) {
        return "Script.lua";
    }
    return name + ".lua";
}

}  // namespace

class ScriptCodeArea : public jadefx::CodeArea {
public:
    IdeScriptEditor* editor = nullptr;

    void handleKey(jadefx::KeyEvent& event) override;
    void handleText(jadefx::TextEvent& event) override;
    bool applyPair(char unit);
    bool applyEnter();
    void handleMousePressed(const jadefx::MouseEvent& event) override;
    void handleMouseMoved(const jadefx::MouseEvent& event) override;
    void handleScroll(jadefx::ScrollEvent& event) override;

    void tickHover();
    void dismissHover();
    // Replaces the squiggles. Returns true when the set changed.
    bool setProblems(std::vector<ScriptMark> marks);
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

    status_ = jadefx::make<jadefx::Label>("");
    status_->setAlignment(jadefx::Pos::CenterLeft);
    status_->setMouseTransparent(true);
    status_->setVisible(false);
    status_->setPadding(jadefx::Insets{3, 8, 3, 8});
    status_->setStyle("font-size: 12px;");
    status_->setMinSize(0, 0);
    status_->setPrefHeight(0);
    status_->setMaxSize(100000, 0);

    auto column = jadefx::make<jadefx::BorderPane>();
    Fill(*column);
    column->setCenter(area_);
    column->setBottom(status_);
    getChildren().add(column);
    load();
}

void IdeScriptEditor::setOnTitle(std::function<void(const std::string&)> handler) {
    on_title_ = std::move(handler);
    if (on_title_ && loaded_) {
        on_title_(lua_title(shown_name_));
    }
}

void IdeScriptEditor::setTitleText(const std::string& name) {
    shown_name_ = name;
    if (on_title_) {
        on_title_(lua_title(name));
    }
}

std::string IdeScriptEditor::text() const { return area_ ? area_->getText() : std::string(); }

void IdeScriptEditor::focus() {
    if (area_) {
        area_->requestFocus();
    }
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

IdeScriptEditor::~IdeScriptEditor() { engine_.analysis().unwatch(id_); }

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
    } else {
        std::string text;
        std::string name;
        bool alive = false;
        if (read_source(text, name, alive) && alive && name != shown_name_) {
            setTitleText(name);
        }
    }
    if (completion_open()) {
        place_completion();
        if (area_) {
            static_cast<ScriptCodeArea*>(area_.get())->dismissHover();
        }
    } else if (area_) {
        static_cast<ScriptCodeArea*>(area_.get())->tickHover();
    }
    refresh_marks();
    StackPane::layoutChildren();
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
    if (!area_ || missing_) {
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
}

void IdeScriptEditor::paint() {
    if (!area_) {
        return;
    }
    jadefx::StyleSpansBuilder builder;
    for (const LuauSpan& span : highlight_luau(area_->getText())) {
        jadefx::TextStyle style;
        if (span.style != nullptr) {
            style.styleClass = span.style;
        }
        builder.add(style, span.length);
    }
    area_->suspendUndo();
    area_->setStyleSpans(0, builder.create());
    area_->resumeUndo();
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
        status.setBackground(jadefx::Color::transparent());
        return;
    }
    if (status.isVisible() && status.getText() == summary.text) {
        return;
    }
    status.setVisible(true);
    status.setText(summary.text);
    status.setPrefHeight(22);
    status.setMaxSize(100000, 28);
    jadefx::Color fill = jadefx::Color::rgb8(92, 101, 112);
    jadefx::Color back = jadefx::Color::rgb8(243, 244, 246);
    if (summary.blocks_compile || summary.severity == engine_core::Severity::Error) {
        fill = jadefx::Color::rgb8(176, 0, 32);
        back = jadefx::Color::rgb8(253, 236, 234);
    } else if (summary.severity == engine_core::Severity::Warning) {
        fill = jadefx::Color::rgb8(138, 90, 0);
        back = jadefx::Color::rgb8(255, 244, 214);
    }
    status.setTextFill(fill);
    status.setBackground(back);
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
    }
    show_banner(*status_, summarize_problems(diagnostics));
}

void IdeScriptEditor::note_text() {
    if (loading_ || !area_) {
        return;
    }
    static_cast<ScriptCodeArea*>(area_.get())->dismissHover();
    paint();
    if (!completion_.accepting()) {
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
    if (!area_ || missing_ || !loaded_ || !commit_) {
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
    completion_.present(complete_luau(area_->getText(), area_->caretPosition(), world(), id_), force, *area_, bounds.x,
                        bounds.y, bounds.height);
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
    const jadefx::CharacterHit where = hit(event.x, event.y);
    if (!where.valid) {
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

char32_t CodePointAt(std::string_view text, int index) {
    int count = 0;
    for (std::size_t cursor = 0; cursor < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[cursor]);
        std::size_t step = 1;
        char32_t code = lead;
        if (lead >= 0x80) {
            if ((lead & 0xE0) == 0xC0) {
                step = 2;
                code = lead & 0x1F;
            } else if ((lead & 0xF0) == 0xE0) {
                step = 3;
                code = lead & 0x0F;
            } else {
                step = 4;
                code = lead & 0x07;
            }
            for (std::size_t i = 1; i < step && cursor + i < text.size(); ++i) {
                code = (code << 6) | (static_cast<unsigned char>(text[cursor + i]) & 0x3F);
            }
        }
        if (count == index) {
            return code;
        }
        if (cursor + step > text.size()) {
            break;
        }
        cursor += step;
        ++count;
    }
    return 0;
}

bool NameChar(char32_t code) {
    return (code >= U'A' && code <= U'Z') || (code >= U'a' && code <= U'z') || code == U'_' ||
           (code >= U'0' && code <= U'9') || code >= 0x80;
}

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
            "background-color: #ffffff; border-style: solid; border-width: 1px; border-color: #c5c8ce; "
            "box-shadow: 0 2px 8px rgba(32, 33, 36, 0.16);");
    }
    tip_->getChildren().clear();
    const jadefx::Color body_fill = jadefx::Color::parse("#5c6570");
    // A Label draws one line and cuts off what does not fit, so long text is
    // wrapped here, one Label per line. The tip is never wider than
    // kTipMaxWidth, or than the window less a margin.
    double wrap_width = kTipMaxWidth;
    if (const jadefx::Scene* scene = getScene(); scene != nullptr && scene->getWidth() > 0) {
        wrap_width = std::min(wrap_width, scene->getWidth() - kTipMargin * 2);
    }
    wrap_width = std::max(wrap_width - kTipPaddingX * 2, 80.0);
    // Measured and drawn in the same font: each line pins it, so a style cannot change it later.
    const jadefx::Font font = jadefx::Label().getFont();
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
        jadefx::Color title = jadefx::Color::rgb8(176, 0, 32);
        if (mark->severity == engine_core::Severity::Warning) {
            title = jadefx::Color::rgb8(138, 90, 0);
        } else if (mark->severity != engine_core::Severity::Error) {
            title = jadefx::Color::parse("#1f2328");
        }
        const std::string heading = mark->message.empty() ? mark->code : mark->message;
        showTip(heading, problem_detail(*mark), "", title);
        return;
    }
    const HoverInfo info = hover_luau(getText(), hover_index_, editor->world(), editor->id_);
    if (!info.found || info.title.empty()) {
        hover_waiting_ = false;
        return;
    }
    hover_begin_ = info.begin;
    hover_end_ = info.end;
    showTip(info.title, info.detail, info.summary, jadefx::Color::parse("#1f2328"));
}

void ScriptCodeArea::tickHover() {
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
