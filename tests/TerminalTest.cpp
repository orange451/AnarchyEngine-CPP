#include "ide/Pty.hpp"
#include "ide/TerminalScreen.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The terminal page's two halves without a window: a pseudo-terminal running
// real programs, and the screen that turns their output into cells and keys
// into the bytes a program reads.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

// What a child wrote and how it ended, filled from the pty's reader thread.
struct Transcript {
    std::mutex mutex;
    std::condition_variable changed;
    std::string output;
    int exit_code = -1;
    bool exited = false;

    void attach(ide::PtyOptions& options) {
        options.on_output = [this](std::string_view bytes) {
            std::lock_guard<std::mutex> lock(mutex);
            output.append(bytes.data(), bytes.size());
            changed.notify_all();
        };
        options.on_exit = [this](int code) {
            std::lock_guard<std::mutex> lock(mutex);
            exit_code = code;
            exited = true;
            changed.notify_all();
        };
    }

    // True when the child exited within the time.
    bool wait_exit(std::chrono::seconds limit = std::chrono::seconds(20)) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, limit, [this] { return exited; });
    }

    // True when text appeared within the time.
    bool wait_for(const std::string& text, std::chrono::seconds limit = std::chrono::seconds(20)) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, limit, [&] { return output.find(text) != std::string::npos; });
    }

    std::string text() {
        std::lock_guard<std::mutex> lock(mutex);
        return output;
    }
};

// A program that runs one shell command and exits.
ide::PtyOptions ShellCommand(const std::string& command) {
    ide::PtyOptions options;
#if defined(_WIN32)
    options.program = "cmd.exe";
    options.args = {"/d", "/c", command};
#else
    options.program = "/bin/sh";
    options.args = {"-c", command};
#endif
    return options;
}

// A shell that reads commands from the terminal until it is told to exit.
ide::PtyOptions InteractiveShell() {
    ide::PtyOptions options;
#if defined(_WIN32)
    options.program = "cmd.exe";
    options.args = {"/d"};
#else
    options.program = "/bin/sh";
#endif
    return options;
}

#if defined(_WIN32)
const char* const kSixTimesSeven = "set /a 6*7\r";
const char* const kShowSize = "mode con\r";
const char* const kExit = "exit\r";
#else
const char* const kSixTimesSeven = "echo $((6*7))\r";
const char* const kShowSize = "stty size\r";
const char* const kExit = "exit\r";
#endif

void TestCommandOutputArrives() {
    Transcript transcript;
    ide::PtyOptions options = ShellCommand("echo hello-pty");
    transcript.attach(options);
    std::string error;
    std::unique_ptr<ide::Pty> pty = ide::Pty::spawn(std::move(options), &error);
    Expect(pty != nullptr, "a shell command starts in a pty");
    if (!pty) {
        std::fprintf(stderr, "  %s\n", error.c_str());
        return;
    }
    Expect(transcript.wait_exit(), "the command exits");
    Expect(transcript.text().find("hello-pty") != std::string::npos, "what the command printed arrives before it exits");
    Expect(transcript.exit_code == 0, "a command that succeeds exits with 0");
}

void TestExitCodeIsReported() {
    Transcript transcript;
    ide::PtyOptions options = ShellCommand("exit 3");
    transcript.attach(options);
    std::unique_ptr<ide::Pty> pty = ide::Pty::spawn(std::move(options), nullptr);
    Expect(pty != nullptr && transcript.wait_exit(), "a command that fails exits");
    Expect(transcript.exit_code == 3, "its exit code is reported");
}

void TestMissingProgramFails() {
    ide::PtyOptions options;
    options.program = "anarchy-no-such-program-xyz";
    std::string error;
    Transcript transcript;
    transcript.attach(options);
    std::unique_ptr<ide::Pty> pty = ide::Pty::spawn(std::move(options), &error);
    // POSIX finds out after the fork, so the child exits with 127 instead.
    Expect(pty == nullptr ? !error.empty() : (transcript.wait_exit() && transcript.exit_code == 127),
           "a program that is not found is an error, or exits with 127");
}

void TestTypingReachesTheChild() {
    Transcript transcript;
    ide::PtyOptions options = InteractiveShell();
    transcript.attach(options);
    std::unique_ptr<ide::Pty> pty = ide::Pty::spawn(std::move(options), nullptr);
    Expect(pty != nullptr, "an interactive shell starts");
    if (!pty) {
        return;
    }
    pty->write(kSixTimesSeven);
    Expect(transcript.wait_for("42"), "a typed command runs in the shell");
    pty->write(kExit);
    Expect(transcript.wait_exit(), "typing exit ends the shell");
}

