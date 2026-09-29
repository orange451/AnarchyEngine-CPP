#pragma once

#include "IdePane.hpp"
#include "Pty.hpp"
#include "TerminalView.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace ide {

// What the Terminal page needs from the studio.
struct TerminalHost {
    // Starts the program. Pty::spawn unless a test hands in another.
    std::function<std::unique_ptr<Pty>(PtyOptions, std::string*)> spawn;
    // The folder the shell starts in: the project's. Empty is this process's.
    std::function<std::string()> folder;
};

// The user's shell on a pseudo-terminal, in a page that docks like any other.
// The shell starts at the page's first layout, at its size, and lives as long
// as the page: switching tabs or closing this one leaves it running, so the
// Window menu brings back the same session. When it exits the page says so,
// and Enter starts another. The tab shows the title the program sets.
class IdeTerminal : public IdePane {
public:
    explicit IdeTerminal(TerminalHost host = {});
    ~IdeTerminal() override;

    TerminalView& view() const { return *view_; }
    bool running() const { return pty_ != nullptr; }

protected:
    // Focuses the terminal when its tab is chosen.
    void onOpen() override;

private:
    // What the reader thread hands to the UI thread. The page clears owner
    // when it goes, so a task queued after that does nothing.
    struct Inbox {
        std::mutex mutex;
        std::string output;
        bool exited = false;
        int exit_code = 0;
        bool queued = false;
        IdeTerminal* owner = nullptr;
    };

    void start(int cols, int rows);
    // On the UI thread: output into the screen, then the exit, if any.
    void drain();
    void sync_title();

    TerminalHost host_;
    std::shared_ptr<TerminalView> view_;
    std::shared_ptr<Inbox> inbox_;
    std::unique_ptr<Pty> pty_;
    bool started_ = false;
    std::string shown_title_;
};

}  // namespace ide
