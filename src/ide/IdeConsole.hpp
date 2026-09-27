#pragma once

#include "CompletionPopup.hpp"
#include "ConsoleLog.hpp"
#include "IdePane.hpp"
#include "ide/TextUndoStack.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace engine_core {
class Engine;
}  // namespace engine_core

namespace ide {

class CommandField;

// Log of Lua print lines and errors, with a command line under it.
// The command line runs against the live data model, stopped or running.
// A submitted command is shown in full, then run after that line has been drawn.
// Each log row shows the wall time the line was recorded. A printed table opens in place.
// The log clears when a play session starts, and from Clear Output: a right-click
// on the log, or Cmd+K (Ctrl+K elsewhere) while the log or the command line has focus.
class IdeConsole : public IdePane {
    friend class CommandField;

public:
    explicit IdeConsole(engine_core::Engine& engine);

    void bindUndo(TextUndoStack* stack);
    bool commandFocused(const jadefx::Node* node) const;
    void applyUndoText();
    void noteCommandEdit();
    // Empties the log. Output printed before this does not come back.
    void clearOutput();
    // Null until the log is right-clicked.
    jadefx::Menu* contextMenu() const { return menu_.get(); }
    ConsoleLog& log() const { return *log_; }

protected:
    void layoutChildren() override;
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void handleKey(jadefx::KeyEvent& event) override;
    void onClose() override;

private:
    struct PendingCommand {
        std::string source;
        std::uint32_t world = 0;
    };

    void pull();
    void showMenu(double x, double y);
    void submitCommand();
    void runPending();
    void refresh_completion(bool force);
    void accept_completion(bool parentheses);

    engine_core::Engine& engine_;
    std::shared_ptr<ConsoleLog> log_;
    std::shared_ptr<jadefx::TextField> command_;
    std::shared_ptr<jadefx::Menu> menu_;
    CompletionPopup completion_;
    std::deque<PendingCommand> pending_;
    std::uint64_t epoch_ = 0;
    bool pulling_ = false;
    // Set once this pane has been drawn since the last submit. A paused command
    // runs on the UI thread, so it waits until the submitted line is on screen.
    bool command_painted_ = false;
    TextUndoStack* undo_stack_ = nullptr;
    bool mute_undo_ = false;
};

}  // namespace ide
