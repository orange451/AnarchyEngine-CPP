#include "IdeConsole.hpp"

#include "Engine.hpp"
#include "ScriptRuntime.hpp"

#include <exception>
#include <string>

namespace ide {
namespace {

struct Editable {
    jadefx::StyledTextArea& area;
    bool previous;
    explicit Editable(jadefx::StyledTextArea& area) : area(area), previous(area.isEditable()) { area.setEditable(true); }
    ~Editable() { area.setEditable(previous); }
};

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
        pulling_ = false;
    }
    StackPane::layoutChildren();
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
        const int start = log_->length();
        log_->appendText(line.text);
        const int end = log_->length();
        if (line.kind == engine_core::ScriptRuntime::OutputKind::Error && end > start) {
            log_->setStyleClass(start, end, "error");
        }
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
    // Runs now when the simulation is paused, and on the next step while it is playing.
    // A stop that lands first changes the world, and that command is dropped.
    const std::uint32_t world = engine_.datamodel().world_generation();
    try {
        engine_.on_simulation([scripts = &engine_.scripts(), source, world](engine_core::DataModel& model) {
            if (model.world_generation() != world) {
                return;
            }
            scripts->run_chunk(source);
        });
    } catch (const std::exception& ex) {
        engine_.scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error, ex.what());
    }
    if (!pulling_) {
        pulling_ = true;
        pull();
        pulling_ = false;
    }
}

}  // namespace ide
