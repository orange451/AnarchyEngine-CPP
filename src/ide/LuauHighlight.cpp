#include "LuauHighlight.hpp"
#include "Utf8.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ide {
namespace {

bool IsNameStart(char32_t code) {
    return (code >= U'A' && code <= U'Z') || (code >= U'a' && code <= U'z') || code == U'_' || code == U'@';
}

bool IsNameContinue(char32_t code) {
    return IsNameStart(code) || (code >= U'0' && code <= U'9');
}

bool IsDigit(char32_t code) { return code >= U'0' && code <= U'9'; }

const char* Keyword(std::string_view word) {
    // continue, const, export, and type are contextual in Luau, but read as keywords.
    static const char* kWords[] = {"and",   "break", "const",  "continue", "do",    "else", "elseif", "end",
                                   "export", "false", "for",   "function", "if",    "in",   "local",  "nil",
                                   "not",   "or",    "repeat", "return",   "then",  "true", "type",   "until",
                                   "while"};
    for (const char* candidate : kWords) {
        if (word == candidate) {
            return "keyword";
        }
    }
    return nullptr;
}

const char* Builtin(std::string_view word) {
    // Names the play VM and the command line actually expose, plus the Luau
    // libraries that sandbox opens. `self` is the method receiver.
    static const char* kWords[] = {"assert",   "bit32",      "buffer",  "coroutine",   "error",
                                   "game",     "getmetatable", "ipairs", "math",        "next",      "pairs",
                                   "pcall",    "print",      "rawequal", "rawget",      "rawlen",    "rawset",
                                   "require",  "script",     "select",   "setmetatable", "shared",   "string",
                                   "table",    "task",       "tonumber", "tostring",    "typeof",    "unpack",
                                   "utf8",     "vector",     "warn",     "xpcall",      "_G",        "self"};
    for (const char* candidate : kWords) {
        if (word == candidate) {
            return "builtin";
        }
    }
    return nullptr;
}

// The engine's datatype globals: what Instance.new, Color3.new, Vector2.new, Vector3.new, and Enum.<Name> start from.
// A new datatype global goes here too; LuauHighlightTest checks every capitalized global is listed.
const char* Datatype(std::string_view word) {
    static const char* kWords[] = {"Color3", "Enum", "Instance", "Vector2", "Vector3"};
    for (const char* candidate : kWords) {
        if (word == candidate) {
            return "datatype";
        }
    }
    return nullptr;
}

class Scanner {
public:
    explicit Scanner(std::u32string text) : text_(std::move(text)) {}

    std::vector<LuauSpan> run() {
        while (index_ < text_.size()) {
            scanOne();
        }
        return spans_;
    }

private:
    char32_t at(std::size_t index) const { return index < text_.size() ? text_[index] : 0; }

    void add(const char* style, int length) {
        if (length <= 0) {
            return;
        }
        if (!spans_.empty() && spans_.back().style == style) {
            spans_.back().length += length;
            return;
        }
        spans_.push_back(LuauSpan{style, length});
    }

    // [===[ or ]===]. -1 when this is not a long bracket. Does not consume.
    int longSeparator(std::size_t index) const {
        const char32_t start = at(index);
        if (start != U'[' && start != U']') {
            return -1;
        }
        std::size_t cursor = index + 1;
        int count = 0;
        while (at(cursor) == U'=') {
            ++count;
            ++cursor;
        }
        if (at(cursor) != start) {
            return -1;
        }
        return count;
    }

    void scanOne() {
        const char32_t code = at(index_);
        if (code == U'-' && at(index_ + 1) == U'-') {
            scanComment();
            return;
        }
        if (code == U'"' || code == U'\'') {
            scanQuoted(code);
            return;
        }
        if (code == U'`') {
            scanInterpolated();
            return;
        }
        if (code == U'[') {
            const int sep = longSeparator(index_);
            if (sep >= 0) {
                scanLong(sep, "string");
                return;
            }
        }
        if (IsDigit(code) || (code == U'.' && IsDigit(at(index_ + 1)))) {
            scanNumber();
            return;
        }
        if (IsNameStart(code)) {
            scanName();
            return;
        }
        add(nullptr, 1);
        ++index_;
    }

    void scanComment() {
        const std::size_t start = index_;
        index_ += 2;
        if (at(index_) == U'[') {
            const int sep = longSeparator(index_);
            if (sep >= 0) {
                consumeLong(sep);
                add("comment", static_cast<int>(index_ - start));
                return;
            }
        }
        while (index_ < text_.size() && text_[index_] != U'\n' && text_[index_] != U'\r') {
            ++index_;
        }
        add("comment", static_cast<int>(index_ - start));
    }

