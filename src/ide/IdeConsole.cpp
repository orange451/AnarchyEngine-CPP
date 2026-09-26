#include "IdeConsole.hpp"

#include "Engine.hpp"
#include "LuauComplete.hpp"
#include "ScriptPairs.hpp"
#include "ScriptRuntime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace ide {
namespace {

struct Editable {
    jadefx::StyledTextArea& area;
    bool previous;
    explicit Editable(jadefx::StyledTextArea& area) : area(area), previous(area.isEditable()) { area.setEditable(true); }
    ~Editable() { area.setEditable(previous); }
};

std::string formatStamp(std::chrono::system_clock::time_point when) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &seconds) != 0) {
        return "00:00:00.000";
    }
#else
    if (localtime_r(&seconds, &local) == nullptr) {
        return "00:00:00.000";
    }
#endif
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()) % 1000;
    int ms = static_cast<int>(millis.count());
    if (ms < 0) {
        ms += 1000;
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec, ms);
    return buffer;
}

void appendVisual(jadefx::StyleClassedTextArea& log, std::string_view message,
                  engine_core::ScriptRuntime::OutputKind kind, std::chrono::system_clock::time_point when) {
    const int origin = log.length();
    log.appendText(formatStamp(when) + " ");
    const int messageAt = log.length();
    log.appendText(std::string(message));
    log.appendText("\n");
    const int end = log.length();
    if (messageAt > origin) {
        log.setStyleClass(origin, messageAt, "time");
    }
    if (end <= messageAt) {
        return;
    }
    if (kind == engine_core::ScriptRuntime::OutputKind::Error) {
        log.setStyleClass(messageAt, end, "error");
    } else if (kind == engine_core::ScriptRuntime::OutputKind::Command) {
        log.setStyleClass(messageAt, end, "command");
    }
}

void appendStamped(jadefx::StyleClassedTextArea& log, const engine_core::ScriptRuntime::OutputLine& line) {
    const std::string_view text = line.text;
    if (text.empty()) {
        appendVisual(log, "", line.kind, line.time);
        return;
    }
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t newline = text.find('\n', begin);
        const std::size_t stop = newline == std::string_view::npos ? text.size() : newline;
        std::string_view segment = text.substr(begin, stop - begin);
        if (!segment.empty() && segment.back() == '\r') {
            segment.remove_suffix(1);
        }
        appendVisual(log, segment, line.kind, line.time);
        if (newline == std::string_view::npos) {
            break;
        }
        begin = newline + 1;
        if (begin == text.size()) {
            break;
        }
    }
}

}  // namespace

class CommandField : public jadefx::TextField {
public:
    IdeConsole* console = nullptr;

    void handleKey(jadefx::KeyEvent& event) override;
    void handleText(jadefx::TextEvent& event) override;
    void handleMousePressed(const jadefx::MouseEvent& event) override;

private:
    bool applyPair(char unit);
    bool applyEnter();
};

IdeConsole::IdeConsole(engine_core::Engine& engine) : IdePane("Console", true), engine_(engine) {
    setIconFile("Console.png");
    setMinSize(80, 64);
    log_ = jadefx::make<jadefx::StyleClassedTextArea>();
    log_->setEditable(false);
    log_->setWrapText(true);
    log_->setShowCaret(jadefx::StyledTextArea::CaretVisibility::Off);
    log_->setFollowCaret(true);
    // New lines take the plain style. Error ranges are painted after they are inserted,
    // so a following print does not inherit the red.
    log_->setUseInitialStyleForInsertion(true);
    log_->suspendUndo();
    jadefx::TextStyle error;
    error.hasFill = true;
    error.fill = jadefx::Color::rgb8(176, 0, 32);
    log_->defineStyleClass("error", error);
    jadefx::TextStyle command;
    command.hasFill = true;
    command.fill = jadefx::Color::rgb8(18, 78, 148);
    log_->defineStyleClass("command", command);
    jadefx::TextStyle time;
    time.hasFill = true;
    time.fill = jadefx::Color::rgb8(120, 124, 130);
    log_->defineStyleClass("time", time);
    Fill(*log_);

    auto field = std::make_shared<CommandField>();
    field->console = this;
    command_ = field;
    command_->setPromptText("Lua Command Line");
    command_->setStyle("width: 100%;");
    command_->setOnAction([this](jadefx::ActionEvent&) { submitCommand(); });
    completion_.setOnAccept([this] { accept_completion(true); });

    // The log takes the page left after the command line, so a splitter drag
    // grows and shrinks the log instead of leaving a gap or pushing the field out.
    auto column = jadefx::make<jadefx::BorderPane>();
    Fill(*column);
    column->setCenter(log_);
    column->setBottom(command_);
    getChildren().add(column);
}

void IdeConsole::layoutChildren() {
    if (!pulling_) {
        pulling_ = true;
        pull();
        // A paused chunk runs here, on the UI thread. Wait until renderContent has
        // drawn the submitted line and the previous frame has been swapped.
        if (!pending_.empty() && (command_painted_ || !isVisible())) {
            runPending();
            pull();
        }
        pulling_ = false;
    }
    if (completion_.isOpen() && command_) {
        double x = 0;
        double y = 0;
        double height = 0;
        if (command_->caretBounds(x, y, height)) {
            completion_.moveTo(*command_, x, y, height);
        }
    }
    StackPane::layoutChildren();
}

