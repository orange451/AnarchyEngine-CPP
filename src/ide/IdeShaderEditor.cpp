#include "IdeShaderEditor.hpp"

#include "EditorFont.hpp"
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
#include <optional>
#include <utility>

namespace ide {
namespace {

// As the CSS editor: Source is written half a second after the last keystroke.
constexpr std::chrono::milliseconds kSaveDelay(500);

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
    void handleKey(jadefx::KeyEvent& event) override {
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
    StackPane::layoutChildren();
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
    jadefx::StyleSpansBuilder builder;
    for (const GlslSpan& span : highlight_glsl(text)) {
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

void IdeShaderEditor::markError(const std::string& error) {
    if (error == shownError_ && loaded_) {
        return;
    }
    shownError_ = error;
    const int line = ErrorLine(error);
    errorLine_ = line;
    std::vector<jadefx::TextMark> marks;
    std::vector<jadefx::ScrollMark> scroll;
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
            scroll.push_back({start, stop, theme_color("--error-color"), jadefx::ScrollMarkLane::Right});
        }
    }
    area_->setTextMarks(std::move(marks));
    area_->setScrollMarks(std::move(scroll));
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
