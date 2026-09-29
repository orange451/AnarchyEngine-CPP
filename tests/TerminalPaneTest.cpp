#include "ide/IdeTerminal.hpp"
#include "ide/IdeTheme.hpp"
#include "ide/Pty.hpp"
#include "ide/TerminalScreen.hpp"
#include "ide/TerminalView.hpp"

#include "jadefx/jadefx.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// The Terminal page in a headless scene, with a stand-in for the program so
// what it is sent can be read back: its size, output, keys, pastes, and exit.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

// What the page did to the program it started.
struct FakeProgram {
    ide::PtyOptions options;
    std::string typed;
    int cols = 0;
    int rows = 0;
    int starts = 0;
    bool alive = false;
};

class FakePty : public ide::Pty {
public:
    explicit FakePty(std::shared_ptr<FakeProgram> program) : program_(std::move(program)) {}
    ~FakePty() override { program_->alive = false; }
    void write(std::string_view bytes) override { program_->typed.append(bytes.data(), bytes.size()); }
    void resize(int cols, int rows) override {
        program_->cols = cols;
        program_->rows = rows;
    }

private:
    std::shared_ptr<FakeProgram> program_;
};

ide::TerminalHost FakeHost(const std::shared_ptr<FakeProgram>& program) {
    ide::TerminalHost host;
    host.spawn = [program](ide::PtyOptions options, std::string*) -> std::unique_ptr<ide::Pty> {
        ++program->starts;
        program->alive = true;
        program->cols = options.cols;
        program->rows = options.rows;
        program->options = std::move(options);
        return std::make_unique<FakePty>(program);
    };
    host.folder = [] { return std::string("project-folder"); };
    return host;
}

// Output arrives on the pty's reader thread, as a real program's does.
void Output(FakeProgram& program, const std::string& bytes) {
    std::thread reader([&] { program.options.on_output(bytes); });
    reader.join();
}

void Exit(FakeProgram& program, int code) {
    std::thread reader([&] { program.options.on_exit(code); });
    reader.join();
}

struct Page {
    std::shared_ptr<FakeProgram> program = std::make_shared<FakeProgram>();
    std::shared_ptr<ide::IdeTerminal> terminal;
    std::shared_ptr<jadefx::Scene> scene;
    double width = 800;
    double height = 400;
    double time = 0;

    Page() {
        terminal = jadefx::make<ide::IdeTerminal>(FakeHost(program));
        terminal->setPrefWidthRatio(1);
        terminal->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(terminal, width, height);
        frame();
    }

    // What the studio's frame does: queued work, then layout.
    void frame() {
        jadefx::drainRunLater();
        time += 0.016;
        scene->layout(width, height, time);
    }

    ide::TerminalScreen& screen() { return terminal->view().screen(); }

    void key(int key, int mods = 0) { scene->noteKey(key, true, false, mods); scene->noteKey(key, false, false, mods); }

    std::string take() {
        std::string out;
        out.swap(program->typed);
        return out;
    }
};

#if defined(__APPLE__)
constexpr int kPasteMods = jadefx::Key::ModSuper;
#elif defined(_WIN32)
constexpr int kPasteMods = jadefx::Key::ModControl;
#else
constexpr int kPasteMods = jadefx::Key::ModControl | jadefx::Key::ModShift;
#endif

void TestStartsAtItsSize() {
    Page page;
    Expect(page.program->starts == 1, "the page starts its program once it has a size");
    Expect(page.program->cols > 20 && page.program->rows > 5, "the program starts with the page's columns and rows");
    Expect(page.program->cols == page.screen().cols() && page.program->rows == page.screen().rows(),
           "the program and the screen agree on the size");
    Expect(page.program->options.cwd == "project-folder", "the program starts in the project's folder");
    page.frame();
    Expect(page.program->starts == 1, "a later frame does not start another program");
}

void TestOutputShowsOnTheNextFrame() {
    Page page;
    Output(*page.program, "hello\r\nworld");
    Expect(page.screen().row_text(0).empty(), "output waits for the UI thread");
    page.frame();
    Expect(page.screen().row_text(0) == "hello" && page.screen().row_text(1) == "world",
           "output reaches the screen on the next frame");
}