void IdeConsole::onClose() { completion_.dismiss(); }

void IdeConsole::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    command_painted_ = true;
    IdePane::renderContent(renderer, opacity);
}

void IdeConsole::pull() {
    if (!log_) {
        return;
    }
    const engine_core::ScriptRuntime::OutputBatch batch = engine_.scripts().drain_output();
    if (batch.epoch != epoch_) {
        log_->clear();
        epoch_ = batch.epoch;
    }
    if (batch.lines.empty()) {
        return;
    }
    Editable editing(*log_);
    for (const engine_core::ScriptRuntime::OutputLine& line : batch.lines) {
        appendStamped(*log_, line);
    }
}

void IdeConsole::submitCommand() {
    completion_.dismiss();
    if (!command_) {
        return;
    }
    const std::string source = command_->getText();
    if (source.empty()) {
        return;
    }
    command_->clear();
    noteCommandEdit();
    const std::uint32_t world = engine_.datamodel().world_generation();
    // Show the whole command before Lua runs. The next painted frame draws this
    // line; layout after that paint is what calls runPending.
    engine_.scripts().append_output(engine_core::ScriptRuntime::OutputKind::Command, source);
    pending_.push_back(PendingCommand{source, world});
    command_painted_ = false;
    if (!pulling_) {
        pulling_ = true;
        pull();
        pulling_ = false;
    }
}

void IdeConsole::runPending() {
    std::deque<PendingCommand> batch;
    batch.swap(pending_);
    for (const PendingCommand& command : batch) {
        // Runs now when the simulation is paused, and on the next step while it is playing.
        // A stop that lands first changes the world, and that command is dropped.
        try {
            engine_.on_simulation([scripts = &engine_.scripts(), source = command.source,
                                   world = command.world](engine_core::DataModel& model) {
                if (model.world_generation() != world) {
                    return;
                }
                scripts->run_chunk(source);
            });
        } catch (const std::exception& ex) {
            engine_.scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error, ex.what());
        }
    }
}

void IdeConsole::refresh_completion(bool force) {
    if (!command_ || completion_.accepting()) {
        return;
    }
    if (command_->getAnchor() != command_->getCaretPosition()) {
        completion_.dismiss();
        return;
    }
    double x = 0;
    double y = 0;
    double height = 0;
    if (!command_->caretBounds(x, y, height)) {
        completion_.dismiss();
        return;
    }
    const std::string text = command_->getText();
    completion_.present(complete_luau(text, command_->getCaretPosition(), completion_world(engine_, 0, nullptr), 0, false),
                        force, *command_, x, y, height);
}

void IdeConsole::accept_completion(bool parentheses) {
    if (!command_) {
        completion_.dismiss();
        return;
    }
    const std::optional<CompletionEdit> edit = completion_.take(parentheses, command_->getText());
    if (!edit) {
        return;
    }
    command_->selectRange(edit->begin, edit->end);
    command_->replaceSelection(edit->text);
    command_->positionCaret(edit->caret);
    noteCommandEdit();
    command_->requestFocus();
    completion_.finish();
}

void IdeConsole::bindUndo(TextUndoStack* stack) {
    undo_stack_ = stack;
    if (undo_stack_ != nullptr && command_) {
        undo_stack_->reset(command_->getText());
    }
}

bool IdeConsole::commandFocused(const jadefx::Node* node) const {
    for (const jadefx::Node* cursor = node; cursor != nullptr; cursor = cursor->getParent()) {
        if (cursor == command_.get()) {
            return true;
        }
    }
    return false;
}

void IdeConsole::noteCommandEdit() {
    if (mute_undo_ || undo_stack_ == nullptr || command_ == nullptr) {
        return;
    }
    const std::string& next = command_->getText();
    const std::string& previous = undo_stack_->text();
    if (next == previous) {
        return;
    }
    std::size_t start = 0;
    while (start < previous.size() && start < next.size() && previous[start] == next[start]) {
        ++start;
    }
    std::size_t previous_end = previous.size();
    std::size_t next_end = next.size();
    while (previous_end > start && next_end > start && previous[previous_end - 1] == next[next_end - 1]) {
        --previous_end;
        --next_end;
    }
    while (start > 0 && (static_cast<unsigned char>(previous[start]) & 0xC0u) == 0x80u) {
        --start;
    }
    while (previous_end < previous.size() && next_end < next.size() &&
           (static_cast<unsigned char>(previous[previous_end]) & 0xC0u) == 0x80u) {
        ++previous_end;
        ++next_end;
    }
    undo_stack_->replace(start, previous_end - start, next.substr(start, next_end - start));
}

void IdeConsole::applyUndoText() {
    if (!command_ || undo_stack_ == nullptr || command_->getText() == undo_stack_->text()) {
        return;
    }
    mute_undo_ = true;
    command_->setText(undo_stack_->text());
    command_->positionCaret(undo_stack_->caret());
    mute_undo_ = false;
}

