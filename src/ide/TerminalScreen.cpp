#include "TerminalScreen.hpp"

#include "Utf8.hpp"

#include <vterm.h>

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

namespace ide {

struct TerminalScreen::Vt {
    VTerm* term = nullptr;
    VTermScreen* screen = nullptr;
    VTermState* state = nullptr;
    // Lines that scrolled off the top, oldest first, without their trailing blanks.
    std::deque<std::vector<VTermScreenCell>> history;
};

namespace {

// libvterm marks the second cell of a wide character with this.
constexpr std::uint32_t kWideTail = 0xFFFFFFFFu;

bool IsBlank(const VTermScreenCell& cell) {
    return cell.chars[0] == 0 && VTERM_COLOR_IS_DEFAULT_BG(&cell.bg) && !cell.attrs.reverse;
}

TerminalColor Convert(const VTermScreen* screen, VTermColor color, bool is_default) {
    TerminalColor out;
    out.is_default = is_default;
    vterm_screen_convert_color_to_rgb(screen, &color);
    out.r = color.rgb.red;
    out.g = color.rgb.green;
    out.b = color.rgb.blue;
    return out;
}

TerminalCell Convert(const VTermScreen* screen, const VTermScreenCell& cell) {
    TerminalCell out;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0 && cell.chars[i] != kWideTail; ++i) {
        out.chars.push_back(static_cast<char32_t>(cell.chars[i]));
    }
    out.width = cell.width;
    out.fg = Convert(screen, cell.fg, VTERM_COLOR_IS_DEFAULT_FG(&cell.fg));
    out.bg = Convert(screen, cell.bg, VTERM_COLOR_IS_DEFAULT_BG(&cell.bg));
    out.bold = cell.attrs.bold != 0;
    out.italic = cell.attrs.italic != 0;
    out.underline = cell.attrs.underline != 0;
    out.strike = cell.attrs.strike != 0;
    out.reverse = cell.attrs.reverse != 0;
    return out;
}

VTermModifier Modifiers(TerminalMods mods) {
    int out = VTERM_MOD_NONE;
    if (mods.shift) {
        out |= VTERM_MOD_SHIFT;
    }
    if (mods.alt) {
        out |= VTERM_MOD_ALT;
    }
    if (mods.ctrl) {
        out |= VTERM_MOD_CTRL;
    }
    return static_cast<VTermModifier>(out);
}

VTermKey KeyFor(TerminalKey key) {
    switch (key) {
        case TerminalKey::Enter: return VTERM_KEY_ENTER;
        case TerminalKey::Tab: return VTERM_KEY_TAB;
        case TerminalKey::Backspace: return VTERM_KEY_BACKSPACE;
        case TerminalKey::Escape: return VTERM_KEY_ESCAPE;
        case TerminalKey::Up: return VTERM_KEY_UP;
        case TerminalKey::Down: return VTERM_KEY_DOWN;
        case TerminalKey::Left: return VTERM_KEY_LEFT;
        case TerminalKey::Right: return VTERM_KEY_RIGHT;
        case TerminalKey::Insert: return VTERM_KEY_INS;
        case TerminalKey::Delete: return VTERM_KEY_DEL;
        case TerminalKey::Home: return VTERM_KEY_HOME;
        case TerminalKey::End: return VTERM_KEY_END;
        case TerminalKey::PageUp: return VTERM_KEY_PAGEUP;
        case TerminalKey::PageDown: return VTERM_KEY_PAGEDOWN;
        default: break;
    }
    const int function = static_cast<int>(key) - static_cast<int>(TerminalKey::F1) + 1;
    return static_cast<VTermKey>(VTERM_KEY_FUNCTION(function));
}

}  // namespace

// libvterm's callbacks, with the screen as their user data.
struct TerminalScreenCallbacks {
    static void output(const char* bytes, std::size_t length, void* user) {
        auto* self = static_cast<TerminalScreen*>(user);
        if (self->on_reply_) {
            self->on_reply_(std::string_view(bytes, length));
        }
    }

    static int settermprop(VTermProp prop, VTermValue* value, void* user) {
        auto* self = static_cast<TerminalScreen*>(user);
        switch (prop) {
            case VTERM_PROP_ALTSCREEN:
                self->alt_screen_ = value->boolean != 0;
                break;
            case VTERM_PROP_CURSORVISIBLE:
                self->cursor_visible_ = value->boolean != 0;
                break;
            case VTERM_PROP_TITLE:
                if (value->string.initial) {
                    self->title_pending_.clear();
                }
                self->title_pending_.append(value->string.str, value->string.len);
                if (value->string.final) {
                    self->title_ = std::move(self->title_pending_);
                    self->title_pending_.clear();
                }
                break;
            default:
                break;
        }
        return 1;
    }

    static int pushline(int cols, const VTermScreenCell* cells, void* user) {
        auto* self = static_cast<TerminalScreen*>(user);
        int used = cols;
        while (used > 0 && IsBlank(cells[used - 1])) {
            --used;
        }
        auto& history = self->vt_->history;
        history.emplace_back(cells, cells + used);
        while (history.size() > TerminalScreen::kScrollbackLimit) {
            history.pop_front();
        }
        return 1;
    }

    // Brings the newest history line back, as when the screen grows taller.
    static int popline(int cols, VTermScreenCell* cells, void* user) {
        auto* self = static_cast<TerminalScreen*>(user);
        auto& history = self->vt_->history;
        if (history.empty()) {
            return 0;
        }
        const std::vector<VTermScreenCell>& line = history.back();
        VTermScreenCell blank{};
        blank.width = 1;
        vterm_state_get_default_colors(self->vt_->state, &blank.fg, &blank.bg);
        for (int col = 0; col < cols; ++col) {
            cells[col] = col < static_cast<int>(line.size()) ? line[static_cast<std::size_t>(col)] : blank;
        }
        history.pop_back();
        return 1;
    }