void TestKeysReachTheProgram() {
    Page page;
    page.terminal->view().requestFocus();
    int hooked = 0;
    page.scene->addKeyHook([&](jadefx::KeyEvent& event) {
        if (event.pressed && !event.consumed) {
            ++hooked;
        }
    });
    page.scene->noteText("ls");
    page.key(jadefx::Key::Enter);
    Expect(page.take() == "ls\r", "typing and Enter are sent to the program");
    page.key(jadefx::Key::C, jadefx::Key::ModControl);
    Expect(page.take() == "\x03", "Ctrl+C is sent as an interrupt");
    page.key(jadefx::Key::S, jadefx::Key::ModControl);
    Expect(page.take() == "\x13", "Ctrl+S goes to the program, not to Save");
    page.key(jadefx::Key::Up);
    Expect(page.take() == "\x1b[A", "Up is sent as an arrow key");
    page.key(jadefx::Key::Tab);
    Expect(page.take() == "\t", "Tab is sent, not used to move focus");
    page.key(jadefx::Key::Escape);
    Expect(page.take() == "\x1b", "Escape is sent to the program");
    page.key(jadefx::Key::F);
    Expect(page.take().empty(), "a letter key alone sends nothing; its text does");
    Expect(hooked == 0, "keys the terminal takes do not reach the studio's shortcuts");
#if !defined(__APPLE__)
    // On macOS, Option types the character it composes instead.
    page.key(jadefx::Key::X, jadefx::Key::ModAlt);
    page.scene->noteText("x");
    Expect(page.take() == "\x1bx", "Alt+X is sent once, with an escape before it");
#endif
}

void TestPaste() {
    Page page;
    page.terminal->view().requestFocus();
    page.scene->setClipboardText("a\nb");
    page.key(jadefx::Key::V, kPasteMods);
    Expect(page.take() == "a\rb", "the paste shortcut sends the clipboard");
}

void TestResizeFollowsThePage() {
    Page page;
    const int cols = page.program->cols;
    page.width = 1000;
    page.frame();
    Expect(page.program->cols > cols, "a wider page gives the program more columns");
    Expect(page.program->cols == page.screen().cols(), "the screen resizes with the program");
}

void TestHistoryScrolls() {
    Page page;
    std::string lines;
    for (int i = 0; i < 100; ++i) {
        lines += "line " + std::to_string(i) + "\r\n";
    }
    Output(*page.program, lines);
    page.frame();
    Expect(page.terminal->view().scrollOffset() == 0, "the page shows the newest rows");
    page.scene->noteScroll(100, 100, 0, 1);
    Expect(page.terminal->view().scrollOffset() > 0, "the wheel scrolls back through history");
    page.terminal->view().requestFocus();
    page.scene->noteText("q");
    Expect(page.terminal->view().scrollOffset() == 0, "typing goes back to the newest rows");
}

void TestExitAndRestart() {
    Page page;
    page.terminal->view().requestFocus();
    Exit(*page.program, 3);
    page.frame();
    Expect(!page.terminal->running(), "the page knows the program ended");
    bool said = false;
    for (int row = 0; row < page.screen().rows(); ++row) {
        said = said || page.screen().row_text(row).find("exited with code 3") != std::string::npos;
    }
    Expect(said, "the page says the program ended, and with what code");
    page.key(jadefx::Key::Enter);
    Expect(page.program->starts == 2 && page.terminal->running(), "Enter after the end starts the program again");
}

void TestTitleFollowsTheProgram() {
    Page page;
    Output(*page.program, "\x1b]0;claude\x07");
    page.frame();
    Expect(page.terminal->title() == "Terminal - claude", "the tab shows the program's title");
}

