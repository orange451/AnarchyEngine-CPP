#include "AiClientsPage.hpp"

#include <chrono>
#include <initializer_list>
#include <thread>
#include <utility>

namespace ide {
namespace {

// claude starts node and may check each server's health, so it gets a while.
constexpr std::chrono::seconds kCliTimeout{60};
constexpr std::size_t kMessageLength = 160;

constexpr const char* kStylesheet = R"CSS(
.ai-heading {
    color: var(--ide-text-color);
    font-size: 14px;
    padding: 6px 0 0 0;
}
.ai-paragraph {
    spacing: 0px;
}
)CSS";

std::shared_ptr<jadefx::Label> Text(const std::string& text, const char* style) {
    auto label = jadefx::make<jadefx::Label>(text);
    label->getClassList().add(style);
    return label;
}

// Hint lines kept together as one paragraph, since a label does not wrap.
std::shared_ptr<jadefx::VBox> Hint(std::initializer_list<const char*> lines) {
    auto box = jadefx::make<jadefx::VBox>();
    box->getClassList().add("ai-paragraph");
    for (const char* line : lines) {
        box->getChildren().add(Text(line, "prefs-hint"));
    }
    return box;
}

// The first line of what the CLI said, short enough for one label.
std::string FirstLine(const std::string& text) {
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            return line.size() > kMessageLength ? line.substr(0, kMessageLength) + "..." : line;
        }
        start = end + 1;
    }
    return {};
}

void SetError(jadefx::Label& label, bool error) {
    label.getClassList().removeIf([](const std::string& name) { return name == "error"; });
    if (error) {
        label.getClassList().add("error");
    }
}

}  // namespace

struct AiClientsPage::Outcome {
    // Why a step failed. Empty when every step ran.
    std::string failure;
    ProcessResult get;
};

AiClientsPage::AiClientsPage(Server server, ClaudeCode claude)
    : server_(std::move(server)), claude_(std::move(claude)) {
    if (!claude_.run) {
        const std::filesystem::path cli = claude_.cli;
        claude_.run = [cli](const std::vector<std::string>& args) { return run_process(cli, args, kCliTimeout); };
    }
    if (!claude_.async) {
        claude_.async = [](std::function<void()> work, std::function<void()> done) {
            std::thread([work = std::move(work), done = std::move(done)]() mutable {
                work();
                jadefx::runLater(std::move(done));
            }).detach();
        };
    }
    build();
    refresh_server();
    if (!claude_.cli.empty()) {
        refresh_claude();
    } else {
        show_claude();
    }
}

AiClientsPage::~AiClientsPage() = default;

void AiClientsPage::build() {
    setStylesheet(kStylesheet);
    getClassList().add("prefs-top");
    setPrefWidthRatio(1);
    setPrefHeightRatio(1);
    auto& children = getChildren();

    children.add(Text("MCP server", "ai-heading"));
    server_box_ = jadefx::make<jadefx::CheckBox>("Let AI clients drive this studio");
    server_box_->setOnAction([this](jadefx::ActionEvent&) { set_server_enabled(server_box_->isSelected()); });
    children.add(server_box_);
    children.add(Hint({"Off unless turned on. While it is on, a program on this computer that has the",
                       "studio's token, such as the anarchy-mcp bridge, can read and edit the open place."}));
    forced_ = Text("", "prefs-hint");
    children.add(forced_);
    server_status_ = Text("", "prefs-status");
    children.add(server_status_);

    children.add(Text("Claude Code", "ai-heading"));
    claude_status_ = Text("", "prefs-status");
    children.add(claude_status_);
    auto buttons = jadefx::make<jadefx::HBox>();
    buttons->getClassList().add("prefs-row");
    connect_ = jadefx::make<jadefx::Button>("Connect");
    connect_->setOnAction([this](jadefx::ActionEvent&) { connect_claude(); });
    disconnect_ = jadefx::make<jadefx::Button>("Disconnect");
    disconnect_->setOnAction([this](jadefx::ActionEvent&) { disconnect_claude(); });
    copy_ = jadefx::make<jadefx::Button>("Copy Command");
    copy_->setOnAction([this](jadefx::ActionEvent&) { copy_command(); });
    check_ = jadefx::make<jadefx::Button>("Check Again");
    check_->setOnAction([this](jadefx::ActionEvent&) { refresh_claude(); });
    buttons->getChildren().add(connect_);
    buttons->getChildren().add(disconnect_);
    buttons->getChildren().add(copy_);
    buttons->getChildren().add(check_);
    children.add(buttons);
    claude_message_ = Text("", "prefs-status");
    children.add(claude_message_);
    children.add(Hint({"Claude Code finds the studio when it next starts, or after /mcp in a running session.",
                       "With several studios open, it talks to the one whose project holds its folder."}));
}

void AiClientsPage::refresh_server() {
    const McpSwitch setting = server_.setting ? server_.setting() : McpSwitch{};
    server_box_->setSelected(setting.on);
    server_box_->setDisable(!setting.forced_by.empty());
    forced_->setText(setting.forced_by.empty()
                         ? std::string()
                         : setting.forced_by + " is set, so it decides whether the server runs, not this box.");
    server_status_text_ = server_.status ? server_.status() : std::string();
    server_status_->setText(server_status_text_);
}

