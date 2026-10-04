#include "TerminalView.hpp"

#include "IdeTheme.hpp"
#include "Utf8.hpp"

#include "jadefx/scene/Painter.hpp"
#include "jadefx/scene/text/Font.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace ide {
namespace {

namespace Key = jadefx::Key;

// Lines the wheel moves for each notch.
constexpr double kWheelRows = 3.0;
// WCAG AA for text, and VS Code's terminal default.
constexpr double kMinimumContrast = 4.5;

// Draws what the monospace font has no glyph for, such as ❯ and ⏺, which
// command-line programs draw their interfaces with.
const std::string& symbol_family() {
    static const std::string family = [] {
        const char* paths[] = {
            "C:/Windows/Fonts/seguisym.ttf",
            "/System/Library/Fonts/Apple Symbols.ttf",
            "/System/Library/Fonts/Menlo.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        };
        for (const char* path : paths) {
            if (jadefx::Font::loadFile("Terminal Symbols", path)) {
                return std::string("Terminal Symbols");
            }
        }
        return editor_mono_family();
    }();
    return family;
}

// Besides letters and digits, what a double-click takes as part of a word,
// so a path, a URL, or file:line comes whole.
bool IsWordCharacter(const std::u32string& chars) {
    if (chars.empty()) {
        return false;
    }
    const char32_t c = chars[0];
    if (c < 0x80) {
        return (c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') ||
               std::u32string_view(U"_-./~:@%+#?=&").find(c) != std::u32string_view::npos;
    }
    // Not the spaces, punctuation, arrows, box drawing, and symbols that programs draw their interfaces with.
    return c != 0xA0 && c != 0x3000 && (c < 0x2000 || c > 0x2BFF);
}

bool TerminalKeyFor(int key, TerminalKey& out) {
    switch (key) {
        case Key::Enter:
        case Key::KpEnter: out = TerminalKey::Enter; return true;
        case Key::Tab: out = TerminalKey::Tab; return true;
        case Key::Backspace: out = TerminalKey::Backspace; return true;
        case Key::Escape: out = TerminalKey::Escape; return true;
        case Key::Up: out = TerminalKey::Up; return true;
        case Key::Down: out = TerminalKey::Down; return true;
        case Key::Left: out = TerminalKey::Left; return true;
        case Key::Right: out = TerminalKey::Right; return true;
        case Key::Insert: out = TerminalKey::Insert; return true;
        case Key::Delete: out = TerminalKey::Delete; return true;
        case Key::Home: out = TerminalKey::Home; return true;
        case Key::End: out = TerminalKey::End; return true;
        case Key::PageUp: out = TerminalKey::PageUp; return true;
        case Key::PageDown: out = TerminalKey::PageDown; return true;
        default: break;
    }
    if (key >= Key::F1 && key <= Key::F12) {
        out = static_cast<TerminalKey>(static_cast<int>(TerminalKey::F1) + (key - Key::F1));
        return true;
    }
    return false;
}

// The character a key with Ctrl or Alt held stands for. Key codes are ASCII
// for letters, digits, and punctuation.
bool CharacterFor(const jadefx::KeyEvent& event, char32_t& out) {
    if (event.key >= Key::A && event.key <= Key::Z) {
        out = static_cast<char32_t>(event.shift ? event.key : event.key + ('a' - 'A'));
        return true;
    }
    if (event.key >= Key::Space && event.key < Key::A) {
        out = static_cast<char32_t>(event.key);
        return true;
    }
    if (event.key >= Key::LeftBracket && event.key <= Key::RightBracket) {
        out = static_cast<char32_t>(event.key);
        return true;
    }
    return false;
}

jadefx::Color ColorOf(const TerminalColor& color, const jadefx::Color& fallback) {
    if (color.is_default) {
        return fallback;
    }
    return jadefx::Color::rgb8(color.r, color.g, color.b);
}

bool Same(const jadefx::Color& a, const jadefx::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

jadefx::Color Faded(jadefx::Color color, float opacity) {
    color.a *= opacity;
    return color;
}

// One cell as it is drawn: its colors after reverse video and the cursor.
struct Painted {
    TerminalCell cell;
    jadefx::Color fg;
    // Before the contrast rule, for block and box-drawing shapes.
    jadefx::Color shape_fg;
    jadefx::Color bg;
    bool filled = false;
};

}  // namespace

namespace {

double Linear(float channel) {
    const double c = std::clamp(static_cast<double>(channel), 0.0, 1.0);
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// WCAG's relative luminance.
double Luminance(const jadefx::Color& color) {
    return 0.2126 * Linear(color.r) + 0.7152 * Linear(color.g) + 0.0722 * Linear(color.b);
}

jadefx::Color Toward(const jadefx::Color& from, const jadefx::Color& to, float amount) {
    jadefx::Color out = from;
    out.r = from.r + (to.r - from.r) * amount;
    out.g = from.g + (to.g - from.g) * amount;
    out.b = from.b + (to.b - from.b) * amount;
    return out;
}

// The first step from fg toward target that reaches ratio, or target itself.
jadefx::Color Reach(const jadefx::Color& fg, const jadefx::Color& bg, const jadefx::Color& target, double ratio,
                    bool& reached) {
    for (int step = 1; step <= 20; ++step) {
        const jadefx::Color next = Toward(fg, target, static_cast<float>(step) / 20.f);
        if (ContrastRatio(next, bg) >= ratio) {
            reached = true;
            return next;
        }
    }
    reached = false;
    return Toward(fg, target, 1.f);
}

TerminalColor Hex(std::uint32_t rgb) {
    TerminalColor out;
    out.r = static_cast<std::uint8_t>(rgb >> 16);
    out.g = static_cast<std::uint8_t>(rgb >> 8);
    out.b = static_cast<std::uint8_t>(rgb);
    out.is_default = false;
    return out;
}

}  // namespace

double ContrastRatio(const jadefx::Color& a, const jadefx::Color& b) {
    const double la = Luminance(a);
    const double lb = Luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

jadefx::Color EnsureContrast(jadefx::Color fg, const jadefx::Color& bg, double ratio) {
    if (ContrastRatio(fg, bg) >= ratio) {
        return fg;
    }
    const jadefx::Color black = jadefx::Color::rgba(0.f, 0.f, 0.f, fg.a);
    const jadefx::Color white = jadefx::Color::rgba(1.f, 1.f, 1.f, fg.a);
    const double lf = Luminance(fg);
    const double lb = Luminance(bg);
    // Away from the background first: darker on a lighter one.
    const bool darken = lf < lb || (lf == lb && lb > 0.5);
    bool reached = false;
    const jadefx::Color first = Reach(fg, bg, darken ? black : white, ratio, reached);
    if (reached) {
        return first;
    }
    const jadefx::Color second = Reach(fg, bg, darken ? white : black, ratio, reached);
    if (reached) {
        return second;
    }
    return ContrastRatio(first, bg) >= ContrastRatio(second, bg) ? first : second;
}

std::array<TerminalColor, 16> AnsiPalette(bool light_background) {
    static const std::uint32_t kLight[16] = {0x000000, 0xcd3131, 0x00bc00, 0x949800, 0x0451a5, 0xbc05bc,
                                             0x0598bc, 0x555555, 0x666666, 0xcd3131, 0x14ce14, 0xb5ba00,
                                             0x0451a5, 0xbc05bc, 0x0598bc, 0xa5a5a5};
    static const std::uint32_t kDark[16] = {0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc,
                                            0x11a8cd, 0xe5e5e5, 0x666666, 0xf14c4c, 0x23d18b, 0xf5f543,
                                            0x3b8eea, 0xd670d6, 0x29b8db, 0xe5e5e5};
    std::array<TerminalColor, 16> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = Hex(light_background ? kLight[i] : kDark[i]);
    }
    return out;
}

namespace {

float Snap(float value, float pixel) { return std::round(value / pixel) * pixel; }

// The lines a box-drawing character sends to each edge: 0 none, 1 light, 2 heavy.
struct Arms {
    int up = 0;
    int right = 0;
    int down = 0;
    int left = 0;
};

bool BoxArms(char32_t c, Arms& arms) {
    switch (c) {
        case U'─': arms = {0, 1, 0, 1}; return true;  // ─
        case U'━': arms = {0, 2, 0, 2}; return true;  // ━
        case U'│': arms = {1, 0, 1, 0}; return true;  // │
        case U'┃': arms = {2, 0, 2, 0}; return true;  // ┃
        case U'┌': arms = {0, 1, 1, 0}; return true;  // ┌
        case U'┏': arms = {0, 2, 2, 0}; return true;  // ┏
        case U'┐': arms = {0, 0, 1, 1}; return true;  // ┐
        case U'┓': arms = {0, 0, 2, 2}; return true;  // ┓
        case U'└': arms = {1, 1, 0, 0}; return true;  // └
        case U'┗': arms = {2, 2, 0, 0}; return true;  // ┗
        case U'┘': arms = {1, 0, 0, 1}; return true;  // ┘
        case U'┛': arms = {2, 0, 0, 2}; return true;  // ┛
        case U'├': arms = {1, 1, 1, 0}; return true;  // ├
        case U'┣': arms = {2, 2, 2, 0}; return true;  // ┣
        case U'┤': arms = {1, 0, 1, 1}; return true;  // ┤
        case U'┫': arms = {2, 0, 2, 2}; return true;  // ┫
        case U'┬': arms = {0, 1, 1, 1}; return true;  // ┬
        case U'┳': arms = {0, 2, 2, 2}; return true;  // ┳
        case U'┴': arms = {1, 1, 0, 1}; return true;  // ┴
        case U'┻': arms = {2, 2, 0, 2}; return true;  // ┻
        case U'┼': arms = {1, 1, 1, 1}; return true;  // ┼
        case U'╋': arms = {2, 2, 2, 2}; return true;  // ╋
        case U'╴': arms = {0, 0, 0, 1}; return true;  // ╴
        case U'╵': arms = {1, 0, 0, 0}; return true;  // ╵
        case U'╶': arms = {0, 1, 0, 0}; return true;  // ╶
        case U'╷': arms = {0, 0, 1, 0}; return true;  // ╷
        case U'╸': arms = {0, 0, 0, 2}; return true;  // ╸
        case U'╹': arms = {2, 0, 0, 0}; return true;  // ╹
        case U'╺': arms = {0, 2, 0, 0}; return true;  // ╺
        case U'╻': arms = {0, 0, 2, 0}; return true;  // ╻
        case U'╼': arms = {0, 2, 0, 1}; return true;  // ╼
        case U'╽': arms = {1, 0, 2, 0}; return true;  // ╽
        case U'╾': arms = {0, 1, 0, 2}; return true;  // ╾
        case U'╿': arms = {2, 0, 1, 0}; return true;  // ╿
        default: return false;
    }
}

}  // namespace

std::vector<CellShape> CellShapes(char32_t c, float x, float y, float width, float height, float line, float pixel) {
    std::vector<CellShape> out;
    pixel = pixel > 0.f ? pixel : 1.f;
    const float left = Snap(x, pixel);
    const float top = Snap(y, pixel);
    const float right = Snap(x + width, pixel);
    const float bottom = Snap(y + height, pixel);
    const float w = right - left;
    const float h = bottom - top;
    auto rect = [&](float x0, float y0, float x1, float y1, float alpha = 1.f) {
        CellShape shape;
        shape.x = x0;
        shape.y = y0;
        shape.width = x1 - x0;
        shape.height = y1 - y0;
        shape.alpha = alpha;
        if (shape.width > 0.f && shape.height > 0.f) {
            out.push_back(shape);
        }
    };
    // A fraction of the cell across and down, on whole pixels.
    auto fraction = [&](float fx0, float fy0, float fx1, float fy1, float alpha = 1.f) {
        rect(Snap(left + w * fx0, pixel), Snap(top + h * fy0, pixel), Snap(left + w * fx1, pixel),
             Snap(top + h * fy1, pixel), alpha);
    };

    if (c >= U'▀' && c <= U'▟') {
        const int code = static_cast<int>(c - U'▀');
        if (c == U'▀') {
            fraction(0.f, 0.f, 1.f, 0.5f);
        } else if (c <= U'█') {
            // ▁ to █: the lower eighths.
            fraction(0.f, 1.f - static_cast<float>(code) / 8.f, 1.f, 1.f);
        } else if (c <= U'▏') {
            // ▉ to ▏: the left seven eighths down to one.
            fraction(0.f, 0.f, static_cast<float>(0x10 - code) / 8.f, 1.f);
        } else if (c == U'▐') {
            fraction(0.5f, 0.f, 1.f, 1.f);
        } else if (c <= U'▓') {
            // ░ ▒ ▓: a quarter, a half, and three quarters of the color.
            fraction(0.f, 0.f, 1.f, 1.f, static_cast<float>(code - 0x10) * 0.25f);
        } else if (c == U'▔') {
            fraction(0.f, 0.f, 1.f, 0.125f);
        } else if (c == U'▕') {
            fraction(0.875f, 0.f, 1.f, 1.f);
        } else {
            // ▖ to ▟: quadrants, as bits for upper left 1, upper right 2, lower left 4, lower right 8.
            static const int kQuadrants[10] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};
            const int bits = kQuadrants[code - 0x16];
            const float mid_x = Snap(left + w * 0.5f, pixel);
            const float mid_y = Snap(top + h * 0.5f, pixel);
            if (bits & 1) {
                rect(left, top, mid_x, mid_y);
            }
            if (bits & 2) {
                rect(mid_x, top, right, mid_y);
            }
            if (bits & 4) {
                rect(left, mid_y, mid_x, bottom);
            }
            if (bits & 8) {
                rect(mid_x, mid_y, right, bottom);
            }
        }
        return out;
    }

    const float light = std::max(pixel, Snap(line, pixel));
    const float heavy = std::max(2.f * pixel, Snap(line * 2.f, pixel));
    // ╭ ╮ ╯ ╰ bend round the top left, top right, bottom right, and bottom left.
    if (c >= U'╭' && c <= U'╰') {
        CellShape shape;
        shape.kind = CellShape::Kind::Arc;
        shape.x = left;
        shape.y = top;
        shape.width = w;
        shape.height = h;
        shape.corner = static_cast<int>(c - U'╭');
        shape.thickness = light;
        out.push_back(shape);
        return out;
    }

    Arms arms;
    if (!BoxArms(c, arms)) {
        return out;
    }
    auto thick = [&](int weight) { return weight == 2 ? heavy : light; };
    // Where a vertical and a horizontal line of each weight run, centered in the cell.
    auto column = [&](int weight) { return Snap(left + (w - thick(weight)) * 0.5f, pixel); };
    auto row = [&](int weight) { return Snap(top + (h - thick(weight)) * 0.5f, pixel); };
    // Arms meet over the thickest line crossing them, so a corner has no notch.
    const int across = std::max(arms.left, arms.right);
    const int along = std::max(arms.up, arms.down);
    if (arms.left != 0 && arms.left == arms.right) {
        rect(left, row(arms.left), right, row(arms.left) + thick(arms.left));
    } else {
        if (arms.left != 0) {
            const float end = along != 0 ? column(along) + thick(along) : Snap(left + w * 0.5f, pixel);
            rect(left, row(arms.left), end, row(arms.left) + thick(arms.left));
        }
        if (arms.right != 0) {
            const float start = along != 0 ? column(along) : Snap(left + w * 0.5f, pixel);
            rect(start, row(arms.right), right, row(arms.right) + thick(arms.right));
        }
    }
    if (arms.up != 0 && arms.up == arms.down) {
        rect(column(arms.up), top, column(arms.up) + thick(arms.up), bottom);
    } else {
        if (arms.up != 0) {
            const float end = across != 0 ? row(across) + thick(across) : Snap(top + h * 0.5f, pixel);
            rect(column(arms.up), top, column(arms.up) + thick(arms.up), end);
        }
        if (arms.down != 0) {
            const float start = across != 0 ? row(across) : Snap(top + h * 0.5f, pixel);
            rect(column(arms.down), start, column(arms.down) + thick(arms.down), bottom);
        }
    }
    return out;
}

namespace {

// A light box-drawing line, in points. At least one device pixel.
constexpr float kLineThickness = 1.f;

// A rounded corner: a CSS border with one rounded corner and its two sides,
// running past the cell and clipped to it, so its straight parts meet the
// lines in the cells beside it.
void DrawArc(jadefx::Painter& painter, const CellShape& shape, float pixel, const jadefx::Color& color) {
    const float t = shape.thickness;
    const float column = Snap(shape.x + (shape.width - t) * 0.5f, pixel);
    const float row = Snap(shape.y + (shape.height - t) * 0.5f, pixel);
    const float radius_size = std::min(shape.width, shape.height) * 0.5f;
    const float big = shape.width + shape.height;
    float radius[4] = {};
    float sides[4] = {};
    const bool right_arm = shape.corner == 0 || shape.corner == 3;
    const bool down_arm = shape.corner == 0 || shape.corner == 1;
    const float x = right_arm ? column : column + t - big;
    const float y = down_arm ? row : row + t - big;
    radius[shape.corner] = radius_size;
    sides[down_arm ? 0 : 2] = t;
    sides[right_arm ? 3 : 1] = t;
    painter.pushClip(shape.x, shape.y, shape.width, shape.height);
    painter.strokeRounded(x, y, big, big, radius, sides, color);
    painter.popClip();
}

}  // namespace

TerminalView::TerminalView() : screen_(24, 80) {
    getClassList().add("ide-terminal");
    setFocusTraversable(true);
    // Ctrl+C, Ctrl+S, and Tab are the program's while it has focus, not the studio's.
    setCapturesKeys(true);
    setDefaultCursor(jadefx::Cursor::Text);
    measure();
    apply_theme();
    theme_listener_ = std::make_unique<ThemeListener>([this] { apply_theme(); });
}

TerminalView::~TerminalView() = default;

void TerminalView::apply_theme() {
    screen_.set_palette(AnsiPalette(Luminance(theme_color("--ide-editor-color")) > 0.5));
    readable_.clear();
}

jadefx::Color TerminalView::readable(const jadefx::Color& fg, const jadefx::Color& bg) {
    auto byte = [](float channel) {
        return static_cast<std::uint64_t>(std::lround(std::clamp(channel, 0.f, 1.f) * 255.f));
    };
    const std::uint64_t key = byte(fg.r) << 40 | byte(fg.g) << 32 | byte(fg.b) << 24 | byte(bg.r) << 16 |
                              byte(bg.g) << 8 | byte(bg.b);
    const auto found = readable_.find(key);
    if (found != readable_.end()) {
        return found->second;
    }
    if (readable_.size() > 4096) {
        readable_.clear();
    }
    const jadefx::Color out = EnsureContrast(fg, bg, kMinimumContrast);
    readable_.emplace(key, out);
    return out;
}

void TerminalView::measure() {
    const jadefx::Font font(editor_mono_family(), font_size_);
    cell_width_ = std::max(1.f, font.measureWidth("M"));
    row_height_ = std::max(1.f, std::ceil(font.lineHeight()));
    ascent_gap_ = (row_height_ - font.lineHeight()) * 0.5f;
}

void TerminalView::layoutChildren() {
    Region::layoutChildren();
    const double width = contentWidth();
    const double height = contentHeight();
    if (width <= 0.0 || height <= 0.0) {
        return;
    }
    const int cols = std::max(2, static_cast<int>(width / cell_width_));
    const int rows = std::max(1, static_cast<int>(height / row_height_));
    if (sized_ && cols == screen_.cols() && rows == screen_.rows()) {
        return;
    }
    sized_ = true;
    // The program redraws for the new size, so what was selected may have moved.
    clear_selection();
    screen_.resize(rows, cols);
    scroll_ = std::min(scroll_, screen_.scrollback_rows());
    if (on_resize_) {
        on_resize_(cols, rows);
    }
}

void TerminalView::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    jadefx::Painter painter(renderer);
    const float left = static_cast<float>(getAbsoluteX() + contentLeft());
    const float top = static_cast<float>(getAbsoluteY() + contentTop());
    painter.pushClip(static_cast<float>(getAbsoluteX()), static_cast<float>(getAbsoluteY()),
                     static_cast<float>(getWidth()), static_cast<float>(getHeight()));

    const jadefx::Color background = theme_color("--ide-editor-color");
    const jadefx::Color foreground = theme_color("--ide-editor-text-color");
    const std::string& mono = editor_mono_family();
    const std::string& symbols = symbol_family();
    const jadefx::Font mono_font(mono, font_size_);
    const int rows = screen_.rows();
    const int cols = screen_.cols();
    const TerminalCursor cursor = screen_.cursor();
    const bool show_cursor = cursor.visible && scroll_ == 0;
    auto x_of = [&](int col) { return left + static_cast<float>(col) * cell_width_; };
    // One device pixel, in points.
    const float pixel = 1.f / std::max(0.01f, painter.pixelsPerPoint());
    const bool selected = hasSelection();
    const Spot selection_start = std::min(anchor_, head_);
    const Spot selection_end = std::max(anchor_, head_);
    const jadefx::Color selection_color = theme_color("--text-selection-color");

    std::vector<Painted> line(static_cast<std::size_t>(cols));
    for (int r = 0; r < rows; ++r) {
        const int row = r - scroll_;
        const float y = top + static_cast<float>(r) * row_height_;
        for (int col = 0; col < cols; ++col) {
            Painted& paint = line[static_cast<std::size_t>(col)];
            paint.cell = screen_.cell(row, col);
            paint.fg = ColorOf(paint.cell.fg, foreground);
            paint.bg = ColorOf(paint.cell.bg, background);
            paint.filled = !paint.cell.bg.is_default;
            if (paint.cell.reverse) {
                std::swap(paint.fg, paint.bg);
                paint.filled = true;
            }
            // A focused page shows a block cursor, with the character over it in the background color.
            if (show_cursor && isFocused() && row == cursor.row && col == cursor.col) {
                std::swap(paint.fg, paint.bg);
                paint.filled = true;
            }
            // Shapes keep their color, as xterm.js leaves U+2500 to U+259F out of its contrast rule.
            paint.shape_fg = paint.fg;
            paint.fg = readable(paint.fg, paint.bg);
        }

        // Backgrounds, a run of one color at a time.
        for (int col = 0; col < cols;) {
            const Painted& first = line[static_cast<std::size_t>(col)];
            int end = col + 1;
            while (end < cols && line[static_cast<std::size_t>(end)].filled == first.filled &&
                   Same(line[static_cast<std::size_t>(end)].bg, first.bg)) {
                ++end;
            }
            if (first.filled) {
                // On whole pixels, so neighboring runs meet without a seam.
                const float x0 = Snap(x_of(col), pixel);
                const float x1 = Snap(x_of(end), pixel);
                const float y0 = Snap(y, pixel);
                painter.fillRect(x0, y0, x1 - x0, Snap(y + row_height_, pixel) - y0, Faded(first.bg, opacity));
            }
            col = end;
        }

        // The selection, over the backgrounds and under the text.
        const long long line_here = screen_.lines_scrolled() + row;
        if (selected && line_here >= selection_start.line && line_here <= selection_end.line) {
            const int from = line_here == selection_start.line ? selection_start.col : 0;
            const int to = line_here == selection_end.line ? selection_end.col : cols;
            if (to > from) {
                const float x0 = Snap(x_of(from), pixel);
                const float x1 = Snap(x_of(to), pixel);
                const float y0 = Snap(y, pixel);
                painter.fillRect(x0, y0, x1 - x0, Snap(y + row_height_, pixel) - y0, Faded(selection_color, opacity));
            }
        }

        // Text: ASCII in runs, since the font's advance is the cell width, and
        // anything else one cell at a time, in the symbol font when the
        // monospace one has no glyph for it.
        std::string run;
        int run_col = 0;
        jadefx::Color run_color;
        bool run_bold = false;
        auto draw = [&](float x, const std::string& text, const std::string& family, const jadefx::Color& color,
                        bool bold) {
            painter.text(x, y + ascent_gap_, text, family, font_size_, Faded(color, opacity));
            if (bold) {
                painter.text(x + 0.6f, y + ascent_gap_, text, family, font_size_, Faded(color, opacity));
            }
        };
        auto flush = [&] {
            if (!run.empty()) {
                draw(x_of(run_col), run, mono, run_color, run_bold);
                run.clear();
            }
        };
        for (int col = 0; col < cols; ++col) {
            const Painted& paint = line[static_cast<std::size_t>(col)];
            const std::u32string& chars = paint.cell.chars;
            if (chars.empty() || chars == U" ") {
                flush();
                continue;
            }
            if (chars.size() == 1 && chars[0] > U' ' && chars[0] < 0x7F) {
                if (!run.empty() && (!Same(run_color, paint.fg) || run_bold != paint.cell.bold)) {
                    flush();
                }
                if (run.empty()) {
                    run_col = col;
                    run_color = paint.fg;
                    run_bold = paint.cell.bold;
                }
                run.push_back(static_cast<char>(chars[0]));
                continue;
            }
            flush();
            const float cell_span = cell_width_ * static_cast<float>(std::max(1, paint.cell.width));
            const std::vector<CellShape> shapes =
                CellShapes(chars[0], x_of(col), y, cell_span, row_height_, kLineThickness, pixel);
            if (!shapes.empty()) {
                for (const CellShape& shape : shapes) {
                    jadefx::Color color = Faded(paint.shape_fg, opacity);
                    color.a *= shape.alpha;
                    if (shape.kind == CellShape::Kind::Rect) {
                        painter.fillRect(shape.x, shape.y, shape.width, shape.height, color);
                    } else {
                        DrawArc(painter, shape, pixel, color);
                    }
                }
                continue;
            }
            const std::string& family = mono_font.hasGlyph(chars[0]) ? mono : symbols;
            draw(x_of(col), Utf8(chars), family, paint.fg, paint.cell.bold);
        }
        flush();

        // Underlines and strikethroughs, under and through each cell's width.
        for (int col = 0; col < cols; ++col) {
            const Painted& paint = line[static_cast<std::size_t>(col)];
            const float width = cell_width_ * static_cast<float>(std::max(1, paint.cell.width));
            if (paint.cell.underline) {
                painter.fillRect(x_of(col), y + row_height_ - 2.f, width, 1.f, Faded(paint.fg, opacity));
            }
            if (paint.cell.strike) {
                painter.fillRect(x_of(col), y + row_height_ * 0.5f, width, 1.f, Faded(paint.fg, opacity));
            }
        }
    }

    // Without focus, the cursor is an outline, as terminals draw it.
    if (show_cursor && !isFocused() && cursor.row >= 0 && cursor.row < rows && cursor.col >= 0 && cursor.col < cols) {
        const float x = x_of(cursor.col);
        const float y = top + static_cast<float>(cursor.row) * row_height_;
        const jadefx::Color color = Faded(foreground, opacity);
        painter.fillRect(x, y, cell_width_, 1.f, color);
        painter.fillRect(x, y + row_height_ - 1.f, cell_width_, 1.f, color);
        painter.fillRect(x, y, 1.f, row_height_, color);
        painter.fillRect(x + cell_width_ - 1.f, y, 1.f, row_height_, color);
    }
    painter.popClip();
}

void TerminalView::paste() {
    if (const jadefx::Scene* scene = getScene()) {
        const std::string text = scene->clipboardText();
        if (!text.empty()) {
            to_bottom();
            screen_.paste(text);
        }
    }
}

void TerminalView::copy() {
    if (jadefx::Scene* scene = getScene()) {
        scene->setClipboardText(selectedText());
    }
}

std::string TerminalView::selectedText() const {
    if (!hasSelection()) {
        return {};
    }
    const Spot start = std::min(anchor_, head_);
    const Spot end = std::max(anchor_, head_);
    std::string out;
    for (long long line = start.line; line <= end.line; ++line) {
        const int row = row_of(line);
        const int from = line == start.line ? start.col : 0;
        const int to = line == end.line ? end.col : screen_.cols();
        std::u32string text;
        for (int col = from; col < to; ++col) {
            const TerminalCell cell = screen_.cell(row, col);
            if (!cell.chars.empty()) {
                text += cell.chars;
            } else if (col == 0 || screen_.cell(row, col - 1).width != 2) {
                // A blank, and not the second half of a wide character.
                text += U' ';
            }
        }
        while (!text.empty() && text.back() == U' ') {
            text.pop_back();
        }
        out += Utf8(text);
        if (line != end.line) {
            out += '\n';
        }
    }
    return out;
}

void TerminalView::scroll_by(int rows) { scroll_ = std::clamp(scroll_ + rows, 0, screen_.scrollback_rows()); }

bool TerminalView::scroll_key(const jadefx::KeyEvent& event) {
    // A full-screen program keeps no history, and these keys are its own.
    if (screen_.alt_screen()) {
        return false;
    }
#if defined(__APPLE__)
    const bool held = event.meta && !event.control && !event.alt && !event.shift;
#else
    const bool held = event.control && event.shift && !event.alt && !event.meta;
#endif
    const bool shift = event.shift && !event.control && !event.alt && !event.meta;
    const int page = std::max(1, screen_.rows() - 1);
    if (held) {
        switch (event.key) {
            case Key::Up: scroll_by(1); return true;
            case Key::Down: scroll_by(-1); return true;
            case Key::PageUp: scroll_by(page); return true;
            case Key::PageDown: scroll_by(-page); return true;
            case Key::Home: scroll_by(screen_.scrollback_rows()); return true;
            case Key::End: to_bottom(); return true;
            default: break;
        }
    }
    if (shift && (event.key == Key::PageUp || event.key == Key::PageDown)) {
        scroll_by(event.key == Key::PageUp ? page : -page);
        return true;
    }
    return false;
}

void TerminalView::handleKey(jadefx::KeyEvent& event) {
    if (!event.pressed) {
        return;
    }
    swallow_text_ = false;
#if defined(__APPLE__)
    // Command is the studio's, but for paste, copy, and scrolling.
    if (event.meta) {
        const bool alone = !event.control && !event.alt && !event.shift;
        if (event.key == Key::V && alone) {
            paste();
            event.consume();
        } else if (event.key == Key::C && alone && hasSelection()) {
            copy();
            event.consume();
        } else if (scroll_key(event)) {
            event.consume();
        }
        return;
    }
#else
    // Ctrl+Shift is the terminal's own and the studio's, as in other terminals.
    if (event.control && event.shift) {
        if (event.key == Key::V) {
            paste();
            event.consume();
        } else if (event.key == Key::C && hasSelection()) {
            copy();
            event.consume();
        } else if (scroll_key(event)) {
            event.consume();
        }
        return;
    }
    if (event.meta) {
        return;
    }
#if defined(_WIN32)
    if (event.control && !event.alt && event.key == Key::V) {
        paste();
        event.consume();
        return;
    }
    // As in Windows Terminal: with a selection, Ctrl+C copies it rather than interrupting.
    if (event.control && !event.alt && event.key == Key::C && hasSelection()) {
        copy();
        clear_selection();
        event.consume();
        return;
    }
#endif
#endif
    if (scroll_key(event)) {
        event.consume();
        return;
    }
    TerminalMods mods;
    mods.shift = event.shift;
    mods.alt = event.alt;
    mods.ctrl = event.control;
    TerminalKey key;
    if (TerminalKeyFor(event.key, key)) {
        to_bottom();
        clear_selection();
        screen_.key(key, mods);
        event.consume();
        return;
    }
#if defined(__APPLE__)
    // Option types the characters it composes, which arrive as text.
    const bool alt_sends = false;
#else
    const bool alt_sends = event.alt;
#endif
    char32_t c = 0;
    if ((event.control || alt_sends) && CharacterFor(event, c)) {
        to_bottom();
        clear_selection();
        if (!alt_sends) {
            mods.alt = false;
        }
        screen_.character(c, mods);
        // Some platforms send Alt+X's text as well.
        swallow_text_ = alt_sends && !event.control;
    }
    // Plain keys are typed through their text. Every key stays here either way,
    // so the studio's single-key shortcuts do not act on what goes to the program.
    event.consume();
}

void TerminalView::handleText(jadefx::TextEvent& event) {
    event.consume();
    if (swallow_text_) {
        swallow_text_ = false;
        return;
    }
    to_bottom();
    clear_selection();
    for (const char32_t c : Utf32(event.text)) {
        screen_.character(c, TerminalMods{});
    }
}

void TerminalView::handleScroll(jadefx::ScrollEvent& event) {
    event.consumed = true;
    const int rows = static_cast<int>(std::lround(event.deltaY * kWheelRows));
    if (rows == 0) {
        return;
    }
    // A full-screen program has no history, so the wheel moves through it as arrow keys would.
    if (screen_.alt_screen()) {
        for (int i = 0; i < std::abs(rows); ++i) {
            screen_.key(rows > 0 ? TerminalKey::Up : TerminalKey::Down, TerminalMods{});
        }
        return;
    }
    scroll_by(rows);
}

TerminalView::Spot TerminalView::spot_at(double x, double y, bool edge) const {
    const double across = (x - getAbsoluteX() - contentLeft()) / cell_width_;
    const double down = (y - getAbsoluteY() - contentTop()) / row_height_;
    const int row = std::clamp(static_cast<int>(std::floor(down)), 0, screen_.rows() - 1);
    const int col = static_cast<int>(edge ? std::lround(across) : std::floor(across));
    Spot spot;
    spot.line = screen_.lines_scrolled() + row - scroll_;
    spot.col = std::clamp(col, 0, edge ? screen_.cols() : screen_.cols() - 1);
    return spot;
}

void TerminalView::select_unit(const Spot& spot, int clicks) {
    unit_start_ = spot;
    unit_end_ = spot;
    if (clicks >= 3) {
        unit_start_.col = 0;
        unit_end_.col = screen_.cols();
    } else {
        const int row = row_of(spot.line);
        // A wide character's second cell stands for its first.
        auto chars_at = [&](int col) {
            TerminalCell cell = screen_.cell(row, col);
            if (cell.chars.empty() && col > 0) {
                const TerminalCell before = screen_.cell(row, col - 1);
                if (before.width == 2) {
                    return before.chars;
                }
            }
            return cell.chars;
        };
        int from = spot.col;
        int to = spot.col + 1;
        if (IsWordCharacter(chars_at(spot.col))) {
            while (from > 0 && IsWordCharacter(chars_at(from - 1))) {
                --from;
            }
            while (to < screen_.cols() && IsWordCharacter(chars_at(to))) {
                ++to;
            }
        }
        unit_start_.col = from;
        unit_end_.col = to;
    }
    anchor_ = unit_start_;
    head_ = unit_end_;
    selecting_ = true;
}

void TerminalView::handleMousePressed(const jadefx::MouseEvent& event) {
    requestFocus();
    if (event.button != 0) {
        return;
    }
    if (event.clickCount >= 2) {
        select_unit(spot_at(event.x, event.y, false), event.clickCount);
        return;
    }
    const Spot spot = spot_at(event.x, event.y);
    // Shift+click moves the selection's far end and keeps where it started.
    if (!(event.shift() && selecting_)) {
        anchor_ = spot;
    }
    head_ = spot;
    unit_start_ = anchor_;
    unit_end_ = anchor_;
    selecting_ = true;
}

void TerminalView::handleMouseDragged(const jadefx::MouseEvent& event) {
    if (!selecting_) {
        return;
    }
    // Past the top or the bottom, the view scrolls to take in more.
    const double top = getAbsoluteY() + contentTop();
    if (event.y < top) {
        scroll_by(1);
    } else if (event.y >= top + contentHeight()) {
        scroll_by(-1);
    }
    const Spot spot = spot_at(event.x, event.y);
    // A word or line from a double- or triple-click stays whole, whichever way the drag goes.
    if (spot < unit_start_) {
        anchor_ = unit_end_;
        head_ = spot;
    } else if (unit_end_ < spot) {
        anchor_ = unit_start_;
        head_ = spot;
    } else {
        anchor_ = unit_start_;
        head_ = unit_end_;
    }
}

void TerminalView::handleFocusGained() { screen_.focus(true); }

void TerminalView::handleFocusLost() { screen_.focus(false); }

}  // namespace ide
