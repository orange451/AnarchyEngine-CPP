#pragma once

#include "TerminalScreen.hpp"

#include "jadefx/jadefx.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ide {

class ThemeListener;

// WCAG's contrast ratio of two colors, from 1:1 to 21:1. Alpha is ignored.
double ContrastRatio(const jadefx::Color& a, const jadefx::Color& b);
// fg, moved toward black or white until its contrast with bg is at least
// ratio. It moves away from bg first, and the other way when that side has no
// room. A color that already reads comes back as it was. VS Code's terminal
// does the same, so programs that choose colors for a dark background stay
// readable on a light one.
jadefx::Color EnsureContrast(jadefx::Color fg, const jadefx::Color& bg, double ratio);
// The 16 basic colors for a light or a dark background, as VS Code's terminal has them.
std::array<TerminalColor, 16> AnsiPalette(bool light_background);

// Part of a block element or a box-drawing line, drawn in the cell's color.
struct CellShape {
    enum class Kind {
        // Fills x, y, width, height.
        Rect,
        // A quarter circle line-thick from the middle of one edge to the middle of
        // the next, bending round the cell's corner: 0 top left, 1 top right,
        // 2 bottom right, 3 bottom left. x, y, width, height is the cell.
        Arc,
    };
    Kind kind = Kind::Rect;
    float x = 0.f;
    float y = 0.f;
    float width = 0.f;
    float height = 0.f;
    // Of the color, for the shade blocks.
    float alpha = 1.f;
    int corner = 0;
    float thickness = 0.f;
};

// What a block element (U+2580 to U+259F) or a light, heavy, or rounded
// box-drawing line is drawn as, in the cell at x, y. Terminals draw these
// rather than use the font's glyphs, which leave gaps between rows and
// columns. line is a light line's thickness; pixel is one device pixel in
// points, and every edge lands on one, so neighboring cells meet exactly.
// Empty for any other character, which the font draws.
std::vector<CellShape> CellShapes(char32_t c, float x, float y, float width, float height, float line, float pixel);

// Draws a TerminalScreen as a grid of cells in the editor's monospace font,
// and sends it what is typed while it has focus. It takes keys before the
// studio's shortcuts, so Ctrl+C, Ctrl+S, Tab, and Escape reach the program;
// Cmd shortcuts on macOS, and Ctrl+Shift ones elsewhere, still reach the
// studio. Paste is Cmd+V on macOS, Ctrl+V on Windows, and Ctrl+Shift+V on
// Linux. The wheel scrolls back through the lines that went off the top, as
// do Cmd+Up, Cmd+Down, Cmd+Home, and Cmd+End on macOS (Ctrl+Shift with them
// elsewhere) and Shift+Page Up and Shift+Page Down; typing comes back to the
// newest. Dragging selects text, a double-click a word, a triple-click a line,
// and Shift+click extends the selection. Copy is Cmd+C on macOS and
// Ctrl+Shift+C elsewhere, and on Windows Ctrl+C copies while there is a
// selection.
class TerminalView : public jadefx::Region {
public:
    TerminalView();
    ~TerminalView() override;

    const char* getElementType() const override { return "terminal"; }

    TerminalScreen& screen() { return screen_; }
    // Called when a layout changes the grid, with its new columns and rows.
    void setOnResize(std::function<void(int cols, int rows)> handler) { on_resize_ = std::move(handler); }
    // Rows scrolled back into history. 0 shows the newest.
    int scrollOffset() const { return scroll_; }
    bool hasSelection() const { return selecting_ && !(anchor_ == head_); }
    // The selected text, one line per row without trailing blanks.
    std::string selectedText() const;

protected:
    void layoutChildren() override;
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void handleKey(jadefx::KeyEvent& event) override;
    void handleText(jadefx::TextEvent& event) override;
    void handleScroll(jadefx::ScrollEvent& event) override;
    void handleMousePressed(const jadefx::MouseEvent& event) override;
    void handleMouseDragged(const jadefx::MouseEvent& event) override;
    void handleFocusGained() override;
    void handleFocusLost() override;

private:
    // A place in the text: a line, counted so it stays put as lines scroll
    // into history, and the edge before a column.
    struct Spot {
        long long line = 0;
        int col = 0;
        bool operator==(const Spot& other) const { return line == other.line && col == other.col; }
        bool operator<(const Spot& other) const {
            return line < other.line || (line == other.line && col < other.col);
        }
    };

    // Cell size in points, from the font.
    void measure();
    void paste();
    void copy();
    void to_bottom() { scroll_ = 0; }
    // Scrolls history by rows, up for more than 0, clamped to what there is.
    void scroll_by(int rows);
    // Up to Cmd+arrows, Shift+Page keys, and their kin, which scroll rather than reach the program.
    bool scroll_key(const jadefx::KeyEvent& event);
    // The spot at a point in the scene: the nearest edge between two cells, or
    // with edge false, the left edge of the cell the point is in.
    Spot spot_at(double x, double y, bool edge = true) const;
    // The screen row, negative in history, that a spot's line is on now.
    int row_of(long long line) const { return static_cast<int>(line - screen_.lines_scrolled()); }
    // The cells a press selects by itself: a word for a double-click, a line for a triple.
    void select_unit(const Spot& spot, int clicks);
    void clear_selection() { selecting_ = false; }
    // The basic colors for the theme's background, light or dark.
    void apply_theme();
    // fg as drawn on bg: EnsureContrast, remembered for the colors seen so far.
    jadefx::Color readable(const jadefx::Color& fg, const jadefx::Color& bg);

    TerminalScreen screen_;
    std::unique_ptr<ThemeListener> theme_listener_;
    std::unordered_map<std::uint64_t, jadefx::Color> readable_;
    std::function<void(int, int)> on_resize_;
    float font_size_ = 14.f;
    float cell_width_ = 8.f;
    float row_height_ = 16.f;
    float ascent_gap_ = 0.f;
    int scroll_ = 0;
    // Where the selection started and where it ends now, in either order.
    Spot anchor_;
    Spot head_;
    bool selecting_ = false;
    // A double- or triple-click's word or line, kept whole while the drag that follows extends it.
    Spot unit_start_;
    Spot unit_end_;
    // The first layout with room reports its size even when it matches the screen's.
    bool sized_ = false;
    // An Alt+key was sent from its key event, so its text event is dropped.
    bool swallow_text_ = false;
};

}  // namespace ide
