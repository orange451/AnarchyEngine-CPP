#include "IdeTerminal.hpp"

#include "Utf8.hpp"

#include <utility>

namespace ide {
namespace {

// The most of a program's title the tab shows.
constexpr std::size_t kTitleLength = 40;

}  // namespace

IdeTerminal::IdeTerminal(TerminalHost host)
    : IdePane("Terminal", true), host_(std::move(host)), view_(jadefx::make<TerminalView>()),
      inbox_(std::make_shared<Inbox>()) {
    setIconFile("Console.png");
    inbox_->owner = this;
    if (!host_.spawn) {
        host_.spawn = [](PtyOptions options, std::string* error) { return Pty::spawn(std::move(options), error); };
    }
    getChildren().add(view_);
    Fill(*view_);
    view_->screen().set_on_reply([this](std::string_view bytes) {
        if (pty_) {
            pty_->write(bytes);
        } else if (started_ && bytes == "\r") {
            // The program ended, and Enter starts another.
            start(view_->screen().cols(), view_->screen().rows());
        }
    });
    // The first size starts the program, so it draws for the page it is in.
    view_->setOnResize([this](int cols, int rows) {
        if (!started_) {
            start(cols, rows);
        } else if (pty_) {
            pty_->resize(cols, rows);
        }
    });
}

IdeTerminal::~IdeTerminal() {
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        inbox_->owner = nullptr;
    }
    pty_.reset();
}

void IdeTerminal::onOpen() {
    if (getScene() != nullptr) {
        view_->requestFocus();
    }
}

void IdeTerminal::start(int cols, int rows) {
    started_ = true;
    PtyOptions options = DefaultShell();
    options.cols = cols;
    options.rows = rows;
    if (host_.folder) {
        options.cwd = host_.folder();
    }
    options.env.emplace_back("TERM_PROGRAM", "AnarchyEngine");
    // Each call hands the bytes to the UI thread, queuing one drain until it runs.
    auto post = [inbox = inbox_](const std::function<void(Inbox&)>& change) {
        std::lock_guard<std::mutex> lock(inbox->mutex);
        change(*inbox);
        if (inbox->queued) {
            return;
        }
        inbox->queued = true;
        jadefx::runLater([inbox] {
            IdeTerminal* owner = nullptr;
            {
                std::lock_guard<std::mutex> lock(inbox->mutex);
                owner = inbox->owner;
            }
            if (owner != nullptr) {
                owner->drain();
            }
        });
    };
    options.on_output = [post](std::string_view bytes) {
        post([bytes](Inbox& inbox) { inbox.output.append(bytes.data(), bytes.size()); });
    };
    options.on_exit = [post](int code) {
        post([code](Inbox& inbox) {
            inbox.exited = true;
            inbox.exit_code = code;
        });
    };
    const std::string program = options.program;
    std::string error;
    pty_ = host_.spawn(std::move(options), &error);
    if (!pty_) {
        view_->screen().write("\x1b[31mCan't start " + program + ": " + error +
                              "\x1b[0m\r\n\x1b[90m[Press Enter to try again.]\x1b[0m\r\n");
    }
}

void IdeTerminal::drain() {
    std::string output;
    bool exited = false;
    int code = 0;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        output.swap(inbox_->output);
        exited = inbox_->exited;
        code = inbox_->exit_code;
        inbox_->exited = false;
        inbox_->queued = false;
    }
    TerminalScreen& screen = view_->screen();
    screen.write(output);
    if (exited) {
        // Its reader thread has handed over the last output, so this joins at once.
        pty_.reset();
        screen.write("\r\n\x1b[90m[Process exited with code " + std::to_string(code) +
                     ". Press Enter to start a new shell.]\x1b[0m\r\n");
    }
    sync_title();
}

void IdeTerminal::sync_title() {
    std::u32string title = Utf32(view_->screen().title());
    if (title.size() > kTitleLength) {
        title.resize(kTitleLength - 1);
        title += U"…";
    }
    const std::string shown = title.empty() ? std::string() : "Terminal - " + Utf8(title);
    if (shown != shown_title_) {
        shown_title_ = shown;
        setTitle(shown);
    }
}

}  // namespace ide
