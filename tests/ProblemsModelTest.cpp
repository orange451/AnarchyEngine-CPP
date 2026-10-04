#include "ide/ProblemsModel.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++gFailures;
    }
}

using engine_core::Severity;

engine_core::Diagnostic diag(Severity severity, const char* code, const char* message, std::uint32_t line,
                             std::uint32_t character, std::uint32_t end_line, std::uint32_t end_character) {
    engine_core::Diagnostic out;
    out.severity = severity;
    out.code = code;
    out.message = message;
    out.range.start.line = line;
    out.range.start.character = character;
    out.range.end.line = end_line;
    out.range.end.character = end_character;
    return out;
}

ide::Problem problem(Severity severity, int line, const char* message, const char* code = "Type") {
    ide::Problem out;
    out.severity = severity;
    out.line = line;
    out.column = 0;
    out.column_end = 1;
    out.code = code;
    out.message = message;
    return out;
}

ide::ProblemSource source(std::uint32_t id, const char* name, const char* path, std::vector<ide::Problem> problems) {
    ide::ProblemSource out;
    out.id = id;
    out.name = name;
    out.class_name = "Script";
    out.path = path;
    out.problems = std::move(problems);
    return out;
}

std::vector<ide::ProblemSource> place() {
    return {
        source(1, "Shop", "ReplicatedStorage", {problem(Severity::Warning, 4, "Unknown global 'wiat'", "Lint/UnknownGlobal")}),
        source(2, "Mover", "Workspace", {problem(Severity::Warning, 9, "unused"), problem(Severity::Error, 30, "Type mismatch"),
                                          problem(Severity::Error, 3, "Expected 'end'", "Syntax")}),
        source(3, "Clean", "Workspace", {}),
        source(4, "Info", "Workspace.Deep", {problem(Severity::Information, 1, "Deprecated call")}),
        source(5, "Alpha", "Workspace", {problem(Severity::Error, 7, "Bad")}),
    };
}

}  // namespace