void TestTextStaysReadable() {
    const jadefx::Color white = jadefx::Color::rgb8(255, 255, 255);
    const jadefx::Color black = jadefx::Color::rgb8(0, 0, 0);
    const jadefx::Color bright_yellow = jadefx::Color::rgb8(245, 245, 67);
    Expect(ide::ContrastRatio(white, black) > 20.9 && ide::ContrastRatio(black, white) > 20.9,
           "black on white has the most contrast there is, 21:1");
    const jadefx::Color on_white = ide::EnsureContrast(bright_yellow, white, 4.5);
    Expect(ide::ContrastRatio(on_white, white) >= 4.5, "bright yellow on white is darkened until it reads");
    Expect(on_white.r > on_white.b, "the darkened yellow is still a yellow");
    const jadefx::Color on_black = ide::EnsureContrast(bright_yellow, black, 4.5);
    Expect(on_black.r == bright_yellow.r && on_black.g == bright_yellow.g && on_black.b == bright_yellow.b,
           "a color that already reads is left alone");
    const jadefx::Color navy = jadefx::Color::rgb8(0, 0, 90);
    Expect(ide::ContrastRatio(ide::EnsureContrast(navy, black, 4.5), black) >= 4.5,
           "a dark color on a dark background is lightened");
    Expect(ide::ContrastRatio(ide::EnsureContrast(white, white, 4.5), white) >= 4.5,
           "white text on white, as a dark-theme program draws, becomes readable");

    const std::array<ide::TerminalColor, 16> light = ide::AnsiPalette(true);
    const std::array<ide::TerminalColor, 16> dark = ide::AnsiPalette(false);
    auto color = [](const ide::TerminalColor& c) { return jadefx::Color::rgb8(c.r, c.g, c.b); };
    Expect(ide::ContrastRatio(color(light[11]), white) > ide::ContrastRatio(color(dark[11]), white),
           "a light theme's bright yellow is darker than a dark theme's");
}

void TestLightThemeUsesTheLightPalette() {
    auto same = [](const ide::TerminalColor& a, const ide::TerminalColor& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    };
    ide::set_current_theme(ide::shipped_theme("light"));
    Page page;
    Output(*page.program, "\x1b[93mY");
    page.frame();
    Expect(same(page.screen().cell(0, 0).fg, ide::AnsiPalette(true)[11]),
           "on the light theme, bright yellow comes from the light palette");
    ide::set_current_theme(ide::shipped_theme("dark"));
    page.frame();
    Expect(same(page.screen().cell(0, 0).fg, ide::AnsiPalette(false)[11]),
           "switching to a dark theme switches the palette, for text already shown too");
    ide::set_current_theme(ide::shipped_theme("light"));
}

bool Covers(const std::vector<ide::CellShape>& shapes, float x, float y) {
    for (const ide::CellShape& shape : shapes) {
        if (shape.kind == ide::CellShape::Kind::Rect && x >= shape.x && x < shape.x + shape.width && y >= shape.y &&
            y < shape.y + shape.height) {
            return true;
        }
    }
    return false;
}

