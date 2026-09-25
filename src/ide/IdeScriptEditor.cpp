#include "IdeScriptEditor.hpp"

#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "LuauHighlight.hpp"
#include "Script.hpp"

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

struct IdeScriptEditor::Commit {
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<std::uint64_t> acked{0};
    engine_core::InstanceId id = 0;
};

IdeScriptEditor::IdeScriptEditor(engine_core::Engine& engine, std::uint32_t id)
    : IdePane("Script.lua", true), engine_(engine), id_(id), commit_(std::make_shared<Commit>()) {
    commit_->id = id;
    area_ = jadefx::make<jadefx::CodeArea>();
    area_->getClassList().add("ide-script");
    // Load before the area is laid out. The stylesheet asks for this family.
    (void)editor_font();
    define_styles(*area_);
    area_->setOnPlainTextChange([this](const jadefx::PlainTextChange&) { note_text(); });
    Fill(*area_);
    getChildren().add(area_);
    load();
}

IdeScriptEditor::~IdeScriptEditor() = default;

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

}  // namespace ide
