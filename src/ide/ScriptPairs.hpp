#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ide {

// What typing one character does to the caret. Indexes are code points, the
// same unit CodeArea uses. None leaves the key to the normal editor.
enum class PairAction { None, Skip, Insert, Wrap };

struct PairResult {
    PairAction action = PairAction::None;
    char open = 0;
    char close = 0;
};

// Quotes, parentheses, and braces. Skip steps over a closer that is already there.
// Insert writes the pair and leaves the caret between the two characters.
// Wrap puts the pair around a selection. Strings and comments take the key as typed,
// except a '{' in an interpolated string, which opens an expression and pairs.
PairResult pair_luau(std::string_view source, int begin, int end, char32_t typed);

// Ctrl+/ over the lines from `anchor` to `caret`. When every non-blank line already
// starts with `--` after its indent, those marks come off, with one space after them.
// Otherwise each non-blank line gains `-- ` after its own indent. A selection that
// ends at the start of a line leaves that line alone. The replacement covers
// [begin, end), and the anchor and caret move with the text around them.
struct CommentResult {
    bool change = false;
    int begin = 0;
    int end = 0;
    std::string text;
    int anchor = 0;
    int caret = 0;
};

CommentResult comment_luau(std::string_view source, int anchor, int caret);

// Enter at the end of a function, do, for, while, or conditional header.
// The replacement is inserted at [begin, end) and the caret lands on the new body line.
// A ')' already closing the call around the function, as in Connect(function(dt)|),
// stays after the inserted end.
// When that block already has a body, or an `end`, `else`, or `elseif` at the header's
// indent, Enter inserts only the indented line. An `end` on a following line stays.
// A ')' still on the header, as in Connect(function(dt)|), moves to the line after the body.
// Enter just after a table's '{' works the same way: a '{' that ends the line gains a
// '}' at the header's indent unless a deeper line or a '}' at that indent follows, and
// `{|}` opens onto three lines. `foo({` closes as `})`. The command line leaves braces alone.
// `insert` is false when the line is not one of those headers.
// When `flat` is set, the command stays on one line: a space, then `end`, then one
// ')' when the function is an anonymous callback. The caret sits just before `end`.
// A block that already has a body or closer does not insert, so Enter can run the line.
// A trailing `--` comment stays after that closer.
struct EnterResult {
    bool insert = false;
    int begin = 0;
    int end = 0;
    std::string text;
    int caret = 0;
};

// `spaces` writes the extra body indent as `tab_size` spaces. Otherwise it is one tab.
// The header line keeps whatever indent it already uses. `flat` is the one-line form.
EnterResult enter_luau(std::string_view source, int caret, int tab_size, bool spaces, bool flat = false);

// Code point at `index`, or 0 when `index` is past the end.
char32_t source_code_point(std::string_view source, int index);

std::vector<int> fold_ranges_luau(std::string_view source);

}  // namespace ide