    // index_ is on the opening bracket. sep is the number of '=' signs.
    void consumeLong(int sep) {
        index_ += static_cast<std::size_t>(sep) + 2;
        while (index_ < text_.size()) {
            if (text_[index_] == U']' && longSeparator(index_) == sep) {
                index_ += static_cast<std::size_t>(sep) + 2;
                break;
            }
            ++index_;
        }
    }

    void scanLong(int sep, const char* style) {
        const std::size_t start = index_;
        consumeLong(sep);
        add(style, static_cast<int>(index_ - start));
    }

    void scanQuoted(char32_t quote) {
        const std::size_t start = index_;
        ++index_;
        while (index_ < text_.size()) {
            const char32_t code = text_[index_];
            if (code == U'\n' || code == U'\r') {
                break;
            }
            ++index_;
            if (code == U'\\') {
                if (index_ < text_.size() && text_[index_] != U'\n' && text_[index_] != U'\r') {
                    ++index_;
                }
                continue;
            }
            if (code == quote) {
                break;
            }
        }
        add("string", static_cast<int>(index_ - start));
    }

    void scanInterpolated() {
        add("string", 1);
        ++index_;
        scanStringRest();
    }

    // The inside of a backtick string. index_ is on the first character after a
    // backtick or after an interpolation's closing brace.
    void scanStringRest() {
        const std::size_t start = index_;
        while (index_ < text_.size()) {
            const char32_t code = text_[index_];
            if (code == U'\n' || code == U'\r') {
                break;
            }
            if (code == U'\\') {
                ++index_;
                if (index_ < text_.size() && text_[index_] != U'\n' && text_[index_] != U'\r') {
                    ++index_;
                }
                continue;
            }
            if (code == U'`') {
                ++index_;
                break;
            }
            if (code == U'{') {
                add("string", static_cast<int>(index_ - start));
                add(nullptr, 1);
                ++index_;
                scanExpression();
                return;
            }
            ++index_;
        }
        add("string", static_cast<int>(index_ - start));
    }

    // The body of a `...{ expression }...` string. The opening brace is already
    // consumed. After the matching brace, the string continues.
    void scanExpression() {
        int depth = 1;
        while (index_ < text_.size() && depth > 0) {
            const char32_t code = text_[index_];
            if (code == U'{') {
                add(nullptr, 1);
                ++index_;
                ++depth;
                continue;
            }
            if (code == U'}') {
                add(nullptr, 1);
                ++index_;
                --depth;
                continue;
            }
            const std::size_t before = index_;
            scanOne();
            if (index_ == before) {
                add(nullptr, 1);
                ++index_;
            }
        }
        if (index_ < text_.size()) {
            scanStringRest();
        }
    }

    void scanNumber() {
        const std::size_t start = index_;
        if (text_[index_] == U'.') {
            ++index_;
        }
        while (index_ < text_.size() && (IsDigit(text_[index_]) || text_[index_] == U'.' || text_[index_] == U'_')) {
            ++index_;
        }
        if (index_ < text_.size() && (text_[index_] == U'e' || text_[index_] == U'E')) {
            ++index_;
            if (index_ < text_.size() && (text_[index_] == U'+' || text_[index_] == U'-')) {
                ++index_;
            }
        }
        while (index_ < text_.size() && (IsNameContinue(text_[index_]) || text_[index_] == U'_')) {
            ++index_;
        }
        add("number", static_cast<int>(index_ - start));
    }

    void scanName() {
        const std::size_t start = index_;
        ++index_;
        while (index_ < text_.size() && IsNameContinue(text_[index_]) && text_[index_] != U'@') {
            ++index_;
        }
        std::string word;
        word.reserve(index_ - start);
        for (std::size_t i = start; i < index_; ++i) {
            const char32_t code = text_[i];
            if (code < 0x80) {
                word.push_back(static_cast<char>(code));
            }
        }
        const char* style = Keyword(word);
        if (style == nullptr) {
            style = Builtin(word);
        }
        if (style == nullptr) {
            style = Datatype(word);
        }
        add(style, static_cast<int>(index_ - start));
    }

    std::u32string text_;
    std::size_t index_ = 0;
    std::vector<LuauSpan> spans_;
};

}  // namespace

std::vector<LuauSpan> highlight_luau(std::string_view source) {
    return Scanner(Utf32(source)).run();
}

}  // namespace ide
