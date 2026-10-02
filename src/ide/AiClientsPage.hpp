#pragma once

#include "McpSetup.hpp"
#include "RunProcess.hpp"

#include "jadefx/jadefx.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ide {

// The Preferences window's AI tab. A checkbox turns the studio's MCP server on
// and off, unless an ANARCHY_MCP variable decides, and a Claude Code row
// registers the anarchy-mcp bridge with the claude CLI, or takes it out, and
// shows which bridge Claude Code has. The CLI runs off the UI thread.
class AiClientsPage : public jadefx::VBox {
public:
    // The studio's server.
    struct Server {
        // Whether it should run, and what decided.
        std::function<McpSwitch()> setting;
        // Remembers the checkbox, then starts or stops the server.
        std::function<void(bool on)> set_enabled;
        // Where it listens, that it is off, or why it could not start.
        std::function<std::string()> status;
    };
    // Claude Code, and how to reach it.
    struct ClaudeCode {
        // The claude CLI. Empty when it was not found.
        std::filesystem::path cli;
        // anarchy-mcp beside the studio. Empty when it is not there.
        std::filesystem::path bridge;
        // Runs the CLI with args and waits. run_process with cli by default.
        std::function<ProcessResult(const std::vector<std::string>& args)> run;
        // Calls work away from the UI thread, then done on it. A thread and
        // jadefx::runLater by default.
        std::function<void(std::function<void()> work, std::function<void()> done)> async;
    };

    AiClientsPage(Server server, ClaudeCode claude);
    ~AiClientsPage() override;

    // What the controls do.
    void set_server_enabled(bool on);
    // Asks claude what it has registered as anarchy.
    void refresh_claude();
    // Registers the bridge, replacing another anarchy registration.
    void connect_claude();
    void disconnect_claude();
    // Puts the command that registers the bridge on the clipboard.
    void copy_command();

    // What the tab shows, for tests.
    jadefx::CheckBox* server_box() const { return server_box_.get(); }
    const std::string& server_status() const { return server_status_text_; }
    ClaudeRegistration registration() const { return registration_; }
    const std::string& claude_status() const { return claude_status_text_; }
    const std::string& claude_message() const { return claude_message_text_; }
    jadefx::Button* connect_button() const { return connect_.get(); }
    jadefx::Button* disconnect_button() const { return disconnect_.get(); }
    // True while the CLI runs.
    bool busy() const { return busy_; }

private:
    struct Outcome;

    void build();
    void refresh_server();
    // Runs steps of CLI arguments in order, stopping at a failure, then asks
    // what is registered, and shows both. failure names what a failed step was.
    void run_claude(std::vector<std::vector<std::string>> steps, std::string failure);
    void show_claude();

    Server server_;
    ClaudeCode claude_;

    std::shared_ptr<jadefx::CheckBox> server_box_;
    std::shared_ptr<jadefx::Label> forced_;
    std::shared_ptr<jadefx::Label> server_status_;
    std::string server_status_text_;

    std::shared_ptr<jadefx::Label> claude_status_;
    std::shared_ptr<jadefx::Label> claude_message_;
    std::shared_ptr<jadefx::Button> connect_;
    std::shared_ptr<jadefx::Button> disconnect_;
    std::shared_ptr<jadefx::Button> copy_;
    std::shared_ptr<jadefx::Button> check_;
    std::string claude_status_text_;
    // What the CLI's last run, or Copy Command, said.
    std::string cli_message_;
    // That, and what the server's state adds.
    std::string claude_message_text_;
    ClaudeRegistration registration_ = ClaudeRegistration::Unknown;
    std::string registered_command_;
    bool busy_ = false;
    // A CLI run that ends after the page is gone finds this expired.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace ide