void TestSizeReachesTheChild() {
    Transcript transcript;
    ide::PtyOptions options = InteractiveShell();
    options.cols = 101;
    options.rows = 31;
    transcript.attach(options);
    std::unique_ptr<ide::Pty> pty = ide::Pty::spawn(std::move(options), nullptr);
    if (!pty) {
        Expect(false, "a shell starts to measure");
        return;
    }
    pty->write(kShowSize);
    Expect(transcript.wait_for("101"), "the shell sees the columns it started with");
    pty->resize(93, 27);
    pty->write(kShowSize);
    Expect(transcript.wait_for("93"), "the shell sees the columns after a resize");
    pty->write(kExit);
    transcript.wait_exit();
}

void TestEnvironmentAndFolderReachTheChild() {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "anarchy-pty-folder";
    std::filesystem::create_directories(folder);
#if defined(_WIN32)
    ide::PtyOptions options = ShellCommand("echo %ANARCHY_PTY_VALUE% & cd");
#else
    ide::PtyOptions options = ShellCommand("echo $ANARCHY_PTY_VALUE; pwd");
#endif
    options.cwd = folder.string();
    options.env.emplace_back("ANARCHY_PTY_VALUE", "value-from-parent");
    Transcript transcript;
    transcript.attach(options);
    std::unique_ptr<ide::Pty> pty = ide::Pty::spawn(std::move(options), nullptr);
    Expect(pty != nullptr && transcript.wait_exit(), "a command with its own environment runs");
    const std::string text = transcript.text();
    Expect(text.find("value-from-parent") != std::string::npos, "an added variable reaches the child");
    Expect(text.find("anarchy-pty-folder") != std::string::npos, "the child starts in the folder asked for");
}

