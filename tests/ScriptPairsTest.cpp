#include "ide/ScriptPairs.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

int gFailures = 0;

std::string Show(std::string_view text) {
    std::string out;
    for (char unit : text) {
        if (unit == '\n') {
            out += "\\n";
        } else if (unit == '\t') {
            out += "\\t";
        } else {
            out += unit;
        }
    }
    return out;
}

void Fail(const char* label) {
    std::fprintf(stderr, "FAIL %s\n", label);
    ++gFailures;
}

void Expect(bool condition, const char* label) {
    if (!condition) {
        Fail(label);
    }
}

const char* ActionName(ide::PairAction action) {
    switch (action) {
    case ide::PairAction::None:
        return "none";
    case ide::PairAction::Skip:
        return "skip";
    case ide::PairAction::Insert:
        return "insert";
    case ide::PairAction::Wrap:
        return "wrap";
    }
    return "?";
}

void ExpectPair(const std::string& source, int begin, int end, char32_t typed, ide::PairAction action, char open,
                char close, const char* label) {
    const ide::PairResult result = ide::pair_luau(source, begin, end, typed);
    if (result.action != action || (action != ide::PairAction::None && action != ide::PairAction::Skip &&
                                     (result.open != open || result.close != close))) {
        std::fprintf(stderr, "FAIL %s (got %s '%c%c')\n", label, ActionName(result.action), result.open, result.close);
        ++gFailures;
    }
}

int EndCaret(const std::string& text) {
    int line_start = 0;
    for (int index = 0; index <= static_cast<int>(text.size()); ++index) {
        if (index != static_cast<int>(text.size()) && text[static_cast<std::size_t>(index)] != '\n') {
            continue;
        }
        const std::string line = text.substr(static_cast<std::size_t>(line_start), static_cast<std::size_t>(index - line_start));
        const std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos) {
            const std::string trimmed = line.substr(first);
            if (trimmed.size() >= 3 && trimmed.compare(0, 3, "end") == 0) {
                std::size_t index = 3;
                while (index < trimmed.size() && trimmed[index] == ')') {
                    ++index;
                }
                if (index == trimmed.size() || trimmed[index] == ' ') {
                    return line_start - 1;
                }
            }
        }
        line_start = index + 1;
    }
    return -1;
}

void ExpectEnter(const std::string& source, int caret, const std::string& want, const char* label, int want_caret = -1) {
    if (caret < 0) {
        caret = static_cast<int>(source.size());
    }
    const ide::EnterResult result = ide::enter_luau(source, caret, 4, true);
    if (!result.insert) {
        std::fprintf(stderr, "FAIL %s (no insert)\n", label);
        ++gFailures;
        return;
    }
    std::string got = source;
    const int count = result.end - result.begin;
    if (result.begin < 0 || count < 0 || result.begin + count > static_cast<int>(got.size())) {
        std::fprintf(stderr, "FAIL %s (bad range %d..%d)\n", label, result.begin, result.end);
        ++gFailures;
        return;
    }
    got.replace(static_cast<std::size_t>(result.begin), static_cast<std::size_t>(count), result.text);
    const int caret_at = want_caret >= 0 ? want_caret : EndCaret(want);
    if (got != want || result.caret != caret_at) {
        std::fprintf(stderr, "FAIL %s\n got:  %s caret %d\n want: %s caret %d\n", label, Show(got).c_str(), result.caret,
                     Show(want).c_str(), caret_at);
        ++gFailures;
    }
}

void ExpectPlain(const std::string& source, int caret, const char* label) {
    if (caret < 0) {
        caret = static_cast<int>(source.size());
    }
    const ide::EnterResult result = ide::enter_luau(source, caret, 4, true);
    if (result.insert) {
        std::fprintf(stderr, "FAIL %s (inserted %s)\n", label, Show(result.text).c_str());
        ++gFailures;
    }
}

void ExpectFlat(const std::string& source, int caret, const std::string& want, const char* label) {
    if (caret < 0) {
        caret = static_cast<int>(source.size());
    }
    const ide::EnterResult result = ide::enter_luau(source, caret, 4, true, true);
    if (!result.insert) {
        std::fprintf(stderr, "FAIL %s (no insert)\n", label);
        ++gFailures;
        return;
    }
    std::string got = source;
    const int count = result.end - result.begin;
    if (result.begin < 0 || count < 0 || result.begin + count > static_cast<int>(got.size())) {
        std::fprintf(stderr, "FAIL %s (bad range %d..%d)\n", label, result.begin, result.end);
        ++gFailures;
        return;
    }
    got.replace(static_cast<std::size_t>(result.begin), static_cast<std::size_t>(count), result.text);
    const std::size_t end_at = want.rfind("end");
    if (got != want || end_at == std::string::npos || result.caret != static_cast<int>(end_at)) {
        std::fprintf(stderr, "FAIL %s\n got:  %s caret %d\n want: %s caret %d\n", label, Show(got).c_str(), result.caret,
                     Show(want).c_str(), end_at == std::string::npos ? -1 : static_cast<int>(end_at));
        ++gFailures;
    }
}

