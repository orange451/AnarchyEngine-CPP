#include "ScriptPairs.hpp"

#include <vector>

namespace ide {
namespace {

bool IsSpace(char32_t code) {
    return code == U' ' || code == U'\t' || code == U'\n' || code == U'\r' || code == U'\v' || code == U'\f';
}

bool IsDigit(char32_t code) { return code >= U'0' && code <= U'9'; }

bool IsNameStart(char32_t code) {
    return (code >= U'A' && code <= U'Z') || (code >= U'a' && code <= U'z') || code == U'_' || code == U'@' || code >= 0x80;
}

bool IsNameContinue(char32_t code) { return IsNameStart(code) || IsDigit(code); }

std::u32string Utf32(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index++]);
        if (lead < 0x80) {
            out.push_back(lead);
            continue;
        }
        int need = 0;
        char32_t value = 0;
        if ((lead & 0xE0) == 0xC0 && lead >= 0xC2) {
            need = 1;
            value = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            need = 2;
            value = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0 && lead <= 0xF4) {
            need = 3;
            value = lead & 0x07;
        } else {
            out.push_back(0xFFFD);
            continue;
        }
        bool ok = true;
        for (int i = 0; i < need; ++i) {
            if (index >= text.size()) {
                ok = false;
                break;
            }
            const unsigned char next = static_cast<unsigned char>(text[index]);
            if ((next & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            value = (value << 6) | (next & 0x3F);
            ++index;
        }
        if (!ok || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            out.push_back(0xFFFD);
            continue;
        }
        out.push_back(value);
    }
    return out;
}

std::string Utf8(std::u32string_view text) {
    std::string out;
    for (char32_t code : text) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

const char* Keyword(std::string_view word) {
    static const char* kWords[] = {"and",  "break", "continue", "do",    "else", "elseif", "end",  "export", "false", "for",
                                   "function", "if", "in",       "local", "nil",  "not",    "or",   "repeat", "return", "then",
                                   "true", "type",  "until",     "while"};
    for (const char* candidate : kWords) {
        if (word == candidate) {
            return candidate;
        }
    }
    return nullptr;
}

// [=*[ or ]=*] at `index`. The count of '=' , or -1 when this is not a long bracket.
int LongSeparator(const std::u32string& text, int index) {
    if (index < 0 || index >= static_cast<int>(text.size())) {
        return -1;
    }
    const char32_t start = text[static_cast<std::size_t>(index)];
    if (start != U'[' && start != U']') {
        return -1;
    }
    int count = 0;
    int cursor = index + 1;
    while (cursor < static_cast<int>(text.size()) && text[static_cast<std::size_t>(cursor)] == U'=') {
        ++count;
        ++cursor;
    }
    if (cursor < static_cast<int>(text.size()) && text[static_cast<std::size_t>(cursor)] == start) {
        return count;
    }
    return -1;
}

enum class Kind {
    Name,
    Keyword,
    Number,
    String,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBrack,
    RBrack,
    Lt,
    Gt,
    Colon,
    Dot,
    Other
};

struct Tok {
    Kind kind = Kind::Other;
    std::string text;
    int begin = 0;
    int end = 0;
};

enum class RegionKind { LineComment, LongComment, ShortString, LongString, Interp };

struct Region {
    RegionKind kind = RegionKind::ShortString;
    int begin = 0;
    int end = 0;
    bool unclosed = false;
};

struct Frame {
    bool interp = false;
    int braces = 0;
    int piece = 0;
};

class Scan {
public:
    explicit Scan(std::u32string text) : text_(std::move(text)) {
        line_starts_.push_back(0);
        for (int index = 0; index < size(); ++index) {
            if (text_[static_cast<std::size_t>(index)] == U'\n') {
                line_starts_.push_back(index + 1);
            }
        }
        tokenize();
    }

    int size() const { return static_cast<int>(text_.size()); }
    int line_count() const { return static_cast<int>(line_starts_.size()); }

    int line_of(int pos) const {
        int line = 0;
        for (int index = 1; index < line_count(); ++index) {
            if (line_starts_[static_cast<std::size_t>(index)] > pos) {
                break;
            }
            line = index;
        }
        return line;
    }

    int line_end(int line) const {
        if (line + 1 < line_count()) {
            return line_starts_[static_cast<std::size_t>(line + 1)] - 1;
        }
        return size();
    }

    int line_start(int line) const { return line_starts_[static_cast<std::size_t>(line)]; }

    bool blank(int line) const {
        const int end = line_end(line);
        for (int index = line_start(line); index < end; ++index) {
            if (!IsSpace(text_[static_cast<std::size_t>(index)])) {
                return false;
            }
        }
        return true;
    }

    int indent_of(int line, int tab_size) const {
        const int end = line_end(line);
        int column = 0;
        for (int index = line_start(line); index < end; ++index) {
            const char32_t code = text_[static_cast<std::size_t>(index)];
            if (code == U' ') {
                ++column;
            } else if (code == U'\t') {
                column += tab_size;
            } else {
                break;
            }
        }
        return column;
    }

    std::u32string leading(int line) const {
        std::u32string out;
        const int end = line_end(line);
        for (int index = line_start(line); index < end; ++index) {
            const char32_t code = text_[static_cast<std::size_t>(index)];
            if (code != U' ' && code != U'\t') {
                break;
            }
            out.push_back(code);
        }
        return out;
    }

    int first_token(int line) const {
        const int begin = line_start(line);
        const int end = line_end(line);
        for (int index = 0; index < static_cast<int>(tokens_.size()); ++index) {
            if (tokens_[static_cast<std::size_t>(index)].begin >= end) {
                return -1;
            }
            if (tokens_[static_cast<std::size_t>(index)].begin >= begin) {
                return index;
            }
        }
        return -1;
    }

    int last_token(int line) const {
        const int begin = line_start(line);
        const int end = line_end(line);
        int last = -1;
        for (int index = 0; index < static_cast<int>(tokens_.size()); ++index) {
            if (tokens_[static_cast<std::size_t>(index)].begin >= end) {
                break;
            }
            if (tokens_[static_cast<std::size_t>(index)].begin >= begin) {
                last = index;
            }
        }
        return last;
    }

    bool logical_end(int caret, int line) const {
        const int end = line_end(line);
        if (caret < line_start(line) || caret > end) {
            return false;
        }
        for (const Tok& tok : tokens_) {
            if (tok.begin >= end) {
                break;
            }
            if (tok.end > caret && tok.begin < end) {
                return false;
            }
        }
        return true;
    }

    // A `--` comment on this line that begins at or after `from`. Long comments
    // are separate: they are not a trailing line comment.
    int line_comment_from(int line, int from) const {
        const int end = line_end(line);
        int found = -1;
        for (const Region& region : regions_) {
            if (region.kind != RegionKind::LineComment || region.begin < from || region.begin >= end) {
                continue;
            }
            if (found < 0 || region.begin < found) {
                found = region.begin;
            }
        }
        return found;
    }

    const Tok& token(int index) const { return tokens_[static_cast<std::size_t>(index)]; }
    int token_count() const { return static_cast<int>(tokens_.size()); }

    bool is_keyword(const Tok& tok, const char* word) const { return tok.kind == Kind::Keyword && tok.text == word; }

    struct Context {
        bool in_short = false;
        bool in_long_string = false;
        bool in_interp = false;
        bool in_line_comment = false;
        bool in_long_comment = false;

        bool in_string() const { return in_short || in_long_string || in_interp; }
        bool in_comment() const { return in_line_comment || in_long_comment; }
    };

    Context context(int caret) const {
        Context found;
        for (const Region& region : regions_) {
            if (!covers(region, caret)) {
                continue;
            }
            switch (region.kind) {
            case RegionKind::LineComment:
                found.in_line_comment = true;
                break;
            case RegionKind::LongComment:
                found.in_long_comment = true;
                break;
            case RegionKind::ShortString:
                found.in_short = true;
                break;
            case RegionKind::LongString:
                found.in_long_string = true;
                break;
            case RegionKind::Interp:
                found.in_interp = true;
                break;
            }
        }
        return found;
    }

    const std::u32string& text() const { return text_; }

private:
    bool covers(const Region& region, int caret) const {
        if (caret <= region.begin || caret > region.end) {
            return false;
        }
        if (caret < region.end) {
            return true;
        }
        return region.unclosed && region.end == size();
    }

    void add_region(RegionKind kind, int begin, int end, bool unclosed) {
        if (end < begin) {
            return;
        }
        regions_.push_back(Region{kind, begin, end, unclosed});
    }

    void emit(Kind kind, int begin, int end, std::string text = {}) {
        if (end <= begin) {
            return;
        }
        Tok tok;
        tok.kind = kind;
        tok.begin = begin;
        tok.end = end;
        tok.text = std::move(text);
        tokens_.push_back(std::move(tok));
    }

    void tokenize() {
        std::vector<Frame> frames(1);
        int index = 0;
        while (index < size()) {
            const char32_t code = text_[static_cast<std::size_t>(index)];
            if (IsSpace(code)) {
                ++index;
                continue;
            }
            if (frames.back().interp) {
                if (code == U'\\' && index + 1 < size()) {
                    index += 2;
                    continue;
                }
                if (code == U'`') {
                    emit(Kind::String, frames.back().piece, index + 1);
                    add_region(RegionKind::Interp, frames.back().piece, index + 1, false);
                    frames.pop_back();
                    ++index;
                    continue;
                }
                if (code == U'{') {
                    emit(Kind::String, frames.back().piece, index);
                    add_region(RegionKind::Interp, frames.back().piece, index, false);
                    ++index;
                    frames.push_back(Frame{});
                    continue;
                }
                ++index;
                continue;
            }
            if (code == U'-' && index + 1 < size() && text_[static_cast<std::size_t>(index + 1)] == U'-') {
                const int start = index;
                index += 2;
                if (index < size() && text_[static_cast<std::size_t>(index)] == U'[' && LongSeparator(text_, index) >= 0) {
                    const int sep = LongSeparator(text_, index);
                    index += sep + 2;
                    bool closed = false;
                    while (index < size()) {
                        if (text_[static_cast<std::size_t>(index)] == U']' && LongSeparator(text_, index) == sep) {
                            index += sep + 2;
                            closed = true;
                            break;
                        }
                        ++index;
                    }
                    add_region(RegionKind::LongComment, start, index, !closed);
                    continue;
                }
                while (index < size() && text_[static_cast<std::size_t>(index)] != U'\n' &&
                       text_[static_cast<std::size_t>(index)] != U'\r') {
                    ++index;
                }
                add_region(RegionKind::LineComment, start, index, index >= size());
                continue;
            }
            if (code == U'"' || code == U'\'') {
                const int start = index;
                const char32_t quote = code;
                ++index;
                bool closed = false;
                while (index < size()) {
                    if (text_[static_cast<std::size_t>(index)] == U'\\' && index + 1 < size()) {
                        if (text_[static_cast<std::size_t>(index + 1)] == U'z') {
                            index += 2;
                            while (index < size() && IsSpace(text_[static_cast<std::size_t>(index)])) {
                                ++index;
                            }
                            continue;
                        }
                        index += 2;
                        continue;
                    }
                    if (text_[static_cast<std::size_t>(index)] == quote) {
                        ++index;
                        closed = true;
                        break;
                    }
                    if (text_[static_cast<std::size_t>(index)] == U'\n' || text_[static_cast<std::size_t>(index)] == U'\r') {
                        break;
                    }
                    ++index;
                }
                emit(Kind::String, start, index);
                add_region(RegionKind::ShortString, start, index, !closed);
                continue;
            }
            if (code == U'`') {
                frames.push_back(Frame{true, 0, index});
                ++index;
                continue;
            }
            if (code == U'[' && LongSeparator(text_, index) >= 0) {
                const int start = index;
                const int sep = LongSeparator(text_, index);
                index += sep + 2;
                bool closed = false;
                while (index < size()) {
                    if (text_[static_cast<std::size_t>(index)] == U']' && LongSeparator(text_, index) == sep) {
                        index += sep + 2;
                        closed = true;
                        break;
                    }
                    ++index;
                }
                emit(Kind::String, start, index);
                add_region(RegionKind::LongString, start, index, !closed);
                continue;
            }
            if (code == U'{') {
                emit(Kind::LBrace, index, index + 1);
                ++frames.back().braces;
                ++index;
                continue;
            }
            if (code == U'}') {
                if (frames.back().braces > 0) {
                    emit(Kind::RBrace, index, index + 1);
                    --frames.back().braces;
                    ++index;
                    continue;
                }
                if (frames.size() >= 2 && frames[frames.size() - 2].interp) {
                    frames.pop_back();
                    ++index;
                    frames.back().piece = index;
                    continue;
                }
                emit(Kind::RBrace, index, index + 1);
                ++index;
                continue;
            }
            if (IsDigit(code) || (code == U'.' && index + 1 < size() && IsDigit(text_[static_cast<std::size_t>(index + 1)]))) {
                const int start = index;
                if (code == U'0' && index + 1 < size() &&
                    (text_[static_cast<std::size_t>(index + 1)] == U'x' || text_[static_cast<std::size_t>(index + 1)] == U'X' ||
                     text_[static_cast<std::size_t>(index + 1)] == U'b' || text_[static_cast<std::size_t>(index + 1)] == U'B')) {
                    index += 2;
                    while (index < size()) {
                        const char32_t unit = text_[static_cast<std::size_t>(index)];
                        const bool hex = (unit >= U'0' && unit <= U'9') || (unit >= U'a' && unit <= U'f') ||
                                         (unit >= U'A' && unit <= U'F') || unit == U'_';
                        if (!hex) {
                            break;
                        }
                        ++index;
                    }
                } else {
                    while (index < size()) {
                        const char32_t unit = text_[static_cast<std::size_t>(index)];
                        if (IsDigit(unit) || unit == U'_') {
                            ++index;
                            continue;
                        }
                        if (unit == U'.' && index + 1 < size() && IsDigit(text_[static_cast<std::size_t>(index + 1)])) {
                            ++index;
                            continue;
                        }
                        if ((unit == U'e' || unit == U'E') && index + 1 < size()) {
                            int next = index + 1;
                            if (text_[static_cast<std::size_t>(next)] == U'+' || text_[static_cast<std::size_t>(next)] == U'-') {
                                ++next;
                            }
                            if (next < size() && IsDigit(text_[static_cast<std::size_t>(next)])) {
                                index = next;
                                continue;
                            }
                        }
                        break;
                    }
                }
                emit(Kind::Number, start, index);
                continue;
            }
            if (IsNameStart(code)) {
                const int start = index;
                ++index;
                while (index < size() && IsNameContinue(text_[static_cast<std::size_t>(index)])) {
                    ++index;
                }
                const std::string word = Utf8(std::u32string_view(text_.data() + start, static_cast<std::size_t>(index - start)));
                const char* keyword = Keyword(word);
                emit(keyword != nullptr ? Kind::Keyword : Kind::Name, start, index, keyword != nullptr ? keyword : word);
                continue;
            }
            const int start = index;
            ++index;
            if (code == U'.' && index < size() && text_[static_cast<std::size_t>(index)] == U'.') {
                ++index;
                if (index < size() && text_[static_cast<std::size_t>(index)] == U'.') {
                    ++index;
                }
                emit(Kind::Other, start, index);
                continue;
            }
            if (code == U'=' && index < size() && text_[static_cast<std::size_t>(index)] == U'=') {
                emit(Kind::Other, start, ++index);
                continue;
            }
            if ((code == U'~' || code == U'<' || code == U'>') && index < size() && text_[static_cast<std::size_t>(index)] == U'=') {
                emit(Kind::Other, start, ++index);
                continue;
            }
            Kind kind = Kind::Other;
            if (code == U'(') {
                kind = Kind::LParen;
            } else if (code == U')') {
                kind = Kind::RParen;
            } else if (code == U'[') {
                kind = Kind::LBrack;
            } else if (code == U']') {
                kind = Kind::RBrack;
            } else if (code == U'<') {
                kind = Kind::Lt;
            } else if (code == U'>') {
                kind = Kind::Gt;
            } else if (code == U':') {
                kind = Kind::Colon;
            } else if (code == U'.') {
                kind = Kind::Dot;
            }
            emit(kind, start, index);
        }
        if (frames.back().interp) {
            emit(Kind::String, frames.back().piece, size());
            add_region(RegionKind::Interp, frames.back().piece, size(), true);
        }
    }

    std::u32string text_;
    std::vector<int> line_starts_;
    std::vector<Tok> tokens_;
    std::vector<Region> regions_;
};

bool Escaped(const std::u32string& text, int caret) {
    int slashes = 0;
    for (int index = caret - 1; index >= 0 && text[static_cast<std::size_t>(index)] == U'\\'; --index) {
        ++slashes;
    }
    return slashes % 2 == 1;
}

int MatchParen(const Scan& scan, int rparen) {
    int depth = 1;
    for (int index = rparen - 1; index >= 0; --index) {
        const Kind kind = scan.token(index).kind;
        if (kind == Kind::RParen) {
            ++depth;
        } else if (kind == Kind::LParen) {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return -1;
}

// The `(` at `lparen` opens a function parameter list. Returns the function keyword.
int FunctionBefore(const Scan& scan, int lparen, bool& anonymous) {
    int index = lparen - 1;
    if (index >= 0 && scan.token(index).kind == Kind::Gt) {
        int depth = 1;
        --index;
        while (index >= 0 && depth > 0) {
            const Kind kind = scan.token(index).kind;
            if (kind == Kind::Gt) {
                ++depth;
            } else if (kind == Kind::Lt) {
                --depth;
            }
            --index;
        }
    }
    if (index >= 0 && scan.token(index).kind == Kind::Name) {
        while (index >= 2 &&
               (scan.token(index - 1).kind == Kind::Dot || scan.token(index - 1).kind == Kind::Colon) &&
               scan.token(index - 2).kind == Kind::Name) {
            index -= 2;
        }
        if (index >= 1 && scan.is_keyword(scan.token(index - 1), "function")) {
            anonymous = false;
            return index - 1;
        }
        return -1;
    }
    if (index >= 0 && scan.is_keyword(scan.token(index), "function")) {
        anonymous = true;
        return index;
    }
    return -1;
}

bool TypeToken(const Scan& scan, const Tok& tok) {
    if (tok.kind != Kind::Keyword) {
        return true;
    }
    return scan.is_keyword(tok, "true") || scan.is_keyword(tok, "false") || scan.is_keyword(tok, "nil");
}

// Tokens [first, last] are a Luau type. `>` in `->` does not close a generic.
bool ParseType(const Scan& scan, int first, int last) {
    if (first > last) {
        return false;
    }
    int paren = 0;
    int brace = 0;
    int bracket = 0;
    int angle = 0;
    for (int index = first; index <= last; ++index) {
        const Tok& tok = scan.token(index);
        if (!TypeToken(scan, tok)) {
            return false;
        }
        switch (tok.kind) {
        case Kind::LParen:
            ++paren;
            break;
        case Kind::RParen:
            if (--paren < 0) {
                return false;
            }
            break;
        case Kind::LBrace:
            ++brace;
            break;
        case Kind::RBrace:
            if (--brace < 0) {
                return false;
            }
            break;
        case Kind::LBrack:
            ++bracket;
            break;
        case Kind::RBrack:
            if (--bracket < 0) {
                return false;
            }
            break;
        case Kind::Lt:
            ++angle;
            break;
        case Kind::Gt:
            if (angle > 0) {
                --angle;
            }
            break;
        case Kind::Name:
        case Kind::Keyword:
        case Kind::Number:
        case Kind::String:
        case Kind::Colon:
        case Kind::Dot:
        case Kind::Other:
            break;
        }
    }
    return paren == 0 && brace == 0 && bracket == 0 && angle == 0;
}

// Longest return type that starts at `colon`. A ')' after that type is not part of it.
// -1 when this is not a colon or no prefix parses.
int ReturnTypeLast(const Scan& scan, int colon, int limit) {
    if (colon > limit || scan.token(colon).kind != Kind::Colon) {
        return -1;
    }
    int best = -1;
    for (int last = colon + 1; last <= limit; ++last) {
        if (ParseType(scan, colon + 1, last)) {
            best = last;
        }
    }
    return best;
}

int ParenDepth(const Scan& scan, int stop) {
    int depth = 0;
    for (int index = 0; index < stop; ++index) {
        const Kind kind = scan.token(index).kind;
        if (kind == Kind::LParen) {
            ++depth;
        } else if (kind == Kind::RParen) {
            --depth;
        }
    }
    return depth;
}

enum class Follow { Missing, Body, Closer, Paren, Statement };

// The line after a header. A deeper line is a body that already exists. An `end`
// at the same indent already closes this block. An `end` further left belongs to
// an outer block, so this header still needs its own.
Follow NextLine(const Scan& scan, int line, int indent, int tab_size) {
    for (int next = line + 1; next < scan.line_count(); ++next) {
        if (scan.blank(next)) {
            continue;
        }
        const int token = scan.first_token(next);
        if (token < 0) {
            if (scan.indent_of(next, tab_size) > indent) {
                return Follow::Body;
            }
            continue;
        }
        const int next_indent = scan.indent_of(next, tab_size);
        if (next_indent > indent) {
            return Follow::Body;
        }
        const Tok& tok = scan.token(token);
        if (scan.is_keyword(tok, "else") || scan.is_keyword(tok, "elseif")) {
            return Follow::Closer;
        }
        if ((scan.is_keyword(tok, "end") || scan.is_keyword(tok, "until")) && next_indent == indent) {
            return Follow::Closer;
        }
        if (tok.kind == Kind::RParen && next_indent <= indent) {
            return Follow::Paren;
        }
        return Follow::Statement;
    }
    return Follow::Missing;
}

struct Header {
    bool found = false;
    bool anonymous = false;
    int function_index = -1;
    int signature_end = 0;
    int trailing_parens = 0;
};

// A function header on `line` whose signature ends at or before `caret`.
// Connect(function(dt)|) counts: the call's ')' stays after end.
Header FunctionHeader(const Scan& scan, int line, int caret) {
    Header header;
    const int last = scan.last_token(line);
    if (last < 0) {
        return header;
    }
    const int begin = scan.line_start(line);
    for (int index = 0; index <= last; ++index) {
        if (scan.token(index).kind != Kind::RParen || scan.token(index).begin < begin) {
            continue;
        }
        const int lparen = MatchParen(scan, index);
        if (lparen < 0) {
            continue;
        }
        bool anonymous = false;
        const int function = FunctionBefore(scan, lparen, anonymous);
        if (function < 0) {
            continue;
        }
        int cursor = index + 1;
        int signature_end = scan.token(index).end;
        if (cursor <= last && scan.token(cursor).kind == Kind::Colon) {
            const int type_last = ReturnTypeLast(scan, cursor, last);
            if (type_last < 0) {
                continue;
            }
            signature_end = scan.token(type_last).end;
            cursor = type_last + 1;
        }
        int trailing = 0;
        bool only_closers = true;
        for (int extra = cursor; extra <= last; ++extra) {
            if (scan.token(extra).kind != Kind::RParen) {
                only_closers = false;
                break;
            }
            ++trailing;
        }
        if (!only_closers || caret < signature_end) {
            continue;
        }
        if (header.found && signature_end < header.signature_end) {
            continue;
        }
        header.found = true;
        header.anonymous = anonymous;
        header.function_index = function;
        header.signature_end = signature_end;
        header.trailing_parens = trailing;
    }
    return header;
}

// One level deeper than the header line. Spaces follow the editor tab size.
std::u32string BodyLeading(const Scan& scan, int line, int tab_size, bool spaces) {
    std::u32string body = scan.leading(line);
    if (spaces) {
        body.append(static_cast<std::size_t>(tab_size), U' ');
    } else {
        body.push_back(U'\t');
    }
    return body;
}

// Closers already on this line stay after the break. Otherwise the break is the
// end of the line, so a trailing comment stays on the header.
int BreakAt(const Scan& scan, int line, const Header& header) {
    return header.trailing_parens > 0 ? header.signature_end : scan.line_end(line);
}

}  // namespace

char32_t source_code_point(std::string_view source, int index) {
    if (index < 0) {
        return 0;
    }
    const std::u32string text = Utf32(source);
    if (index >= static_cast<int>(text.size())) {
        return 0;
    }
    return text[static_cast<std::size_t>(index)];
}

PairResult pair_luau(std::string_view source, int begin, int end, char32_t typed) {
    PairResult result;
    if (typed != U'"' && typed != U'\'' && typed != U'(' && typed != U')') {
        return result;
    }
    const std::u32string text = Utf32(source);
    const int size = static_cast<int>(text.size());
    if (begin < 0 || end < begin || end > size) {
        return result;
    }
    const Scan scan(text);
    const Scan::Context where = scan.context(begin);
    const bool collapsed = begin == end;
    const char32_t next = begin < size ? text[static_cast<std::size_t>(begin)] : 0;
    const bool escaped = (where.in_short || where.in_interp) && Escaped(text, begin);
    if (collapsed && !escaped && next != 0 && next == typed && (typed == U'"' || typed == U'\'' || typed == U')')) {
        result.action = PairAction::Skip;
        return result;
    }
    if (typed == U')' || where.in_string() || where.in_comment()) {
        return result;
    }
    result.open = static_cast<char>(typed);
    result.close = typed == U'(' ? ')' : result.open;
    result.action = collapsed ? PairAction::Insert : PairAction::Wrap;
    return result;
}

EnterResult enter_luau(std::string_view source, int caret, int tab_size, bool spaces, bool flat) {
    EnterResult result;
    const std::u32string text = Utf32(source);
    const int size = static_cast<int>(text.size());
    if (caret < 0 || caret > size) {
        return result;
    }
    if (tab_size < 1) {
        tab_size = 1;
    }
    const Scan scan(text);
    const Scan::Context where = scan.context(caret);
    if (where.in_string() || where.in_long_comment) {
        return result;
    }
    const int line = scan.line_of(caret);
    const Header header = FunctionHeader(scan, line, caret);
    if (!header.found && !scan.logical_end(caret, line)) {
        return result;
    }

    const int last = scan.last_token(line);
    const bool do_block = !header.found && last >= 0 && scan.is_keyword(scan.token(last), "do");
    const bool conditional = !header.found && last >= 0 && scan.is_keyword(scan.token(last), "then");
    if (!header.found && !do_block && !conditional) {
        return result;
    }

    const int indent = scan.indent_of(line, tab_size);
    const Follow follow = NextLine(scan, line, indent, tab_size);
    // Already inside the block. Indent the new line and leave the body or closer.
    // The command line stays one line, so Enter there still runs it.
    if (follow == Follow::Body || follow == Follow::Closer) {
        if (flat) {
            return result;
        }
        const std::u32string body = BodyLeading(scan, line, tab_size, spaces);
        std::u32string inserted(1, U'\n');
        inserted += body;
        // A call closer still on this line, as in Connect(function(dt)|), moves
        // down with the break so the body line stays empty.
        if (header.trailing_parens > 0) {
            inserted.push_back(U'\n');
        }
        const int at = BreakAt(scan, line, header);
        result.insert = true;
        result.begin = at;
        result.end = at;
        result.text = Utf8(inserted);
        result.caret = at + 1 + static_cast<int>(body.size());
        return result;
    }

    bool add_paren = false;
    if (header.found && header.anonymous && header.trailing_parens == 0 &&
        ParenDepth(scan, header.function_index) > 0 && follow != Follow::Paren) {
        add_paren = true;
    }

    if (flat) {
        int at = header.trailing_parens > 0 ? header.signature_end : scan.line_end(line);
        bool before_comment = false;
        if (header.trailing_parens == 0) {
            const int from = header.found ? header.signature_end : (last >= 0 ? scan.token(last).end : scan.line_start(line));
            const int comment = scan.line_comment_from(line, from);
            if (comment >= 0) {
                at = comment;
                before_comment = true;
            }
        }
        const std::u32string& chars = scan.text();
        const bool spaced_before = at > 0 && IsSpace(chars[static_cast<std::size_t>(at - 1)]);
        std::u32string inserted;
        if (!spaced_before) {
            inserted.push_back(U' ');
        }
        const int caret_at = at + static_cast<int>(inserted.size());
        inserted += std::u32string(U"end");
        if (add_paren) {
            inserted.push_back(U')');
        }
        if (before_comment && at < scan.size() && !IsSpace(chars[static_cast<std::size_t>(at)])) {
            inserted.push_back(U' ');
        }
        result.insert = true;
        result.begin = at;
        result.end = at;
        result.text = Utf8(inserted);
        result.caret = caret_at;
        return result;
    }

    const std::u32string base = scan.leading(line);
    const std::u32string body = BodyLeading(scan, line, tab_size, spaces);
    std::u32string inserted;
    inserted.push_back(U'\n');
    inserted += body;
    inserted.push_back(U'\n');
    inserted += base;
    inserted += std::u32string(U"end");
    if (add_paren) {
        inserted.push_back(U')');
    }

    const int at = BreakAt(scan, line, header);
    result.insert = true;
    result.begin = at;
    result.end = at;
    result.text = Utf8(inserted);
    result.caret = at + 1 + static_cast<int>(body.size());
    return result;
}

}  // namespace ide
