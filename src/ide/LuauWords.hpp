#pragma once

// Luau's keywords, for the editor's highlighter, pairing, and completion.

#include <string_view>

namespace ide {

// Words Luau always reserves. None can name a variable.
inline constexpr const char* kLuauReserved[] = {"and",   "break", "do",     "else",   "elseif", "end",   "false",
                                                "for",   "function", "if", "in",     "local",  "nil",   "not",
                                                "or",    "repeat", "return", "then", "true",   "until", "while"};

// Words Luau reads as keywords only where a statement starts, and which can
// still name a variable. Each user of the lists above chooses which of these
// to treat as keywords.
enum class LuauContextual : unsigned {
    None = 0,
    Const = 1u << 0,
    Continue = 1u << 1,
    Export = 1u << 2,
    Type = 1u << 3,
    All = Const | Continue | Export | Type,
};

inline constexpr LuauContextual operator|(LuauContextual a, LuauContextual b) {
    return static_cast<LuauContextual>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

struct LuauContextualWord {
    const char* text;
    LuauContextual flag;
};

inline constexpr LuauContextualWord kLuauContextual[] = {
    {"const", LuauContextual::Const},
    {"continue", LuauContextual::Continue},
    {"export", LuauContextual::Export},
    {"type", LuauContextual::Type},
};

// The word's own text, which lives for the program, when `word` is reserved or
// one of the contextual words in `contextual`. Null otherwise.
inline const char* LuauKeyword(std::string_view word, LuauContextual contextual) {
    for (const char* candidate : kLuauReserved) {
        if (word == candidate) {
            return candidate;
        }
    }
    for (const LuauContextualWord& candidate : kLuauContextual) {
        if ((static_cast<unsigned>(contextual) & static_cast<unsigned>(candidate.flag)) != 0 &&
            word == candidate.text) {
            return candidate.text;
        }
    }
    return nullptr;
}

}  // namespace ide