int RunProblemsModelTests() {
    gFailures = 0;

    // problems_from: 1-based lines, code-point columns, hints dropped.
    {
        const std::string text = "local a = 1\nlocal é = nope\n";
        const std::vector<ide::Problem> got = ide::problems_from(
            text, {diag(Severity::Warning, "Lint/UnknownGlobal", "Unknown global 'nope'", 1, 11, 1, 15),
                   diag(Severity::Hint, "Lint/LocalUnused", "unused", 0, 6, 0, 7)});
        expect(got.size() == 1, "a hint is dropped");
        expect(!got.empty() && got[0].line == 2, "lines are 1-based");
        expect(!got.empty() && got[0].column == 10 && got[0].column_end == 14,
               "columns count code points, so é is one column, not two bytes");
    }
    {
        const std::string text = "foo(\n  1,\n";
        const std::vector<ide::Problem> got =
            ide::problems_from(text, {diag(Severity::Error, "Syntax", "Expected ')'", 0, 3, 2, 0)});
        expect(got.size() == 1 && got[0].column == 3 && got[0].column_end == 4,
               "a range over several lines ends at the end of its first line");
    }
    {
        const std::vector<ide::Problem> got =
            ide::problems_from("x", {diag(Severity::Error, "Syntax", "Bad", 0, 5, 0, 2)});
        expect(got.size() == 1 && got[0].column_end >= got[0].column, "an end before the start is clamped");
        expect(got.size() == 1 && got[0].column <= 1, "a column past the line's end is clamped to it");
    }
    {
        // Many problems in one source, on lines out of order, CRLF breaks, and a
        // line past the end: each lands on its own line.
        const std::string text = "aa\r\nbbbb\r\ncccccc";
        const std::vector<ide::Problem> got = ide::problems_from(
            text, {diag(Severity::Error, "E", "third", 2, 0, 2, 99), diag(Severity::Error, "E", "first", 0, 0, 0, 99),
                   diag(Severity::Error, "E", "second", 1, 1, 1, 99), diag(Severity::Error, "E", "gone", 7, 0, 7, 1)});
        expect(got.size() == 4 && got[0].line == 3 && got[0].column_end == 6, "the last line has no break");
        expect(got.size() == 4 && got[1].line == 1 && got[1].column_end == 2, "a CRLF line ends before its CR");
        expect(got.size() == 4 && got[2].line == 2 && got[2].column == 1 && got[2].column_end == 4,
               "a middle line is found from the line starts");
        expect(got.size() == 4 && got[3].line == 8 && got[3].column == 0 && got[3].column_end == 0,
               "a line past the end is empty");
    }

    // build_problems: grouping and order.
    {
        const ide::ProblemList list = ide::build_problems(place(), {});
        expect(list.scripts.size() == 4, "a script with no problems has no row");
        expect(list.scripts.size() == 4 && list.scripts[0].name == "Alpha" && list.scripts[1].name == "Mover",
               "scripts with an error come first, by path then name");
        expect(list.scripts.size() == 4 && list.scripts[2].name == "Shop" && list.scripts[3].name == "Info",
               "then the rest by path: ReplicatedStorage before Workspace.Deep");
        const ide::ScriptProblems& mover = list.scripts[1];
        expect(mover.problems.size() == 3 && mover.problems[0].line == 3 && mover.problems[1].line == 30 &&
                   mover.problems[2].severity == Severity::Warning,
               "errors by line, then warnings");
        expect(mover.errors == 2 && mover.warnings == 1, "a script counts its own errors and warnings");
        expect(list.total.errors == 3 && list.total.warnings == 2 && list.total.info == 1 && list.total.scripts == 4,
               "totals count everything");
        expect(ide::problems_summary(list) == "3 errors, 2 warnings, 1 info in 4 scripts", "the summary counts all");
        expect(ide::problems_title(list) == "Problems (5)", "the title counts errors and warnings");
    }

    // Toggles and filter.
    {
        ide::ProblemFilter only_errors;
        only_errors.warnings = false;
        only_errors.info = false;
        const ide::ProblemList list = ide::build_problems(place(), only_errors);
        expect(list.scripts.size() == 2, "with warnings and info off, only scripts with errors show");
        expect(list.shown.errors == 3 && list.shown.warnings == 0, "shown counts follow the toggles");
        expect(list.matching.warnings == 2, "toggle labels still count what the filter matches");
        expect(ide::problems_title(list) == "Problems (5)", "the title ignores the toggles");
        expect(ide::problems_summary(list) == "3 errors in 2 scripts", "the summary counts what is shown");
    }
    {
        // Errors off: Mover shows only its warning, so it sorts with the rest by
        // path instead of first for errors no one can see.
        ide::ProblemFilter no_errors;
        no_errors.errors = false;
        const ide::ProblemList list = ide::build_problems(place(), no_errors);
        expect(list.scripts.size() == 3 && list.scripts[0].name == "Shop" && list.scripts[1].name == "Mover" &&
                   list.scripts[2].name == "Info",
               "a hidden error does not sort its script first");
        expect(list.scripts.size() == 3 && !list.scripts[1].has_error, "has_error counts shown errors only");
    }
    {
        ide::ProblemFilter text;
        text.text = "WIAT";
        const ide::ProblemList list = ide::build_problems(place(), text);
        expect(list.scripts.size() == 1 && list.scripts[0].name == "Shop", "the filter matches messages, ignoring case");
        text.text = "deep";
        expect(ide::build_problems(place(), text).scripts.size() == 1, "and paths");
        text.text = "alpha";
        expect(ide::build_problems(place(), text).scripts.size() == 1, "and script names");
        text.text = "syntax";
        const ide::ProblemList by_code = ide::build_problems(place(), text);
        expect(by_code.scripts.size() == 1 && by_code.scripts[0].problems.size() == 1, "and codes");
        text.text = "alpha";
        const ide::ProblemList by_name = ide::build_problems(place(), text);
        expect(by_name.scripts.size() == 1 && by_name.scripts[0].problems.size() == 1,
               "a name match shows all that script's problems");
        text.text = "zzz";
        const ide::ProblemList none = ide::build_problems(place(), text);
        expect(none.scripts.empty() && ide::problems_summary(none) == "No problems match", "nothing matches");
    }

    // Wording.
    {
        const ide::ProblemList empty = ide::build_problems({source(1, "A", "Workspace", {})}, {});
        expect(ide::problems_summary(empty) == "No problems", "no problems");
        expect(ide::problems_title(empty) == "Problems", "no count in the title");
        const ide::ProblemList one = ide::build_problems(
            {source(1, "A", "Workspace", {problem(Severity::Warning, 1, "w")})}, {});
        expect(ide::problems_summary(one) == "1 warning in 1 script", "singular wording");
        const ide::ProblemList info_only = ide::build_problems(
            {source(1, "A", "Workspace", {problem(Severity::Information, 1, "i")})}, {});
        expect(ide::problems_summary(info_only) == "1 info in 1 script", "info alone");
        expect(ide::problems_title(info_only) == "Problems", "info does not count in the title");
    }

    return gFailures;
}
