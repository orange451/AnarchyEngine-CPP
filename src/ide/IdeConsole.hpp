#pragma once

#include "CompletionPopup.hpp"
#include "LuauComplete.hpp"
#include "ConsoleLog.hpp"
#include "IdePane.hpp"
#include "ide/TextUndoStack.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {
class Engine;
}  // namespace engine_core

namespace ide {

class CommandField;

// Log of Lua print lines and errors, with a command line under it.
// The command line runs against the live data model, stopped or running.
// A submitted command is shown in full, then run after that line has been drawn.
// Up and Down step through the commands submitted before, as in a shell.
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
    // Moves through the history, older for -1 and newer for 1. Past the newest
    // entry, the line typed before browsing comes back.
    void browseHistory(int step);
    void runPending();
    // Runs the submitted commands at the start of the next frame, after the
    // frame that shows them is on screen. Never from inside layout.
    void queueRun();
    void refresh_completion(bool force);
    // Shows Luau's list for the last keystroke, once it has arrived.
    void take_luau_list();
    void accept_completion(bool parentheses);

    engine_core::Engine& engine_;
    std::shared_ptr<ConsoleLog> log_;
    std::shared_ptr<jadefx::TextField> command_;
    std::shared_ptr<jadefx::Menu> menu_;
    CompletionPopup completion_;
    // Luau's list for the last keystroke, on its way.
    std::optional<PendingCompletion> luau_list_;
    std::deque<PendingCommand> pending_;
    std::vector<std::string> history_;
    // history_.size() while not browsing.
    std::size_t history_at_ = 0;
    std::string history_draft_;
    std::uint64_t epoch_ = 0;
    bool pulling_ = false;
    bool run_queued_ = false;
    // A queued run checks this first; it is gone once the console is.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    TextUndoStack* undo_stack_ = nullptr;
    bool mute_undo_ = false;
};

}  // namespace ide
