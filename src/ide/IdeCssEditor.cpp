#include "IdeCssEditor.hpp"

#include "CssHighlight.hpp"
#include "EditorFont.hpp"
#include "IdeTheme.hpp"
#include "LockWaits.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "Gui.hpp"

#include <algorithm>
#include <atomic>
#include <optional>
#include <utility>

namespace ide {
namespace {

// The legacy editor saved every half second.
constexpr std::chrono::milliseconds kSaveDelay(500);

// The script editor's syntax colors, so a stylesheet reads like a script.
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
    define("string", "--ide-syntax-string-color", false);
    define("number", "--ide-syntax-number-color", false);
}

}  // namespace

// Enter keeps the indent, and { brings its }.
class IdeCssEditor::Area : public jadefx::CodeArea {
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
        const CssEnter enter = enter_css(getText(), caretPosition(), indent);
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
        const CssBrace brace = brace_css(getText(), at);
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

struct IdeCssEditor::Commit {
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<std::uint64_t> acked{0};
    std::uint32_t id = 0;
};

IdeCssEditor::IdeCssEditor(engine_core::Engine& engine, std::uint32_t id)
    : IdePane("CSS.css", true), engine_(engine), id_(id), commit_(std::make_shared<Commit>()) {
    setIconFile("CSS.png");
    commit_->id = id;
    area_ = std::make_shared<Area>();
    area_->getClassList().add("ide-script");
    (void)editor_mono_family();
    DefineStyles(*area_);
    area_->setStyle("font-family: \"" + editor_font_family() + "\";");
    themeListener_ = std::make_unique<ThemeListener>([this] {
        DefineStyles(*area_);
        area_->setStyle("font-family: \"" + editor_font_family() + "\";");
    });
    area_->setOnPlainTextChange([this](const jadefx::PlainTextChange&) { noteText(); });
    Fill(*area_);
    getChildren().add(area_);
    load();
}

IdeCssEditor::~IdeCssEditor() = default;

std::string IdeCssEditor::text() const { return area_ ? area_->getText() : std::string(); }

void IdeCssEditor::focus() {
    if (area_) {
        area_->requestFocus();
    }
}

void IdeCssEditor::setTitleText(const std::string& name) {
    shownName_ = name;
    setTitle(name + ".css");
}

void IdeCssEditor::onOpen() { focus(); }

void IdeCssEditor::onClose() { flush(); }

void IdeCssEditor::layoutChildren() {
    if (!loaded_) {
        load();
    } else if (dirty_ && std::chrono::steady_clock::now() - dirtyAt_ >= kSaveDelay) {
        flush();
    } else if (!dirty_) {
        reapply();
    }
    StackPane::layoutChildren();
}

bool IdeCssEditor::readSource(std::string& text, std::string& name, bool& alive, std::uint32_t* world) const {
    alive = false;
    engine_core::DataModel& game = engine_.datamodel();
    engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionLockWait);
    if (!lock.owns()) {
        return false;
    }
    const auto* sheet = dynamic_cast<const engine_core::Css*>(game.instance(id_));
    if (sheet == nullptr) {
        return true;
    }
    alive = true;
    text = sheet->source();
    name = game.name(id_);
    if (world != nullptr) {
        *world = game.world_generation();
    }
    return true;
}

void IdeCssEditor::load() {
    std::string text;
    std::string name;
    bool alive = false;
    if (!readSource(text, name, alive, &world_)) {
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
}

void IdeCssEditor::paint(const std::string& text) {
    jadefx::StyleSpansBuilder builder;
    for (const CssSpan& span : highlight_css(text)) {
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

void IdeCssEditor::noteText() {
    if (loading_) {
        return;
    }
    paint(area_->getText());
    dirty_ = true;
    dirtyAt_ = std::chrono::steady_clock::now();
}

void IdeCssEditor::flush() {
    if (!area_ || missing_ || !loaded_) {
        return;
    }
    dirty_ = false;
    push(area_->getText());
}

void IdeCssEditor::push(const std::string& text) {
    const std::uint64_t gen = commit_->epoch.fetch_add(1, std::memory_order_relaxed) + 1;
    engine_.on_simulation([commit = commit_, text, gen](engine_core::DataModel& game) {
        if (auto* sheet = dynamic_cast<engine_core::Css*>(game.instance(commit->id))) {
            if (sheet->source() != text) {
                // One place waypoint for the buffer, not one per keystroke.
                std::optional<std::string> recording;
                if (!game.simulation_running()) {
                    recording = game.history().try_begin_recording("Edit CSS");
                }
                sheet->set_text(engine_core::GuiProperty::Source, text);
                if (recording) {
                    game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
                }
            }
        }
        commit->acked.store(gen, std::memory_order_release);
    });
}

void IdeCssEditor::reapply() {
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
    bool alive = false;
    std::uint32_t world = 0;
    if (!readSource(text, name, alive, &world)) {
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
    if (text == area_->getText()) {
        return;
    }
    if (stopped) {
        // Stop restored the authored place. The buffer holds edits made during
        // play, so it wins and becomes the place's Source.
        flush();
    } else {
        // An undo, a redo, or a script changed Source under an idle buffer.
        showSource(std::move(text));
    }
}

void IdeCssEditor::showSource(std::string text) {
    const int caret = area_->caretPosition();
    loading_ = true;
    area_->suspendUndo();
    area_->setText(text);
    area_->resumeUndo();
    area_->forgetHistory();
    area_->moveTo(std::min(caret, area_->length()));
    loading_ = false;
    paint(text);
}

}  // namespace ide