void TestClosingEndsARunningChild() {
    Transcript transcript;
    ide::PtyOptions options = InteractiveShell();
    transcript.attach(options);
    std::unique_ptr<ide::Pty> pty = ide::Pty::spawn(std::move(options), nullptr);
    if (!pty) {
        Expect(false, "a shell starts to close");
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    pty.reset();
    const auto took = std::chrono::steady_clock::now() - start;
    Expect(took < std::chrono::seconds(5), "closing a pty with a live shell returns promptly");
}

void TestTextLandsInCells() {
    ide::TerminalScreen screen(5, 20);
    screen.write("hi\r\nthere");
    Expect(screen.row_text(0) == "hi", "text lands on the first row");
    Expect(screen.row_text(1) == "there", "a new line moves to the next row");
    const ide::TerminalCursor cursor = screen.cursor();
    Expect(cursor.row == 1 && cursor.col == 5, "the cursor follows the text");
    Expect(screen.cell(1, 0).chars == U"t", "a cell holds its character");
    Expect(screen.cell(1, 10).chars.empty(), "a cell nothing was written to is blank");
}

void TestColorsAndAttributes() {
    ide::TerminalScreen screen(2, 20);
    screen.write("\x1b[31mR\x1b[0mN\x1b[38;2;1;2;3mT\x1b[0m\x1b[1;7;4mB");
    const ide::TerminalCell red = screen.cell(0, 0);
    Expect(!red.fg.is_default && red.fg.r > 100 && red.fg.g < 100, "SGR 31 is a red foreground");
    Expect(screen.cell(0, 1).fg.is_default, "SGR 0 goes back to the default foreground");
    const ide::TerminalCell rgb = screen.cell(0, 2);
    Expect(rgb.fg.r == 1 && rgb.fg.g == 2 && rgb.fg.b == 3, "a 24-bit foreground keeps its value");
    const ide::TerminalCell styled = screen.cell(0, 3);
    Expect(styled.bold && styled.reverse && styled.underline, "bold, reverse, and underline are kept");
    Expect(screen.cell(0, 3).bg.is_default, "a cell with no background set has the default one");
}

void TestQueriesAreAnswered() {
    ide::TerminalScreen screen(5, 20);
    std::string replies;
    screen.set_on_reply([&](std::string_view bytes) { replies.append(bytes.data(), bytes.size()); });
    screen.write("\x1b[2;3H\x1b[6n");
    Expect(replies == "\x1b[2;3R", "a cursor position query is answered, as ConPTY waits for at startup");
}

void TestLinesScrollIntoHistory() {
    ide::TerminalScreen screen(3, 10);
    screen.write("1\r\n2\r\n3\r\n4\r\n5");
    Expect(screen.row_text(0) == "3", "the screen shows the newest rows");
    Expect(screen.scrollback_rows() == 2, "rows that scroll off are kept");
    Expect(screen.row_text(-1) == "2", "row -1 is the newest line kept");
    Expect(screen.row_text(-2) == "1", "row -2 is the one before it");
    Expect(screen.row_text(-3).empty(), "a row past the history is empty");
}

void TestKeysBecomeBytes() {
    ide::TerminalScreen screen(5, 20);
    std::string sent;
    screen.set_on_reply([&](std::string_view bytes) { sent.append(bytes.data(), bytes.size()); });
    auto take = [&sent] {
        std::string out;
        out.swap(sent);
        return out;
    };
    const ide::TerminalMods none;
    ide::TerminalMods ctrl;
    ctrl.ctrl = true;
    screen.key(ide::TerminalKey::Enter, none);
    Expect(take() == "\r", "Enter sends a carriage return");
    screen.key(ide::TerminalKey::Backspace, none);
    Expect(take() == "\x7f", "Backspace sends DEL");
    screen.key(ide::TerminalKey::Up, none);
    Expect(take() == "\x1b[A", "Up sends CSI A");
    screen.write("\x1b[?1h");
    screen.key(ide::TerminalKey::Up, none);
    Expect(take() == "\x1bOA", "Up sends SS3 A in application cursor mode");
    screen.character(U'c', ctrl);
    Expect(take() == "\x03", "Ctrl+C sends ETX");
    screen.character(U'é', none);
    Expect(take() == "\xC3\xA9", "a typed character is sent as UTF-8");
    screen.key(ide::TerminalKey::F1, none);
    Expect(take() == "\x1bOP", "F1 sends SS3 P");
}

void TestPaste() {
    ide::TerminalScreen screen(5, 20);
    std::string sent;
    screen.set_on_reply([&](std::string_view bytes) { sent.append(bytes.data(), bytes.size()); });
    screen.paste("a\nb");
    Expect(sent == "a\rb", "a pasted line break is sent as Enter would be");
    sent.clear();
    screen.write("\x1b[?2004h");
    screen.paste("ab");
    Expect(sent == "\x1b[200~ab\x1b[201~", "a paste is bracketed when the program asks");
}

void TestFocusReports() {
    ide::TerminalScreen screen(5, 20);
    std::string sent;
    screen.set_on_reply([&](std::string_view bytes) { sent.append(bytes.data(), bytes.size()); });
    screen.focus(true);
    Expect(sent.empty(), "focus is not reported until the program asks");
    screen.write("\x1b[?1004h");
    screen.focus(false);
    Expect(sent == "\x1b[O", "losing focus is reported once asked");
    sent.clear();
    screen.focus(true);
    Expect(sent == "\x1b[I", "gaining focus is reported once asked");
}

void TestPaletteChangesTheBasicColors() {
    ide::TerminalScreen screen(2, 20);
    std::array<ide::TerminalColor, 16> palette{};
    for (int i = 0; i < 16; ++i) {
        palette[static_cast<std::size_t>(i)].r = static_cast<std::uint8_t>(i * 10);
        palette[static_cast<std::size_t>(i)].g = 1;
        palette[static_cast<std::size_t>(i)].b = 2;
        palette[static_cast<std::size_t>(i)].is_default = false;
    }
    screen.write("\x1b[33mY");
    screen.set_palette(palette);
    screen.write("\x1b[93mB\x1b[38;2;7;8;9mT");
    const ide::TerminalCell yellow = screen.cell(0, 0);
    Expect(yellow.fg.r == 30 && yellow.fg.g == 1 && yellow.fg.b == 2,
           "a cell written before the palette changed shows the new color");
    Expect(screen.cell(0, 1).fg.r == 110, "a bright color comes from the palette's upper half");
    Expect(screen.cell(0, 2).fg.r == 7, "a 24-bit color is not the palette's");
}

void TestTitleSizeAndScreens() {
    ide::TerminalScreen screen(5, 20);
    screen.write("\x1b]0;my title\x07");
    Expect(screen.title() == "my title", "OSC 0 sets the title");
    screen.resize(10, 40);
    Expect(screen.rows() == 10 && screen.cols() == 40, "a resize changes the grid");
    Expect(!screen.alt_screen(), "the normal screen shows first");
    screen.write("\x1b[?1049h");
    Expect(screen.alt_screen(), "a full-screen program switches to the alternate screen");
    screen.write("\xE4\xB8\xAD");
    Expect(screen.cell(0, 0).width == 2, "a wide character takes two cells");
}

}  // namespace

int main() {
    TestTextLandsInCells();
    TestColorsAndAttributes();
    TestQueriesAreAnswered();
    TestLinesScrollIntoHistory();
    TestKeysBecomeBytes();
    TestPaste();
    TestFocusReports();
    TestPaletteChangesTheBasicColors();
    TestTitleSizeAndScreens();
    TestCommandOutputArrives();
    TestExitCodeIsReported();
    TestMissingProgramFails();
    TestTypingReachesTheChild();
    TestSizeReachesTheChild();
    TestEnvironmentAndFolderReachTheChild();
    TestClosingEndsARunningChild();
    if (gFailures == 0) {
        std::printf("terminal tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d terminal test failures\n", gFailures);
    return 1;
}
