#include "IdeScriptEditor.hpp"

#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "LuauComplete.hpp"
#include "LuauHighlight.hpp"
#include "Script.hpp"

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
    void handleMousePressed(const jadefx::MouseEvent& event) override;
    void handleScroll(jadefx::ScrollEvent& event) override;
};

struct IdeScriptEditor::Commit {
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<std::uint64_t> acked{0};
    engine_core::InstanceId id = 0;
};

IdeScriptEditor::IdeScriptEditor(engine_core::Engine& engine, std::uint32_t id)
    : IdePane("Script.lua", true), engine_(engine), id_(id), commit_(std::make_shared<Commit>()) {
    commit_->id = id;
    auto area = std::make_shared<ScriptCodeArea>();
    area->editor = this;
    area_ = area;
    area_->getClassList().add("ide-script");
    // Load before the area is laid out. The stylesheet asks for this family.
    (void)editor_font();
    define_styles(*area_);
    area_->setOnPlainTextChange([this](const jadefx::PlainTextChange&) { note_text(); });
    completion_.setOnAccept([this] { accept_completion(true); });
    Fill(*area_);
    getChildren().add(area_);
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

void IdeScriptEditor::onOpen() { focus(); }

void IdeScriptEditor::onClose() { flush(); }

void IdeScriptEditor::layoutChildren() {
    if (!loaded_) {
        load();
    } else if (dirty_ && std::chrono::steady_clock::now() - dirty_at_ >= kSaveDelay) {
        flush();
    } else if (!dirty_ && engine_.paused()) {
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
    }
    StackPane::layoutChildren();
}

bool IdeScriptEditor::read_source(std::string& text, std::string& name, bool& alive) const {
    alive = false;
    engine_core::DataModel& model = engine_.datamodel();
    engine_core::DataModelLock lock(model, engine_core::DataModelLock::Read, kLockWait);
    if (!lock.owns()) {
        return false;
    }
    const engine_core::DataModel* object = model.instance(id_);
    const auto* source = dynamic_cast<const engine_core::LuaSource*>(object);
    if (source == nullptr) {
        return true;
    }
    alive = true;
    text = source->source();
    name = model.name(id_);
    return true;
}

void IdeScriptEditor::load() {
    if (!area_ || missing_) {
        return;
    }
    std::string text;
    std::string name;
    bool alive = false;
    if (!read_source(text, name, alive)) {
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

void IdeScriptEditor::note_text() {
    if (loading_ || !area_) {
        return;
    }
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
    engine_.on_simulation([commit, text, gen](engine_core::DataModel& model) {
        if (auto* source = dynamic_cast<engine_core::LuaSource*>(model.instance(commit->id))) {
            if (source->source() != text) {
                source->set_source(text);
                if (!model.simulation_running()) {
                    model.capture_place();
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
    if (!read_source(text, name, alive)) {
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
    if (text != area_->getText()) {
        flush();
    }
}

std::vector<engine_core::LuaNode> IdeScriptEditor::world() const {
    const std::string text = area_ ? area_->getText() : std::string();
    return completion_world(engine_, id_, area_ ? &text : nullptr);
}

bool IdeScriptEditor::completion_open() const { return completion_.isOpen(); }

bool IdeScriptEditor::completion_commits_name() const { return completion_.commitsName(); }

bool IdeScriptEditor::completion_commits_quote(char quote) const { return completion_.commitsQuote(quote); }

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
    jadefx::CodeArea::handleKey(event);
}

void ScriptCodeArea::handleText(jadefx::TextEvent& event) {
    if (editor != nullptr && event.text.size() == 1) {
        const char unit = event.text[0];
        if ((unit == '.' || unit == ':' || unit == '(') && editor->completion_commits_name()) {
            editor->accept_completion(false);
        } else if ((unit == '"' || unit == '\'') && editor->completion_commits_quote(unit)) {
            editor->accept_completion(false);
        }
    }
    jadefx::CodeArea::handleText(event);
}

void ScriptCodeArea::handleMousePressed(const jadefx::MouseEvent& event) {
    if (editor != nullptr) {
        editor->dismiss_completion();
    }
    jadefx::CodeArea::handleMousePressed(event);
}

void ScriptCodeArea::handleScroll(jadefx::ScrollEvent& event) {
    jadefx::CodeArea::handleScroll(event);
    if (editor != nullptr && editor->completion_open()) {
        editor->place_completion();
    }
}

}  // namespace ide
