#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ide {

// What a find field asks for, with VS Code's three toggles. Plain text matches as
// typed. A regex is ECMAScript, as std::regex reads it. Either way a match stays
// inside one line. Ignoring case folds ASCII letters only.
struct SearchQuery {
    std::string pattern;
    bool match_case = false;
    bool whole_word = false;
    bool regex = false;

    bool operator==(const SearchQuery& other) const {
        return pattern == other.pattern && match_case == other.match_case && whole_word == other.whole_word &&
               regex == other.regex;
    }
    bool operator!=(const SearchQuery& other) const { return !(*this == other); }
};

// One match. start and end are code points from the start of the text, the unit
// the text area counts in. A regex can match nothing, so end may equal start.
struct TextMatch {
    int start = 0;
    int end = 0;
    // The 0-based line, and the code point in it where the match starts.
    int line = 0;
    int column = 0;
    // The same match in bytes, and the byte its line starts at.
    std::size_t byte_start = 0;
    std::size_t byte_end = 0;
    std::size_t line_byte = 0;

    bool operator==(const TextMatch& other) const {
        return start == other.start && end == other.end && line == other.line && column == other.column;
    }
    bool operator!=(const TextMatch& other) const { return !(*this == other); }
};

// A compiled query. Whole word follows VS Code: the match may not continue a
// word at either end, where a word is letters, digits, _, and non-ASCII.
class TextSearch {
public:
    static constexpr std::size_t kNoLimit = std::numeric_limits<std::size_t>::max();

    explicit TextSearch(SearchQuery query);
    ~TextSearch();
    TextSearch(TextSearch&&) noexcept;
    TextSearch& operator=(TextSearch&&) noexcept;

    // False for an empty pattern or a regex that does not compile.
    bool ready() const;
    // Why the regex did not compile. Empty otherwise.
    const std::string& error() const { return error_; }
    const SearchQuery& query() const { return query_; }

    // Every match in text, in order, and at most limit of them. Matches do not overlap.
    std::vector<TextMatch> find_all(std::string_view text, std::size_t limit = kNoLimit) const;
    // What replacement puts in place of match, which find_all found in text. A
    // regex expands $1, $&, and $$, and \n, \t, and \\ in replacement.
    std::string expand(std::string_view text, const TextMatch& match, std::string_view replacement) const;
    // The text from the first of matches to the end of the last, each one replaced.
    // matches come from find_all on text, in order. Empty when there are none.
    std::string replace_span(std::string_view text, const std::vector<TextMatch>& matches,
                             std::string_view replacement) const;
    // text with every match replaced, or only those on line when it is 0 or more.
    // count, when given, gets how many were replaced.
    std::string replace_all(std::string_view text, std::string_view replacement, int line = -1,
                            int* count = nullptr) const;

private:
    struct Compiled;

    SearchQuery query_;
    std::string error_;
    // The pattern as matched: lower case when case is ignored.
    std::string needle_;
    std::unique_ptr<Compiled> regex_;
};

}  // namespace ide