void ExpectFlatPlain(const std::string& source, int caret, const char* label) {
    if (caret < 0) {
        caret = static_cast<int>(source.size());
    }
    const ide::EnterResult result = ide::enter_luau(source, caret, 4, true, true);
    if (result.insert) {
        std::fprintf(stderr, "FAIL %s (inserted %s)\n", label, Show(result.text).c_str());
        ++gFailures;
    }
}

}  // namespace

int RunScriptPairsTests() {
    Expect(ide::source_code_point("ab", 1) == U'b', "code point");
    Expect(ide::source_code_point("\xC3\xA9x", 1) == U'x', "code point after é");
    Expect(ide::source_code_point("ab", 2) == 0, "code point past the end");

    ExpectPair("", 0, 0, U'"', ide::PairAction::Insert, '"', '"', "double quote opens a pair");
    ExpectPair("", 0, 0, U'\'', ide::PairAction::Insert, '\'', '\'', "quote opens a pair");
    ExpectPair("\"\"", 1, 1, U'"', ide::PairAction::Skip, 0, 0, "double quote steps over the closer");
    ExpectPair("''", 1, 1, U'\'', ide::PairAction::Skip, 0, 0, "quote steps over the closer");
    ExpectPair("'", 0, 0, U'"', ide::PairAction::Insert, '"', '"', "a different quote still opens");
    ExpectPair("'", 1, 1, U'"', ide::PairAction::None, 0, 0, "the other quote inside an open string is typed");
    ExpectPair("\"", 0, 0, U'"', ide::PairAction::Skip, 0, 0, "a quote in front of a quote steps over it");
    ExpectPair("", 0, 0, U'(', ide::PairAction::Insert, '(', ')', "parenthesis opens a pair");
    ExpectPair("()", 1, 1, U')', ide::PairAction::Skip, 0, 0, "close parenthesis steps over the closer");
    ExpectPair("", 0, 0, U')', ide::PairAction::None, 0, 0, "a bare close parenthesis is typed");
    ExpectPair("()", 0, 0, U'(', ide::PairAction::Insert, '(', ')', "an open parenthesis still opens");

    ExpectPair("\"hello\"", 3, 3, U'"', ide::PairAction::None, 0, 0, "a quote inside a string is typed");
    ExpectPair("\"hello\"", 6, 6, U'"', ide::PairAction::Skip, 0, 0, "the closing quote steps out of the string");
    ExpectPair("\"hello\"", 3, 3, U'(', ide::PairAction::None, 0, 0, "a parenthesis inside a string is typed");
    ExpectPair("\"foo\\\"\"", 5, 5, U'"', ide::PairAction::None, 0, 0, "an escaped quote is not a closer");
    ExpectPair("'hello'", 6, 6, U'\'', ide::PairAction::Skip, 0, 0, "a single quote steps out of its string");
    ExpectPair("[[hello]]", 4, 4, U'"', ide::PairAction::None, 0, 0, "a long string takes the quote");
    ExpectPair("-- hello", 4, 4, U'"', ide::PairAction::None, 0, 0, "a comment takes the quote");
    ExpectPair("--[[ hello", 6, 6, U'(', ide::PairAction::None, 0, 0, "a long comment takes the parenthesis");
    ExpectPair("-- \"", 3, 3, U'"', ide::PairAction::Skip, 0, 0, "the same quote in a comment still steps over");
    ExpectPair("\"hello", 6, 6, U'"', ide::PairAction::None, 0, 0, "an unclosed string takes one quote");
    ExpectPair("`hello`", 3, 3, U'"', ide::PairAction::None, 0, 0, "an interpolation takes the quote");
    ExpectPair("`a{x}b`", 3, 3, U'(', ide::PairAction::Insert, '(', ')', "a parenthesis inside an interpolation opens");

    ExpectPair("hello", 0, 5, U'"', ide::PairAction::Wrap, '"', '"', "a quote wraps the selection");
    ExpectPair("hello", 0, 5, U'(', ide::PairAction::Wrap, '(', ')', "a parenthesis wraps the selection");
    ExpectPair("hello", 0, 5, U')', ide::PairAction::None, 0, 0, "a close parenthesis does not wrap");
    ExpectPair("\"hello\"", 1, 6, U'"', ide::PairAction::None, 0, 0, "a selection inside a string is not wrapped");

    ExpectEnter("function foo()", -1, "function foo()\n    \nend", "a function gains end");
    ExpectEnter("local function foo()", -1, "local function foo()\n    \nend", "a local function gains end");
    ExpectEnter("function Foo:bar()", -1, "function Foo:bar()\n    \nend", "a method gains end");
    ExpectEnter("function Foo.bar()", -1, "function Foo.bar()\n    \nend", "a dotted function gains end");
    ExpectEnter("local f = function()", -1, "local f = function()\n    \nend", "an assigned function has no extra parenthesis");
    ExpectEnter("foo(function(dt)", -1, "foo(function(dt)\n    \nend)", "a callback gains end)");
    ExpectEnter("Heartbeat:Connect(function(dt)", -1, "Heartbeat:Connect(function(dt)\n    \nend)",
                "Connect gains end)");
    const std::string heartbeat = "game:GetService(\"RunService\").Heartbeat:Connect(function(dt))";
    const int before_closer =
        static_cast<int>(std::string("game:GetService(\"RunService\").Heartbeat:Connect(function(dt)").size());
    const std::string heartbeat_closed = "game:GetService(\"RunService\").Heartbeat:Connect(function(dt)\n    \nend)";
    ExpectEnter(heartbeat, before_closer, heartbeat_closed, "enter before the call closer closes the callback");
    ExpectEnter(heartbeat, -1, heartbeat_closed, "enter after the call closer closes the callback");
    ExpectEnter("foo(bar(function(dt)))", static_cast<int>(std::string("foo(bar(function(dt)").size()),
                "foo(bar(function(dt)\n    \nend))", "closers around a callback stay after end");
    ExpectEnter("Connect(function(dt): number)", static_cast<int>(std::string("Connect(function(dt): number").size()),
                "Connect(function(dt): number\n    \nend)", "a return type stays ahead of the call closer");
    ExpectEnter("foo(function(): (number) -> string)",
                static_cast<int>(std::string("foo(function(): (number) -> string").size()),
                "foo(function(): (number) -> string\n    \nend)", "an arrow return stays ahead of the call closer");
    ExpectEnter("Connect(function(dt)) -- later", static_cast<int>(std::string("Connect(function(dt)").size()),
                "Connect(function(dt)\n    \nend) -- later", "the call closer keeps its trailing comment");
    ExpectEnter("(function()", -1, "(function()\n    \nend)", "a parenthesized function gains end)");
    ExpectEnter("for i = 1, 10 do", -1, "for i = 1, 10 do\n    \nend", "a numeric for gains end");
    ExpectEnter("for k, v in pairs(t) do", -1, "for k, v in pairs(t) do\n    \nend", "a generic for gains end");
    ExpectEnter("while true do", -1, "while true do\n    \nend", "a while gains end");
    ExpectEnter("while (ready) do", -1, "while (ready) do\n    \nend", "a while condition does not gain a parenthesis");
    ExpectEnter("do", -1, "do\n    \nend", "a do block gains end");
    ExpectEnter("if x then", -1, "if x then\n    \nend", "a conditional gains end");
    ExpectEnter("elseif x then", -1, "elseif x then\n    \nend", "elseif gains end");
    ExpectEnter("function foo(): number", -1, "function foo(): number\n    \nend", "a return type still closes");
    ExpectEnter("function foo<T>(x: T): T", -1, "function foo<T>(x: T): T\n    \nend", "a generic function closes");
    ExpectEnter("function foo(a: (() -> number))", -1, "function foo(a: (() -> number))\n    \nend",
                "a function type in the parameter list still closes");
    ExpectEnter("function foo(): (number) -> string", -1, "function foo(): (number) -> string\n    \nend",
                "an arrow return type still closes");
    ExpectEnter("    if x then", -1, "    if x then\n        \n    end", "the header indent is kept");
    ExpectEnter("if x then -- later", -1, "if x then -- later\n    \nend", "a trailing comment stays on the conditional");
    ExpectEnter("do -- block", -1, "do -- block\n    \nend", "a trailing comment stays on do");
    ExpectEnter("function foo() -- hi", -1, "function foo() -- hi\n    \nend", "a trailing comment stays on the header");
    ExpectEnter("function foo() -- hi", static_cast<int>(std::string("function foo() -- ").size()),
                "function foo() -- hi\n    \nend", "enter in the trailing comment closes the function");
    ExpectEnter("function foo(\n    a: number\n)", -1, "function foo(\n    a: number\n)\n    \nend",
                "a finished multi-line signature closes");
    ExpectEnter("foo(\n    function()\n)", static_cast<int>(std::string("foo(\n    function()").size()),
                "foo(\n    function()\n        \n    end\n)", "the call's parenthesis is already on the next line");
    ExpectEnter("function foo()\nprint(1)", static_cast<int>(std::string("function foo()").size()),
                "function foo()\n    \nend\nprint(1)", "a following statement stays after end");
    ExpectEnter("function outer()\n    function inner()\nend",
                static_cast<int>(std::string("function outer()\n    function inner()").size()),
                "function outer()\n    function inner()\n        \n    end\nend",
                "an inner function does not take the outer end");

    const ide::EnterResult tabbed = ide::enter_luau("\tif x then", static_cast<int>(std::string("\tif x then").size()), 4, true);
    Expect(tabbed.insert && tabbed.text == "\n\t    \n\tend", "a tabbed header keeps its tab");

    const ide::EnterResult one_tab = ide::enter_luau("function foo()", 14, 4, false);
    Expect(one_tab.insert && one_tab.text == "\n\t\nend", "body indent can be a tab");

    ExpectPlain("function foo() end", -1, "a finished function does not gain another end");
    ExpectPlain("function foo() return 1", -1, "a body on the header line is left alone");
    ExpectPlain("else", -1, "else does not open a block");
    ExpectPlain("repeat", -1, "repeat does not gain end");
    ExpectPlain("until x", -1, "until does not gain end");
    ExpectPlain("-- function foo()", -1, "a comment is not a header");
    ExpectPlain("\"function foo()\"", -1, "a string is not a header");
    ExpectPlain("function foo()", 4, "enter in the middle of the header splits normally");
    const int function_end = static_cast<int>(std::string("function foo()").size());
    const int indented = static_cast<int>(std::string("\n    ").size());
    ExpectEnter("function foo()\nend", function_end, "function foo()\n    \nend", "an end at the same indent indents the body");
    ExpectEnter("function foo()\n    return 1\nend", function_end, "function foo()\n    \n    return 1\nend",
                "a function that already has a body indents", function_end + indented);
    ExpectEnter("if x then\nelse\nend", static_cast<int>(std::string("if x then").size()), "if x then\n    \nelse\nend",
                "enter before else indents the branch", static_cast<int>(std::string("if x then").size()) + indented);
    ExpectEnter("function foo()\n    -- note\nend", function_end, "function foo()\n    \n    -- note\nend",
                "enter above an indented note indents", function_end + indented);
    ExpectEnter("do\nend", static_cast<int>(std::string("do").size()), "do\n    \nend", "enter before end indents a do block");
    ExpectEnter("for i = 1, 10 do\nend", static_cast<int>(std::string("for i = 1, 10 do").size()),
                "for i = 1, 10 do\n    \nend", "enter before end indents a for block");
    ExpectEnter("while true do\nend", static_cast<int>(std::string("while true do").size()), "while true do\n    \nend",
                "enter before end indents a while block");
    ExpectEnter("    if x then\n    end", static_cast<int>(std::string("    if x then").size()),
                "    if x then\n        \n    end", "an indented header indents one level further");
    const std::string callback = "game:GetService(\"RunService\").Heartbeat:Connect(function(dt)\nend)";
    const int callback_at = static_cast<int>(callback.find('\n'));
    ExpectEnter(callback, callback_at, "game:GetService(\"RunService\").Heartbeat:Connect(function(dt)\n    \nend)",
                "enter inside a callback indents before end)");
    ExpectEnter("function foo()\n\nend", function_end, "function foo()\n    \n\nend",
                "a blank line before end still indents", function_end + indented);
    const ide::EnterResult callback_tab = ide::enter_luau(callback, callback_at, 4, false);
    Expect(callback_tab.insert && callback_tab.text == "\n\t" && callback_tab.caret == callback_at + 2,
           "enter inside a callback can indent with a tab");
    ExpectEnter("function outer()\n    function inner()\n    end\nend",
                static_cast<int>(std::string("function outer()\n    function inner()").size()),
                "function outer()\n    function inner()\n        \n    end\nend",
                "an inner function indents inside its own end");
    ExpectEnter("function foo() -- hi\nend", static_cast<int>(std::string("function foo() -- hi").size()),
                "function foo() -- hi\n    \nend", "a trailing comment stays on the closed header");
    ExpectEnter("Connect(function(dt))\nend", static_cast<int>(std::string("Connect(function(dt)").size()),
                "Connect(function(dt)\n    \n)\nend", "a call closer stays after the indented line",
                static_cast<int>(std::string("Connect(function(dt)").size()) + indented);
    ExpectPlain("function foo()\nend", 4, "enter in the middle of a closed function splits normally");
    ExpectPlain("function foo()\nend", static_cast<int>(std::string("function foo()\nend").size()),
                "enter on the end line stays at that indent");
    ExpectPlain("function foo()\n    return 1\nend", static_cast<int>(std::string("function foo()\n    return 1").size()),
                "enter on a body line keeps the editor indent");
    ExpectPlain("print(\n)", -1, "a closing parenthesis is not a function");
    ExpectPlain("function foo(", -1, "an unfinished parameter list stays open");
    ExpectPlain("Heartbeat:Connect(function(dt))", static_cast<int>(std::string("Heartbeat:Connect(function(dt").size()),
                "enter before the parameter closer does not close");
    ExpectPlain("Heartbeat:Connect(function(dt), other)",
                static_cast<int>(std::string("Heartbeat:Connect(function(dt)").size()),
                "a following argument is a normal newline");

    ExpectEnter("function foo()\n-- note", static_cast<int>(std::string("function foo()").size()),
                "function foo()\n    \nend\n-- note", "a comment below the header stays below end");

    ExpectFlat("function foo()", -1, "function foo() end", "a command-line function gains end");
    ExpectFlat("local function foo()", -1, "local function foo() end", "a command-line local function gains end");
    ExpectFlat("function Foo:bar()", -1, "function Foo:bar() end", "a command-line method gains end");
    ExpectFlat("local f = function()", -1, "local f = function() end", "a command-line assignment adds no parenthesis");
    ExpectFlat("foo(function(dt)", -1, "foo(function(dt) end)", "a command-line callback gains end)");
    ExpectFlat("Heartbeat:Connect(function(dt)", -1, "Heartbeat:Connect(function(dt) end)",
               "a command-line Connect gains end)");
    ExpectFlat(heartbeat, before_closer, "game:GetService(\"RunService\").Heartbeat:Connect(function(dt) end)",
               "a command-line enter before the call closer closes the callback");
    ExpectFlat(heartbeat, -1, "game:GetService(\"RunService\").Heartbeat:Connect(function(dt) end)",
               "a command-line enter after the call closer closes the callback");
    ExpectFlat("foo(bar(function(dt)))", static_cast<int>(std::string("foo(bar(function(dt)").size()),
               "foo(bar(function(dt) end))", "command-line closers stay after end");
    ExpectFlat("for i = 1, 10 do", -1, "for i = 1, 10 do end", "a command-line for gains end");
    ExpectFlat("while (ready) do", -1, "while (ready) do end", "a command-line while adds no parenthesis");
    ExpectFlat("do", -1, "do end", "a command-line do gains end");
    ExpectFlat("if x then", -1, "if x then end", "a command-line conditional gains end");
    ExpectFlat("elseif x then", -1, "elseif x then end", "a command-line elseif gains end");
    ExpectFlat("    if x then", -1, "    if x then end", "a command-line header keeps its indent");
    ExpectFlat("if x then -- later", -1, "if x then end -- later", "a command-line comment stays after end");
    ExpectFlat("do -- block", -1, "do end -- block", "a command-line do comment stays after end");
    ExpectFlat("function foo() -- hi", -1, "function foo() end -- hi", "a command-line header comment stays after end");
    ExpectFlat("function foo()--hi", -1, "function foo() end --hi", "a command-line comment gains a space");
    ExpectFlat("function foo() -- hi", static_cast<int>(std::string("function foo() -- ").size()),
               "function foo() end -- hi", "enter in a command-line comment still closes");
    ExpectFlat("if x then  ", -1, "if x then  end", "spaces already before the closer are kept");
    ExpectFlatPlain("function foo() end", static_cast<int>(std::string("function foo() ").size()),
                    "enter before the inserted end does not add another");

    ExpectFlatPlain("function foo()\nend", static_cast<int>(std::string("function foo()").size()),
                    "a command whose block is already closed still runs");
    ExpectFlatPlain("function foo() end", -1, "a finished command stays one line");
    ExpectFlatPlain("function foo() return 1", -1, "a command with a body is left alone");
    ExpectFlatPlain("-- function foo()", -1, "a command comment is not a header");
    ExpectFlatPlain("function foo(", -1, "an unfinished command stays open");
    ExpectFlatPlain("repeat", -1, "repeat does not close on the command line");

    return gFailures;
}
