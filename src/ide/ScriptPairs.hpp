#pragma once

#include <string>
#include <string_view>

namespace ide {

// What typing one character does to the caret. Indexes are code points, the
// same unit CodeArea uses. None leaves the key to the normal editor.
enum class PairAction { None, Skip, Insert, Wrap };

struct PairResult {
    PairAction action = PairAction::None;
    char open = 0;
    char close = 0;
};

// Quotes and parentheses. Skip steps over a closer that is already there.
// Insert writes the pair and leaves the caret between the two characters.
// Wrap puts the pair around a selection. Strings and comments take the key as typed.
PairResult pair_luau(std::string_view source, int begin, int end, char32_t typed);

// Enter at the end of a function, do, for, while, or conditional header.
// `insert` is false when the line is not a header or the block already has a body.
// The replacement is inserted at [begin, end) and the caret lands on the new body line.
// A ')' already closing the call around the function, as in Connect(function(dt)|),
// stays after the inserted end.
struct EnterResult {
    bool insert = false;
    int begin = 0;
    int end = 0;
    std::string text;
    int caret = 0;
};

// `spaces` writes the extra body indent as `tab_size` spaces. Otherwise it is one tab.
// The header line keeps whatever indent it already uses.
EnterResult enter_luau(std::string_view source, int caret, int tab_size, bool spaces);

// Code point at `index`, or 0 when `index` is past the end.
char32_t source_code_point(std::string_view source, int index);

}  // namespace ide
