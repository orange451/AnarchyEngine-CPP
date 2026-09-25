#pragma once

#include "IdePane.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace engine_core {
class Engine;
}  // namespace engine_core

namespace ide {

// Log of Lua print lines and errors, with a command line under it.
// The command line runs against the live data model, stopped or running.
// A submitted command is shown in full, then run after that line has been drawn.
// Each log row shows the wall time the line was recorded.
// The log clears when a play session starts.
class IdeConsole : public IdePane {
public:
    explicit IdeConsole(engine_core::Engine& engine);

protected:
    void layoutChildren() override;
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;

private:
    struct PendingCommand {
        std::string source;
        std::uint32_t world = 0;
    };

    void pull();
    void submitCommand();
    void runPending();

    engine_core::Engine& engine_;
    std::shared_ptr<jadefx::StyleClassedTextArea> log_;
    std::shared_ptr<jadefx::TextField> command_;
    std::deque<PendingCommand> pending_;
    std::uint64_t epoch_ = 0;
    bool pulling_ = false;
    // Set once this pane has been drawn since the last submit. A paused command
    // runs on the UI thread, so it waits until the submitted line is on screen.
    bool command_painted_ = false;
};

}  // namespace ide