void AiClientsPage::set_server_enabled(bool on) {
    if (server_.set_enabled) {
        server_.set_enabled(on);
    }
    refresh_server();
    show_claude();
}

void AiClientsPage::refresh_claude() { run_claude({}, {}); }

void AiClientsPage::connect_claude() {
    if (claude_.bridge.empty()) {
        return;
    }
    std::vector<std::vector<std::string>> steps;
    if (registration_ == ClaudeRegistration::Elsewhere) {
        steps.push_back(claude_remove_args());
    }
    steps.push_back(claude_add_args(claude_.bridge));
    run_claude(std::move(steps), "connect");
}

void AiClientsPage::disconnect_claude() { run_claude({claude_remove_args()}, "disconnect"); }

void AiClientsPage::copy_command() {
    if (claude_.bridge.empty()) {
        return;
    }
    if (jadefx::Scene* scene = getScene()) {
        scene->setClipboardText(claude_add_command(claude_.bridge));
    }
    cli_message_ = "Copied. Run it in a terminal where claude works.";
    show_claude();
}

void AiClientsPage::run_claude(std::vector<std::vector<std::string>> steps, std::string failure) {
    if (claude_.cli.empty() || busy_) {
        return;
    }
    busy_ = true;
    cli_message_.clear();
    show_claude();
    auto outcome = std::make_shared<Outcome>();
    auto run = claude_.run;
    std::weak_ptr<bool> alive = alive_;
    claude_.async(
        [run, outcome, steps = std::move(steps), failure]() {
            for (const std::vector<std::string>& step : steps) {
                const ProcessResult result = run(step);
                if (!result.started || result.timed_out || result.exit_code != 0) {
                    const std::string said = FirstLine(result.started ? result.output : result.error);
                    outcome->failure = "Could not " + failure + (result.timed_out ? ": claude took too long."
                                                                 : said.empty() ? "." : ": " + said);
                    break;
                }
            }
            outcome->get = run(claude_get_args());
        },
        [this, alive, outcome, connecting = failure == "connect"]() {
            if (alive.expired()) {
                return;
            }
            busy_ = false;
            const ProcessResult& get = outcome->get;
            const ClaudeStatus status = get.started && !get.timed_out
                                            ? parse_claude_get(get.exit_code, get.output, claude_.bridge)
                                            : ClaudeStatus{};
            registration_ = status.registration;
            registered_command_ = status.command;
            if (!outcome->failure.empty()) {
                cli_message_ = outcome->failure;
            } else if (registration_ == ClaudeRegistration::Unknown) {
                const std::string said = FirstLine(get.started ? get.output : get.error);
                cli_message_ = get.timed_out ? "claude took too long to answer."
                                                     : said.empty() ? std::string() : "claude said: " + said;
            } else if (connecting && registration_ == ClaudeRegistration::Connected) {
                cli_message_ = "Registered for every folder you work in.";
            }
            show_claude();
        });
}

void AiClientsPage::show_claude() {
    const bool found = !claude_.cli.empty();
    const bool has_bridge = !claude_.bridge.empty();
    if (!has_bridge) {
        claude_status_text_ = "anarchy-mcp is not beside the studio, so there is nothing to register.";
    } else if (!found) {
        claude_status_text_ = "Claude Code was not found. Install it and reopen Preferences, or copy the command.";
    } else if (busy_) {
        claude_status_text_ = "Asking Claude Code...";
    } else {
        switch (registration_) {
        case ClaudeRegistration::Connected:
            claude_status_text_ = "Connected: Claude Code runs this studio's anarchy-mcp.";
            break;
        case ClaudeRegistration::Elsewhere:
            claude_status_text_ = "Claude Code runs another anarchy server: " + registered_command_ +
                                  ". Connect replaces it.";
            break;
        case ClaudeRegistration::NotRegistered:
            claude_status_text_ = "Not connected.";
            break;
        case ClaudeRegistration::Unknown:
            claude_status_text_ = "Could not tell what Claude Code has registered.";
            break;
        }
    }
    claude_message_text_ = cli_message_;
    if (!busy_ && registration_ == ClaudeRegistration::Connected && server_.setting && !server_.setting().on) {
        claude_message_text_ += (claude_message_text_.empty() ? "" : " ") +
                                std::string("Turn on the MCP server above, or Claude Code's calls will fail.");
    }
    claude_status_->setText(claude_status_text_);
    claude_message_->setText(claude_message_text_);
    SetError(*claude_message_, claude_message_text_.rfind("Could not", 0) == 0);
    connect_->setDisable(!found || !has_bridge || busy_ || registration_ == ClaudeRegistration::Connected);
    disconnect_->setDisable(!found || busy_ ||
                            (registration_ != ClaudeRegistration::Connected &&
                             registration_ != ClaudeRegistration::Elsewhere));
    copy_->setDisable(!has_bridge);
    check_->setDisable(!found || busy_);
}

}  // namespace ide
