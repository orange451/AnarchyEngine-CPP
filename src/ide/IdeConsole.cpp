#include "IdeConsole.hpp"

#include "Engine.hpp"
#include "ScriptRuntime.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
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

IdeConsole::IdeConsole(engine_core::Engine& engine) : IdePane("Console", true), engine_(engine) {
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

    command_ = jadefx::make<jadefx::TextField>();
    command_->setPromptText("Lua Command Line");
    command_->setStyle("width: 100%;");
    command_->setOnAction([this](jadefx::ActionEvent&) { submitCommand(); });

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
    StackPane::layoutChildren();
}

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
    if (!command_) {
        return;
    }
    const std::string source = command_->getText();
    if (source.empty()) {
        return;
    }
    command_->clear();
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

}  // namespace ide
