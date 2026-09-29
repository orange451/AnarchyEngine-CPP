#include "LuauWords.hpp"
#include "ScriptPairs.hpp"
#include "Utf8.hpp"

#include <algorithm>
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

const char* Keyword(std::string_view word) {
    return LuauKeyword(word, LuauContextual::Continue | LuauContextual::Export | LuauContextual::Type);
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

// The line after a table opener. A deeper line is a field that already exists. A
// '}' at the same indent already closes this table. Anything else needs a '}'.
Follow NextBraceLine(const Scan& scan, int line, int indent, int tab_size) {
    for (int next = line + 1; next < scan.line_count(); ++next) {
        if (scan.blank(next)) {
            continue;
        }
        const int next_indent = scan.indent_of(next, tab_size);
        if (next_indent > indent) {
            return Follow::Body;
        }
        const int token = scan.first_token(next);
        if (token >= 0 && scan.token(token).kind == Kind::RBrace && next_indent == indent) {
            return Follow::Closer;
        }
        return Follow::Statement;
    }
    return Follow::Missing;
}

// Last token that ends at or before `caret`, or -1.
int TokenBefore(const Scan& scan, int caret) {
    int found = -1;
    for (int index = 0; index < scan.token_count(); ++index) {
        if (scan.token(index).end > caret) {
            break;
        }
        found = index;
    }
    return found;
}

// Only spaces or tabs in [from, to).
bool OnlyBlanks(const Scan& scan, int from, int to) {
    for (int index = from; index < to; ++index) {
        const char32_t code = scan.text()[static_cast<std::size_t>(index)];
        if (code != U' ' && code != U'\t') {
            return false;
        }
    }
    return true;
}

// Enter after a table's '{', or in a comment after it. `{|}` opens the pair onto three lines. A '{'
// that ends the line gains a '}' on its own line unless the table is already
// closed or filled below. A '(' just before the '{', as in foo({, closes after it.
EnterResult EnterBrace(const Scan& scan, int caret, int tab_size, bool spaces) {
    EnterResult result;
    const int before = TokenBefore(scan, caret);
    if (before < 0 || scan.token(before).kind != Kind::LBrace) {
        return result;
    }
    const Tok& lbrace = scan.token(before);
    const int line = scan.line_of(caret);
    if (lbrace.begin < scan.line_start(line)) {
        return result;
    }
    const std::u32string base = scan.leading(line);
    const std::u32string body = BodyLeading(scan, line, tab_size, spaces);

    const int after = before + 1;
    if (after < scan.token_count() && scan.token(after).kind == Kind::RBrace && OnlyBlanks(scan, lbrace.end, caret) &&
        scan.token(after).begin <= scan.line_end(line) && OnlyBlanks(scan, caret, scan.token(after).begin)) {
        std::u32string inserted(1, U'\n');
        inserted += body;
        inserted.push_back(U'\n');
        inserted += base;
        result.insert = true;
        result.begin = lbrace.end;
        result.end = scan.token(after).begin;
        result.text = Utf8(inserted);
        result.caret = lbrace.end + 1 + static_cast<int>(body.size());
        return result;
    }
    if (!scan.logical_end(caret, line)) {
        return result;
    }

    const int indent = scan.indent_of(line, tab_size);
    const Follow follow = NextBraceLine(scan, line, indent, tab_size);
    std::u32string inserted(1, U'\n');
    inserted += body;
    if (follow == Follow::Missing || follow == Follow::Statement) {
        inserted.push_back(U'\n');
        inserted += base;
        inserted.push_back(U'}');
        if (before > 0 && scan.token(before - 1).kind == Kind::LParen) {
            inserted.push_back(U')');
        }
    }
    const int at = scan.line_end(line);
    result.insert = true;
    result.begin = at;
    result.end = at;
    result.text = Utf8(inserted);
    result.caret = at + 1 + static_cast<int>(body.size());
    return result;
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
    const bool closer = typed == U')' || typed == U'}';
    if (typed != U'"' && typed != U'\'' && typed != U'(' && typed != U'{' && !closer) {
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
    if (collapsed && !escaped && next != 0 && next == typed && (typed == U'"' || typed == U'\'' || closer)) {
        result.action = PairAction::Skip;
        return result;
    }
    // A '{' in an interpolated string opens an expression, so it pairs there too.
    const bool opens_expression = typed == U'{' && collapsed && where.in_interp && !escaped && !where.in_comment();
    if (closer || ((where.in_string() || where.in_comment()) && !opens_expression)) {
        return result;
    }
    result.open = static_cast<char>(typed);
    result.close = typed == U'(' ? ')' : typed == U'{' ? '}' : result.open;
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
    if (!flat) {
        const EnterResult brace = EnterBrace(scan, caret, tab_size, spaces);
        if (brace.insert) {
            return brace;
        }
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

CommentResult comment_luau(std::string_view source, int anchor, int caret) {
    CommentResult result;
    const std::u32string text = Utf32(source);
    const int size = static_cast<int>(text.size());
    if (anchor < 0 || caret < 0 || anchor > size || caret > size) {
        return result;
    }
    const auto at = [&](int index) { return index < size ? text[static_cast<std::size_t>(index)] : char32_t{0}; };
    const int low = std::min(anchor, caret);
    int high = std::max(anchor, caret);
    if (high > low && at(high - 1) == U'\n') {
        --high;
    }

    struct Line {
        int indent_end = 0;
        int end = 0;
        bool blank = false;
    };
    int first = low;
    while (first > 0 && at(first - 1) != U'\n') {
        --first;
    }
    std::vector<Line> lines;
    for (int start = first;;) {
        Line line;
        line.indent_end = start;
        while (at(line.indent_end) == U' ' || at(line.indent_end) == U'\t') {
            ++line.indent_end;
        }
        line.end = line.indent_end;
        while (line.end < size && at(line.end) != U'\n') {
            ++line.end;
        }
        line.blank = line.indent_end == line.end || at(line.indent_end) == U'\r';
        lines.push_back(line);
        if (line.end >= high || line.end >= size) {
            break;
        }
        start = line.end + 1;
    }
    const int last = lines.back().end;

    bool any_text = false;
    bool all_commented = true;
    for (const Line& line : lines) {
        if (line.blank) {
            continue;
        }
        any_text = true;
        if (at(line.indent_end) != U'-' || at(line.indent_end + 1) != U'-') {
            all_commented = false;
        }
    }
    const bool uncomment = any_text && all_commented;

    // One edit per line: `removed` code points at `from` become `added`.
    struct Edit {
        int from = 0;
        int removed = 0;
        int added = 0;
    };
    std::vector<Edit> edits;
    std::u32string replaced;
    int copied = first;
    for (const Line& line : lines) {
        if (any_text && line.blank) {
            continue;
        }
        Edit edit;
        edit.from = line.indent_end;
        if (uncomment) {
            edit.removed = at(line.indent_end + 2) == U' ' ? 3 : 2;
        } else {
            edit.added = 3;
        }
        replaced.append(text, static_cast<std::size_t>(copied), static_cast<std::size_t>(edit.from - copied));
        if (edit.added > 0) {
            replaced += U"-- ";
        }
        copied = edit.from + edit.removed;
        edits.push_back(edit);
    }
    replaced.append(text, static_cast<std::size_t>(copied), static_cast<std::size_t>(last - copied));

    // The low end of a selection stays in front of an insert at its spot, so the
    // selection takes in the new `--`. The high end and a lone caret move past it.
    const auto move = [&](int position, bool high_end) {
        int shift = 0;
        for (const Edit& edit : edits) {
            if (edit.added > 0 && (position > edit.from || (high_end && position == edit.from))) {
                shift += edit.added;
            } else if (edit.removed > 0 && position > edit.from) {
                shift -= std::min(edit.removed, position - edit.from);
            }
        }
        return position + shift;
    };
    const bool collapsed = anchor == caret;
    result.change = true;
    result.begin = first;
    result.end = last;
    result.text = Utf8(replaced);
    result.anchor = move(anchor, collapsed || anchor > caret);
    result.caret = move(caret, collapsed || caret > anchor);
    return result;
}

}  // namespace ide