void CommandField::handleKey(jadefx::KeyEvent& event) {
    if (console == nullptr || !event.pressed || isDisabled()) {
        jadefx::TextField::handleKey(event);
        return;
    }
    struct NoteEdit {
        IdeConsole* console;
        ~NoteEdit() { console->noteCommandEdit(); }
    } note{console};
    if (event.shortcut() && event.key == jadefx::Key::Space) {
        console->refresh_completion(true);
        event.consume();
        return;
    }
    if (console->completion_.isOpen()) {
        if ((event.key == jadefx::Key::Up || event.key == jadefx::Key::Down) && !event.shortcut()) {
            console->completion_.move(event.key == jadefx::Key::Down ? 1 : -1);
            event.consume();
            return;
        }
        if ((event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter || event.key == jadefx::Key::Tab) &&
            !event.shift && !event.shortcut()) {
            if (console->completion_.keyAccepts()) {
                console->accept_completion(true);
                event.consume();
                return;
            }
            console->completion_.dismiss();
        }
        if (event.key == jadefx::Key::Escape) {
            console->completion_.dismiss();
            event.consume();
            return;
        }
        if (event.key == jadefx::Key::Left || event.key == jadefx::Key::Right || event.key == jadefx::Key::Home ||
            event.key == jadefx::Key::End) {
            console->completion_.dismiss();
        }
    }
    const bool plain_enter = (event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter) && !event.shift &&
                             !event.shortcut() && !event.repeat;
    if (plain_enter && applyEnter()) {
        event.consume();
        if (!console->completion_.accepting()) {
            console->refresh_completion(false);
        }
        return;
    }
    const std::string before = getText();
    jadefx::TextField::handleKey(event);
    if (getText() != before) {
        console->refresh_completion(false);
    }
}

bool CommandField::applyPair(char unit) {
    if (!isEditable() || isDisabled()) {
        return false;
    }
    const int anchor = getAnchor();
    const int caret = getCaretPosition();
    const int begin = std::min(anchor, caret);
    const int end = std::max(anchor, caret);
    const PairResult pair =
        pair_luau(getText(), begin, end, static_cast<char32_t>(static_cast<unsigned char>(unit)));
    if (pair.action == PairAction::None) {
        return false;
    }
    if (pair.action == PairAction::Skip) {
        if (console != nullptr) {
            console->completion_.dismiss();
        }
        positionCaret(end + 1);
        return true;
    }
    if (pair.action == PairAction::Insert) {
        std::string both;
        both.push_back(pair.open);
        both.push_back(pair.close);
        selectRange(begin, begin);
        replaceSelection(both);
        positionCaret(begin + 1);
        return true;
    }
    const std::string selected = getSelectedText();
    std::string wrapped;
    wrapped.push_back(pair.open);
    wrapped += selected;
    wrapped.push_back(pair.close);
    selectRange(begin, end);
    replaceSelection(wrapped);
    return true;
}

bool CommandField::applyEnter() {
    if (!isEditable() || isDisabled() || getAnchor() != getCaretPosition()) {
        return false;
    }
    const EnterResult result = enter_luau(getText(), getCaretPosition(), 4, true, true);
    if (!result.insert) {
        return false;
    }
    selectRange(result.begin, result.end);
    replaceSelection(result.text);
    positionCaret(result.caret);
    return true;
}

void CommandField::handleText(jadefx::TextEvent& event) {
    struct NoteEdit {
        IdeConsole* console;
        ~NoteEdit() {
            if (console != nullptr) {
                console->noteCommandEdit();
            }
        }
    } note{console};
    if (console != nullptr && event.text.size() == 1 && !isDisabled()) {
        const char unit = event.text[0];
        const bool name_key = unit == '.' || unit == ':' || unit == '(';
        const bool quote_key = unit == '"' || unit == '\'';
        const bool collapsed = getAnchor() == getCaretPosition();
        const bool steps_over = collapsed && quote_key &&
                                source_code_point(getText(), getCaretPosition()) ==
                                    static_cast<char32_t>(static_cast<unsigned char>(unit));
        if (name_key && console->completion_.commitsName()) {
            console->accept_completion(false);
        } else if (quote_key && console->completion_.commitsQuote(unit, !steps_over)) {
            console->accept_completion(false);
            if (steps_over) {
                event.consume();
                if (!console->completion_.accepting()) {
                    console->refresh_completion(false);
                }
                return;
            }
        }
        if (applyPair(unit)) {
            event.consume();
            if (!console->completion_.accepting()) {
                console->refresh_completion(false);
            }
            return;
        }
    }
    jadefx::TextField::handleText(event);
    if (console != nullptr && !console->completion_.accepting()) {
        console->refresh_completion(false);
    }
}

void CommandField::handleMousePressed(const jadefx::MouseEvent& event) {
    if (console != nullptr) {
        console->completion_.dismiss();
    }
    jadefx::TextField::handleMousePressed(event);
}

}  // namespace ide
