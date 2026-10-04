#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ide {

struct TerminalColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    // No color was asked for, so the page's own foreground or background shows.
    bool is_default = true;
};

struct TerminalCell {
    // The base character and any combining marks. Empty is a blank.
    std::u32string chars;
    // 2 for a wide character's first cell. Its second cell has width 1 and no chars.
    int width = 1;
    TerminalColor fg;
    TerminalColor bg;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    bool reverse = false;
};

struct TerminalCursor {
    int row = 0;
    int col = 0;
    bool visible = true;
};

enum class TerminalKey {
    Enter,
    Tab,
    Backspace,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
};

struct TerminalMods {
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
};

// The grid a terminal shows, from the bytes a program writes, with the lines
// that scrolled off the top. It also turns keys into the bytes the program
// reads, in whatever modes the program has asked for. libvterm does the
// parsing. Used from one thread.
class TerminalScreen {
public:
    TerminalScreen(int rows, int cols);
    ~TerminalScreen();
    TerminalScreen(const TerminalScreen&) = delete;
    TerminalScreen& operator=(const TerminalScreen&) = delete;

    // Bytes for the program: typed keys, pastes, and answers to its queries.
    void set_on_reply(std::function<void(std::string_view)> handler);

    // What the program wrote.
    void write(std::string_view bytes);
    void resize(int rows, int cols);

    int rows() const;
    int cols() const;
    // Row 0 is the top of the screen. A negative row is history: -1 is the
    // newest line that scrolled off. A row or column outside both is blank.
    TerminalCell cell(int row, int col) const;
    // The row's characters as UTF-8, without trailing blanks.
    std::string row_text(int row) const;
    int scrollback_rows() const;
    // Lines that have gone into history, less those brought back. A line keeps
    // the same row plus this as more scroll off, so a selection can follow it.
    long long lines_scrolled() const { return lines_scrolled_; }
    TerminalCursor cursor() const;
    const std::string& title() const { return title_; }
    // A full-screen program's screen, which keeps no history.
    bool alt_screen() const { return alt_screen_; }

    void key(TerminalKey key, TerminalMods mods);
    void character(char32_t c, TerminalMods mods);
    // Bracketed when the program asked for that. Line breaks are sent as Enter.
    void paste(std::string_view text);
    // Told to the program when it asked to know.
    void focus(bool focused);
    // The 16 basic colors, black to bright white, that SGR 30-37 and 90-97 pick.
    // Cells already written change with them.
    void set_palette(const std::array<TerminalColor, 16>& colors);

    // The most history rows kept.
    static constexpr std::size_t kScrollbackLimit = 5000;

private:
    struct Vt;
    friend struct TerminalScreenCallbacks;

    Vt* vt_;
    std::function<void(std::string_view)> on_reply_;
    std::string title_;
    std::string title_pending_;
    bool alt_screen_ = false;
    bool cursor_visible_ = true;
    long long lines_scrolled_ = 0;
};

}  // namespace ide