void TestBlocksAndLinesFillTheirCells() {
    // A cell 8 by 16 at (10, 20), lines 1 point thick, one pixel a point.
    auto shapes = [](char32_t c) { return ide::CellShapes(c, 10.f, 20.f, 8.f, 16.f, 1.f, 1.f); };
    Expect(shapes(U'A').empty(), "a letter is left to the font");

    const std::vector<ide::CellShape> full = shapes(U'█');
    Expect(full.size() == 1 && full[0].x == 10.f && full[0].y == 20.f && full[0].width == 8.f &&
               full[0].height == 16.f && full[0].alpha == 1.f,
           "a full block fills its whole cell, so blocks meet without gaps");
    Expect(Covers(shapes(U'▀'), 12, 22) && !Covers(shapes(U'▀'), 12, 30), "an upper half block fills the top half");
    Expect(Covers(shapes(U'▐'), 16, 30) && !Covers(shapes(U'▐'), 12, 30), "a right half block fills the right half");
    Expect(Covers(shapes(U'▂'), 12, 34) && !Covers(shapes(U'▂'), 12, 30), "a lower quarter block fills the bottom quarter");

    // The mascot's quadrants: upper left, upper right, lower left, and lower right.
    const std::vector<ide::CellShape> three = shapes(U'▛');
    Expect(Covers(three, 12, 22) && Covers(three, 16, 22) && Covers(three, 12, 32) && !Covers(three, 16, 32),
           "a quadrant block fills its three quadrants and not the fourth");
    const std::vector<ide::CellShape> one = shapes(U'▝');
    Expect(Covers(one, 16, 22) && !Covers(one, 12, 22) && !Covers(one, 16, 32), "an upper right quadrant alone");

    const std::vector<ide::CellShape> shade = shapes(U'░');
    Expect(shade.size() == 1 && shade[0].alpha > 0.2f && shade[0].alpha < 0.3f, "a light shade is a quarter of the color");

    const std::vector<ide::CellShape> across = shapes(U'─');
    Expect(Covers(across, 10, 28) && Covers(across, 17.9f, 28) && !Covers(across, 12, 22),
           "a horizontal line crosses the whole cell at its middle, so lines join");
    Expect(across.size() == 1 && across[0].height == 1.f, "a light line is one line thick");
    const std::vector<ide::CellShape> heavy = shapes(U'━');
    Expect(heavy.size() == 1 && heavy[0].height == 2.f, "a heavy line is two");
    const std::vector<ide::CellShape> down = shapes(U'│');
    Expect(Covers(down, 14, 20) && Covers(down, 14, 35.9f), "a vertical line runs the cell's full height");
    const std::vector<ide::CellShape> cross = shapes(U'┼');
    Expect(Covers(cross, 10, 28) && Covers(cross, 14, 20) && !Covers(cross, 11, 21), "a cross reaches all four edges");
    const std::vector<ide::CellShape> corner = shapes(U'┌');
    Expect(Covers(corner, 17.9f, 28) && Covers(corner, 14, 35.9f) && !Covers(corner, 10, 28) && !Covers(corner, 14, 20),
           "a corner reaches right and down only");
    const std::vector<ide::CellShape> round = shapes(U'╭');
    bool arc = false;
    for (const ide::CellShape& shape : round) {
        arc = arc || shape.kind == ide::CellShape::Kind::Arc;
    }
    Expect(arc, "a rounded corner bends in an arc");

    // Edges land on whole pixels, so a cell's right edge is the next one's left.
    const std::vector<ide::CellShape> first = ide::CellShapes(U'█', 10.3f, 20.f, 7.7f, 16.f, 1.f, 1.f);
    const std::vector<ide::CellShape> second = ide::CellShapes(U'█', 18.f, 20.f, 7.7f, 16.f, 1.f, 1.f);
    Expect(first.size() == 1 && second.size() == 1 && first[0].x == 10.f &&
               first[0].x + first[0].width == second[0].x,
           "neighboring blocks share a whole-pixel edge");
    const std::vector<ide::CellShape> scaled = ide::CellShapes(U'█', 10.3f, 20.f, 7.7f, 16.f, 1.f, 0.5f);
    Expect(scaled.size() == 1 && scaled[0].x == 10.5f, "at twice the pixels per point, edges land on half points");
}

void TestClosingEndsTheProgram() {
    auto program = std::make_shared<FakeProgram>();
    {
        Page page;
        program = page.program;
        Expect(program->alive, "the program runs while the page lives");
    }
    Expect(!program->alive, "the program ends with the page");
}

}  // namespace

int RunTerminalPaneTests() {
    gFailures = 0;
    TestStartsAtItsSize();
    TestOutputShowsOnTheNextFrame();
    TestKeysReachTheProgram();
    TestPaste();
    TestResizeFollowsThePage();
    TestHistoryScrolls();
    TestExitAndRestart();
    TestTitleFollowsTheProgram();
    TestTextStaysReadable();
    TestLightThemeUsesTheLightPalette();
    TestBlocksAndLinesFillTheirCells();
    TestClosingEndsTheProgram();
    if (gFailures == 0) {
        std::printf("terminal pane tests passed\n");
    }
    return gFailures;
}
