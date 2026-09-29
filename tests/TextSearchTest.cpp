#include "ide/TextSearch.hpp"
#include "ide/Utf8.hpp"

#include <cstdio>
#include <string>
#include <vector>

// Finding and replacing text the way the find widget and the Search pane do.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

ide::SearchQuery Query(const char* pattern, bool match_case = false, bool whole_word = false, bool regex = false) {
    ide::SearchQuery query;
    query.pattern = pattern;
    query.match_case = match_case;
    query.whole_word = whole_word;
    query.regex = regex;
    return query;
}

// Each match as start-end in code points.
std::string Spans(const std::vector<ide::TextMatch>& matches) {
    std::string out;
    for (const ide::TextMatch& match : matches) {
        if (!out.empty()) {
            out += ' ';
        }
        out += std::to_string(match.start) + "-" + std::to_string(match.end);
    }
    return out;
}

void ExpectSpans(const ide::SearchQuery& query, const std::string& text, const std::string& want, const char* label) {
    const std::string got = Spans(ide::TextSearch(query).find_all(text));
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got [%s], want [%s]\n", label, got.c_str(), want.c_str());
        ++gFailures;
    }
}

void ExpectReplaced(const ide::SearchQuery& query, const std::string& text, const char* replacement,
                    const std::string& want, const char* label) {
    const std::string got = ide::TextSearch(query).replace_all(text, replacement);
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got [%s], want [%s]\n", label, got.c_str(), want.c_str());
        ++gFailures;
    }
}

}  // namespace

int RunTextSearchTests() {
    gFailures = 0;

    ExpectSpans(Query("part"), "Part part PART", "0-4 5-9 10-14", "plain text ignores case by default");
    ExpectSpans(Query("part", true), "Part part PART", "5-9", "Match Case keeps case");
    ExpectSpans(Query("aa"), "aaaa", "0-2 2-4", "matches do not overlap");
    ExpectSpans(Query(""), "anything", "", "an empty pattern finds nothing");
    ExpectSpans(Query("x"), "", "", "empty text has no matches");

    ExpectSpans(Query("part", false, true), "part parts _part part_ (part) part", "0-4 24-28 30-34",
                "Whole Word skips matches inside words");
    ExpectSpans(Query("foo(", false, true), "foo(x) afoo(", "0-4",
                "a match ending in a separator only needs a boundary before it");
    ExpectSpans(Query("é", false, true), "é aé", "0-1", "non-ASCII letters are part of words");

    // Code points, not bytes: é is two bytes.
    ExpectSpans(Query("b"), "\xc3\xa9" "b\n\xc3\xa9\xc3\xa9" "b", "1-2 5-6", "offsets are code points across lines");
    {
        const std::vector<ide::TextMatch> matches = ide::TextSearch(Query("b")).find_all("ab\ncb\n\nb");
        Expect(matches.size() == 3, "one match per line");
        if (matches.size() == 3) {
            Expect(matches[0].line == 0 && matches[0].column == 1, "first match line and column");
            Expect(matches[1].line == 1 && matches[1].column == 1 && matches[1].start == 4, "second match line and column");
            Expect(matches[2].line == 3 && matches[2].column == 0 && matches[2].start == 7, "a blank line still counts");
            Expect(matches[2].line_byte == 7 && matches[2].byte_start == 7, "byte offsets of the last line");
        }
    }
    Expect(ide::TextSearch(Query("a")).find_all("aaaa", 2).size() == 2, "the limit caps the matches");

    ExpectSpans(Query("p[a-z]+t", false, false, true), "Part pot pt", "0-4 5-8", "a regex matches");
    ExpectSpans(Query("P[a-z]+t", true, false, true), "Part pot", "0-4", "a regex keeps case with Match Case");
    ExpectSpans(Query("^local", false, false, true), "local a\n  local b\nlocal c", "0-5 18-23", "^ is the start of each line");
    ExpectSpans(Query("end$", false, false, true), "end end\nend", "4-7 8-11", "$ is the end of each line");
    ExpectSpans(Query("^", false, false, true), "a\nb", "0-0 2-2", "a regex can match nothing");
    ExpectSpans(Query("x*", false, false, true), "axb", "0-0 1-2 2-2 3-3", "empty matches step one code point");
    ExpectSpans(Query("\\bpart\\b", false, false, true), "part parts", "0-4", "\\b matches at word edges");
    ExpectSpans(Query("\\b", false, false, true), "ab cd.", "0-0 2-2 3-3 5-5", "\\b after a match sees the text before it");
    ExpectSpans(Query("par", false, true, true), "par part", "0-3", "Whole Word applies to a regex");
    ExpectSpans(Query("a\\nb", false, false, true), "a\nb", "", "a match stays inside one line");
    {
        const ide::TextSearch broken(Query("(", false, false, true));
        Expect(!broken.ready() && !broken.error().empty(), "a regex that does not compile says why");
        Expect(broken.find_all("((").empty(), "a broken regex finds nothing");
        const ide::TextSearch literal(Query("("));
        Expect(literal.ready() && literal.find_all("((").size() == 2, "plain text treats ( as a character");
    }

    ExpectReplaced(Query("part"), "Part and part", "Model", "Model and Model", "plain replace");
    ExpectReplaced(Query("$1"), "a $1 b", "x", "a x b", "plain text does not expand $");
    ExpectReplaced(Query("(\\w+) = (\\w+)", false, false, true), "a = b\nc = d", "$2 = $1", "b = a\nd = c",
                   "a regex replacement expands groups");
    ExpectReplaced(Query("x", false, false, true), "x", "[$&]", "[x]", "$& is the whole match");
    ExpectReplaced(Query("x", false, false, true), "x", "$$", "$", "$$ is a dollar sign");
    ExpectReplaced(Query(", ", false, false, true), "a, b", ",\\n", "a,\nb", "\\n in a regex replacement is a newline");
    ExpectReplaced(Query("^", false, false, true), "a\nb", "-- ", "-- a\n-- b", "an empty match inserts");
    ExpectReplaced(Query("aa"), "aaa", "b", "ba", "replace goes left to right");
    {
        const ide::TextSearch search(Query("x"));
        int count = 0;
        Expect(search.replace_all("x\nx x\nx", "y", 1, &count) == "x\ny y\nx" && count == 2,
               "replace can be limited to one line");
        Expect(search.replace_all("abc", "y", -1, &count) == "abc" && count == 0, "nothing to replace keeps the text");
    }
    {
        const std::string text = "a1 b2 c3";
        const ide::TextSearch search(Query("([a-z])(\\d)", false, false, true));
        const std::vector<ide::TextMatch> matches = search.find_all(text);
        Expect(matches.size() == 3, "three pairs");
        if (matches.size() == 3) {
            Expect(search.expand(text, matches[1], "$2$1") == "2b", "expand one match");
            const std::vector<ide::TextMatch> tail(matches.begin() + 1, matches.end());
            Expect(search.replace_span(text, tail, "$2$1") == "2b 3c", "a span runs from the first match to the last");
        }
    }
    Expect(ide::CodePoints("a\xc3\xa9\xe2\x82\xac") == 3, "code points count characters, not bytes");
    return gFailures;
}