    static int clear(void* user) {
        static_cast<TerminalScreen*>(user)->vt_->history.clear();
        return 1;
    }
};

TerminalScreen::TerminalScreen(int rows, int cols) : vt_(new Vt) {
    vt_->term = vterm_new(std::max(1, rows), std::max(1, cols));
    vterm_set_utf8(vt_->term, 1);
    vterm_output_set_callback(vt_->term, &TerminalScreenCallbacks::output, this);
    vt_->state = vterm_obtain_state(vt_->term);
    vt_->screen = vterm_obtain_screen(vt_->term);

    static const VTermScreenCallbacks callbacks = [] {
        VTermScreenCallbacks out{};
        out.settermprop = &TerminalScreenCallbacks::settermprop;
        out.sb_pushline = &TerminalScreenCallbacks::pushline;
        out.sb_popline = &TerminalScreenCallbacks::popline;
        out.sb_clear = &TerminalScreenCallbacks::clear;
        return out;
    }();
    vterm_screen_set_callbacks(vt_->screen, &callbacks, this);
    vterm_screen_enable_altscreen(vt_->screen, 1);
    vterm_screen_set_damage_merge(vt_->screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_reset(vt_->screen, 1);
}

TerminalScreen::~TerminalScreen() {
    vterm_free(vt_->term);
    delete vt_;
}

void TerminalScreen::set_on_reply(std::function<void(std::string_view)> handler) { on_reply_ = std::move(handler); }

void TerminalScreen::write(std::string_view bytes) {
    vterm_input_write(vt_->term, bytes.data(), bytes.size());
    vterm_screen_flush_damage(vt_->screen);
}

void TerminalScreen::resize(int rows, int cols) {
    vterm_set_size(vt_->term, std::max(1, rows), std::max(1, cols));
    vterm_screen_flush_damage(vt_->screen);
}

int TerminalScreen::rows() const {
    int rows = 0;
    int cols = 0;
    vterm_get_size(vt_->term, &rows, &cols);
    return rows;
}

int TerminalScreen::cols() const {
    int rows = 0;
    int cols = 0;
    vterm_get_size(vt_->term, &rows, &cols);
    return cols;
}

int TerminalScreen::scrollback_rows() const { return static_cast<int>(vt_->history.size()); }

TerminalCell TerminalScreen::cell(int row, int col) const {
    if (col < 0) {
        return {};
    }
    if (row >= 0) {
        VTermScreenCell cell{};
        if (row >= rows() || col >= cols() || !vterm_screen_get_cell(vt_->screen, VTermPos{row, col}, &cell)) {
            return {};
        }
        return Convert(vt_->screen, cell);
    }
    const auto back = static_cast<std::size_t>(-row);
    if (back > vt_->history.size()) {
        return {};
    }
    const std::vector<VTermScreenCell>& line = vt_->history[vt_->history.size() - back];
    if (col >= static_cast<int>(line.size())) {
        return {};
    }
    return Convert(vt_->screen, line[static_cast<std::size_t>(col)]);
}

std::string TerminalScreen::row_text(int row) const {
    std::u32string text;
    int width = cols();
    if (row < 0) {
        const auto back = static_cast<std::size_t>(-row);
        width = back <= vt_->history.size() ? static_cast<int>(vt_->history[vt_->history.size() - back].size()) : 0;
    }
    bool wide_tail = false;
    for (int col = 0; col < width; ++col) {
        const TerminalCell here = cell(row, col);
        // A wide character's second cell has nothing of its own to show.
        if (!wide_tail) {
            text += here.chars.empty() ? std::u32string(U" ") : here.chars;
        }
        wide_tail = here.width == 2;
    }
    while (!text.empty() && text.back() == U' ') {
        text.pop_back();
    }
    return Utf8(text);
}

TerminalCursor TerminalScreen::cursor() const {
    VTermPos pos{};
    vterm_state_get_cursorpos(vt_->state, &pos);
    TerminalCursor out;
    out.row = pos.row;
    out.col = pos.col;
    out.visible = cursor_visible_;
    return out;
}

void TerminalScreen::key(TerminalKey key, TerminalMods mods) {
    vterm_keyboard_key(vt_->term, KeyFor(key), Modifiers(mods));
}

void TerminalScreen::character(char32_t c, TerminalMods mods) {
    vterm_keyboard_unichar(vt_->term, static_cast<std::uint32_t>(c), Modifiers(mods));
}

void TerminalScreen::paste(std::string_view text) {
    vterm_keyboard_start_paste(vt_->term);
    std::string body;
    body.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
        }
        body.push_back(text[i] == '\n' ? '\r' : text[i]);
    }
    if (on_reply_ && !body.empty()) {
        on_reply_(body);
    }
    vterm_keyboard_end_paste(vt_->term);
}

void TerminalScreen::set_palette(const std::array<TerminalColor, 16>& colors) {
    for (int index = 0; index < 16; ++index) {
        const TerminalColor& color = colors[static_cast<std::size_t>(index)];
        VTermColor rgb;
        vterm_color_rgb(&rgb, color.r, color.g, color.b);
        vterm_state_set_palette_color(vt_->state, index, &rgb);
    }
}

void TerminalScreen::focus(bool focused) {
    if (focused) {
        vterm_state_focus_in(vt_->state);
    } else {
        vterm_state_focus_out(vt_->state);
    }
}

}  // namespace ide
