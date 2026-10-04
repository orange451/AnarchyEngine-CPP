# Problems Pane Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A dockable Problems pane that lists every error, warning, and info message the script checker has found, grouped by script, and opens any of them in one click.

**Architecture:** The pane is split in two:
- A pure `ProblemsModel` turns published diagnostics into sorted, filtered, counted rows and wording. It has no JadeFX and no Engine, so `engine-tests` covers it exhaustively.
- `IdeProblems`, an `IdePane` shaped like `IdeSearch`, gathers diagnostics from `Engine::analysis()` on the UI thread when `diagnostics_changed` sets a `ChangeFlag`, and draws the model as a `TreeView`.

`IdeLayout` registers it as the "Problems" window entry, beside the console.

**Tech Stack:** C++17 (Xcode 13 / libc++ 13), JadeFX (TreeView, Label, HBox, CSS), the studio's custom test harnesses (`engine-tests`, `studio-tests`; `expect`/`Run*Tests`, no Catch2).

**Spec:** `docs/superpowers/specs/2026-10-03-problems-pane-design.md`

## Global Constraints

- Severities shown: Error, Warning, Information. Hints are never shown or counted.
- Script order: scripts with an error first, then the rest; within each, by path then name. Within a script: errors, then warnings, then info, each by line then column.
- The filter is case-insensitive and matches the message, the script's name, its path, or the code. A script shows when any of its problems pass.
- Toggle counts are of what the filter passes. The tab title counts errors plus warnings in the place, unfiltered: "Problems (n)", or "Problems" when n is 0.
- Summary wording, exactly:
  - "3 errors, 5 warnings in 4 scripts"
  - "1 warning in 1 script"
  - "No problems"
  - "No problems match"
  - Info is counted only when there is some, as in "1 error, 2 info in 1 script".
- Play note text, exactly: "Checking resumes when the playtest stops."
- Analysis-off text, exactly: "Script analysis is off."
- Window entry name: "Problems". Tab icon: `Warning.png`. Home: `beside_console()`. Closed until opened.
- Scripts under `game.core()` (the studio's own tools) are left out, as the Search pane leaves them out.
- A script's path is its parent chain without the leading `game`, e.g. `Workspace.Logic`. A script directly under `game` has an empty path.
- Lines and columns given to `open` are 1-based lines and code-point columns, as `IdeScriptEditor::showRange` takes them. They are computed from the source the diagnostics were checked against (`ScriptAnalysis::analyzed_source`), never the current source.
- No C++20 library features. Code style matches `src/ide`: snake_case free functions, camelCase public pane methods as in IdeSearch, plain-sentence comments, no section banners.
- Glyphs: an error is a filled circle, a warning a filled square (the spec's triangle is not drawable in JadeFX CSS), info a ring.
- Every shipped theme (`resources/themes/*.css`) declares every new token itself (ThemeTest checks this). A value may reference that theme's own existing variables.

## Review Focus

- **A script renamed or moved while the pane is open:** its row must show the new name and path on the next rebuild. Pinned in Task 3 ("renaming a script renames its row").
- **A script destroyed while its problems are shown:** its row disappears and nothing crashes when an old row is clicked. Pinned in Task 2 ("a destroyed script's row goes").
- **Non-ASCII source** (UTF-8 before the problem on its line): the column given to `open` counts code points, not bytes. Pinned in Task 1 (`problems_from` with "é").
- **A diagnostic spanning several lines:** `column_end` is the end of the first line. It is never a column on another line, and never before `column`. Pinned in Task 1.
- **Hundreds of scripts with problems, rebuilt during a large batch:** at most one rebuild per layout pass, and the collapsed state survives it. Pinned in Task 2 ("many changes, one rebuild").

---

## File Structure

| File | Responsibility |
| --- | --- |
| `src/ide/ProblemsModel.hpp/.cpp` (new) | `Problem`, `ProblemSource`, `ProblemFilter`, `ProblemList`; `problems_from` (diagnostics to 1-based code-point problems, hints dropped); `build_problems` (sort, filter, count); `problems_summary`, `problems_title`. Pure. |
| `tests/ProblemsModelTest.cpp` (new) | `RunProblemsModelTests()` in `engine-tests`. |
| `src/ide/IdeProblems.hpp/.cpp` (new) | The pane: filter field, toggles, summary, play note, analysis-off notice, tree, open. |
| `src/ide/IdeLayout.hpp`, `src/ide/IdeLayout.cpp`, `src/ide/IdeLayoutEditing.cpp` | `make_problems()`, the window entry, public `show_problems()`. |
| `src/ide/IdeTheme.cpp`, `resources/themes/*.css` | The "Problems" colour tokens. |
| `tests/ProblemsTest.cpp` (new) | `RunProblemsTests(IdeLayout&, Scene&)` in `studio-tests`. |
| `CMakeLists.txt` | New sources and test files. |


Commands:
- Build: `cmake --build build --target engine-tests studio-tests -j8`
- Run: `./build/engine-tests`, `./build/studio-tests`. Both print `FAIL <label>` per failure and exit non-zero on any failure.

---

### Task 1: ProblemsModel

**Files:**
- Create: `src/ide/ProblemsModel.hpp`, `src/ide/ProblemsModel.cpp`, `tests/ProblemsModelTest.cpp`
- Modify: `CMakeLists.txt`: add `src/ide/ProblemsModel.cpp` next to `src/ide/ScriptMarks.cpp` in whichever target lists it, and `tests/ProblemsModelTest.cpp` to `engine-tests`.
- Modify: `tests/LuaEngineTest.cpp`: declare `int RunProblemsModelTests();` beside `int RunScriptMarksTests();` (line ~453), and add `failures += RunProblemsModelTests();` where `RunScriptMarksTests()` is called.

**Interfaces:**
- Produces (namespace `ide`):

```cpp
struct Problem {
    // 1-based line; code-point columns on that line, end exclusive and never before column.
    int line = 1;
    int column = 0;
    int column_end = 0;
    engine_core::Severity severity = engine_core::Severity::Error;
    std::string code;
    std::string message;
};
std::vector<Problem> problems_from(std::string_view source, const std::vector<engine_core::Diagnostic>& diagnostics);

struct ProblemSource {
    std::uint32_t id = 0;
    std::string name;
    std::string class_name;
    std::string path;
    std::vector<Problem> problems;
};
struct ProblemFilter {
    std::string text;
    bool errors = true;
    bool warnings = true;
    bool info = true;
};
struct ProblemCounts {
    int errors = 0;
    int warnings = 0;
    int info = 0;
    int scripts = 0;
};
struct ScriptProblems {
    std::uint32_t id = 0;
    std::string name;
    std::string class_name;
    std::string path;
    std::vector<Problem> problems;
    bool has_error = false;
    int errors = 0;
    int warnings = 0;
};
struct ProblemList {
    std::vector<ScriptProblems> scripts;
    // What the filter and toggles let through, and everything, before filtering.
    ProblemCounts shown;
    ProblemCounts total;
    // Counts per severity after the text filter, before the toggles, for the toggle labels.
    ProblemCounts matching;
};
ProblemList build_problems(std::vector<ProblemSource> sources, const ProblemFilter& filter);
std::string problems_summary(const ProblemList& list);
std::string problems_title(const ProblemList& list);
```

- [ ] **Step 1: Write the failing tests**

Create `tests/ProblemsModelTest.cpp`:

```cpp
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
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build --target engine-tests -j8`
Expected: compile error, `'ide/ProblemsModel.hpp' file not found`.

- [ ] **Step 3: Create `src/ide/ProblemsModel.hpp`**

```cpp
#pragma once

#include "ScriptAnalysis.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ide {

// One problem as the Problems pane lists it. The line is 1-based. Columns are
// code points on that line, as the script editor counts them, end exclusive.
struct Problem {
    int line = 1;
    int column = 0;
    int column_end = 0;
    engine_core::Severity severity = engine_core::Severity::Error;
    std::string code;
    std::string message;
};

// The problems in diagnostics, placed in source, the text they were checked
// against. Hints are left out: the editor dims them, and a list of them swamps
// the rest. A range over several lines ends at the end of its first line.
std::vector<Problem> problems_from(std::string_view source, const std::vector<engine_core::Diagnostic>& diagnostics);

// One script and its problems, as the pane gathers them.
struct ProblemSource {
    std::uint32_t id = 0;
    std::string name;
    std::string class_name;
    // Its parent chain without game, such as Workspace.Logic.
    std::string path;
    std::vector<Problem> problems;
};

struct ProblemFilter {
    // Matched, ignoring case, against the message, the script's name, its path, and the code.
    std::string text;
    bool errors = true;
    bool warnings = true;
    bool info = true;
};

struct ProblemCounts {
    int errors = 0;
    int warnings = 0;
    int info = 0;
    int scripts = 0;
};

struct ScriptProblems {
    std::uint32_t id = 0;
    std::string name;
    std::string class_name;
    std::string path;
    // Errors, then warnings, then info, each by line then column.
    std::vector<Problem> problems;
    bool has_error = false;
    int errors = 0;
    int warnings = 0;
};

struct ProblemList {
    // Scripts with an error first, then the rest; within each, by path then name.
    std::vector<ScriptProblems> scripts;
    // What the filter and toggles let through.
    ProblemCounts shown;
    // Everything, before filtering.
    ProblemCounts total;
    // What the text filter passes, before the toggles: the toggles' own counts.
    ProblemCounts matching;
};

ProblemList build_problems(std::vector<ProblemSource> sources, const ProblemFilter& filter);

// "3 errors, 5 warnings in 4 scripts", "No problems", or "No problems match".
std::string problems_summary(const ProblemList& list);

// "Problems (n)" with n the place's errors and warnings, or "Problems".
std::string problems_title(const ProblemList& list);

}  // namespace ide
```

- [ ] **Step 4: Create `src/ide/ProblemsModel.cpp`**

```cpp
#include "ProblemsModel.hpp"

#include "Strings.hpp"
#include "Utf8.hpp"

#include <algorithm>
#include <cctype>

namespace ide {
namespace {

int rank(engine_core::Severity severity) {
    switch (severity) {
    case engine_core::Severity::Error:
        return 0;
    case engine_core::Severity::Warning:
        return 1;
    case engine_core::Severity::Information:
        return 2;
    case engine_core::Severity::Hint:
        return 3;
    }
    return 3;
}

// The bytes of 0-based line `line` in source, without its line break.
std::string_view line_of(std::string_view source, std::uint32_t line) {
    std::size_t start = 0;
    for (std::uint32_t at = 0; at < line; ++at) {
        const std::size_t stop = source.find('\n', start);
        if (stop == std::string_view::npos) {
            return {};
        }
        start = stop + 1;
    }
    std::size_t stop = source.find('\n', start);
    if (stop == std::string_view::npos) {
        stop = source.size();
    }
    if (stop > start && source[stop - 1] == '\r') {
        --stop;
    }
    return source.substr(start, stop - start);
}

// Code points before byte column `byte` on the line, clamped to the line.
int column_of(std::string_view line, std::uint32_t byte) {
    return CodePointsBefore(line, std::min<std::size_t>(byte, line.size()));
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& unit : out) {
        unit = static_cast<char>(std::tolower(static_cast<unsigned char>(unit)));
    }
    return out;
}

bool contains(const std::string& haystack, const std::string& lowered_needle) {
    return lower(haystack).find(lowered_needle) != std::string::npos;
}

void count(ProblemCounts& counts, engine_core::Severity severity) {
    switch (severity) {
    case engine_core::Severity::Error:
        ++counts.errors;
        break;
    case engine_core::Severity::Warning:
        ++counts.warnings;
        break;
    case engine_core::Severity::Information:
        ++counts.info;
        break;
    case engine_core::Severity::Hint:
        break;
    }
}

bool shown_by(const ProblemFilter& filter, engine_core::Severity severity) {
    switch (severity) {
    case engine_core::Severity::Error:
        return filter.errors;
    case engine_core::Severity::Warning:
        return filter.warnings;
    case engine_core::Severity::Information:
        return filter.info;
    case engine_core::Severity::Hint:
        return false;
    }
    return false;
}

}  // namespace

std::vector<Problem> problems_from(std::string_view source, const std::vector<engine_core::Diagnostic>& diagnostics) {
    std::vector<Problem> out;
    for (const engine_core::Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == engine_core::Severity::Hint) {
            continue;
        }
        const std::string_view line = line_of(source, diagnostic.range.start.line);
        Problem problem;
        problem.line = static_cast<int>(diagnostic.range.start.line) + 1;
        problem.column = column_of(line, diagnostic.range.start.character);
        problem.column_end = diagnostic.range.end.line == diagnostic.range.start.line
                                 ? column_of(line, diagnostic.range.end.character)
                                 : CodePoints(line);
        problem.column_end = std::max(problem.column_end, problem.column);
        problem.severity = diagnostic.severity;
        problem.code = diagnostic.code;
        problem.message = diagnostic.message;
        out.push_back(std::move(problem));
    }
    return out;
}

ProblemList build_problems(std::vector<ProblemSource> sources, const ProblemFilter& filter) {
    ProblemList list;
    const std::string needle = lower(filter.text);
    for (ProblemSource& source : sources) {
        if (source.problems.empty()) {
            continue;
        }
        ++list.total.scripts;
        const bool script_matches =
            needle.empty() || contains(source.name, needle) || contains(source.path, needle);
        ScriptProblems row;
        row.id = source.id;
        row.name = std::move(source.name);
        row.class_name = std::move(source.class_name);
        row.path = std::move(source.path);
        bool matched_any = false;
        for (Problem& problem : source.problems) {
            count(list.total, problem.severity);
            if (problem.severity == engine_core::Severity::Error) {
                row.has_error = true;
            }
            const bool matches =
                script_matches || contains(problem.message, needle) || contains(problem.code, needle);
            if (!matches) {
                continue;
            }
            matched_any = true;
            count(list.matching, problem.severity);
            if (!shown_by(filter, problem.severity)) {
                continue;
            }
            count(list.shown, problem.severity);
            if (problem.severity == engine_core::Severity::Error) {
                ++row.errors;
            } else if (problem.severity == engine_core::Severity::Warning) {
                ++row.warnings;
            }
            row.problems.push_back(std::move(problem));
        }
        if (matched_any) {
            ++list.matching.scripts;
        }
        if (row.problems.empty()) {
            continue;
        }
        std::stable_sort(row.problems.begin(), row.problems.end(), [](const Problem& a, const Problem& b) {
            if (rank(a.severity) != rank(b.severity)) {
                return rank(a.severity) < rank(b.severity);
            }
            if (a.line != b.line) {
                return a.line < b.line;
            }
            return a.column < b.column;
        });
        ++list.shown.scripts;
        list.scripts.push_back(std::move(row));
    }
    std::stable_sort(list.scripts.begin(), list.scripts.end(), [](const ScriptProblems& a, const ScriptProblems& b) {
        if (a.has_error != b.has_error) {
            return a.has_error;
        }
        if (a.path != b.path) {
            return a.path < b.path;
        }
        return a.name < b.name;
    });
    return list;
}

std::string problems_summary(const ProblemList& list) {
    if (list.total.scripts == 0) {
        return "No problems";
    }
    if (list.shown.scripts == 0) {
        return "No problems match";
    }
    std::string text;
    const auto add = [&text](int n, const char* one, const char* many) {
        if (n == 0) {
            return;
        }
        if (!text.empty()) {
            text += ", ";
        }
        text += counted(static_cast<std::size_t>(n), one, many);
    };
    add(list.shown.errors, "error", "errors");
    add(list.shown.warnings, "warning", "warnings");
    add(list.shown.info, "info", "info");
    return text + " in " + counted(static_cast<std::size_t>(list.shown.scripts), "script", "scripts");
}

std::string problems_title(const ProblemList& list) {
    const int count = list.total.errors + list.total.warnings;
    return count == 0 ? std::string("Problems") : "Problems (" + std::to_string(count) + ")";
}

}  // namespace ide
```

Check `counted`'s exact output in `src/ide/Strings.hpp`. It must give "1 error" and "3 errors". If it differs, build the string inline instead.

Note that `has_error` is computed before the toggles, so a script's place in the order does not jump when Errors is toggled off.

- [ ] **Step 5: Run the tests**

Run: `cmake --build build --target engine-tests -j8 && ./build/engine-tests`
Expected: exit 0, and no `FAIL` lines.

- [ ] **Step 6: Commit**

```bash
git add src/ide/ProblemsModel.hpp src/ide/ProblemsModel.cpp tests/ProblemsModelTest.cpp tests/LuaEngineTest.cpp CMakeLists.txt
git commit -m "Add the Problems pane's model: problems grouped, sorted, filtered, and counted"
```

---

### Task 2: The IdeProblems pane and its colours

**Files:**
- Create: `src/ide/IdeProblems.hpp`, `src/ide/IdeProblems.cpp`, `tests/ProblemsTest.cpp`
- Modify: `src/ide/IdeTheme.cpp` (after the "Search" entries, ~line 174), and every `resources/themes/*.css`.
- Modify: `CMakeLists.txt`: add `src/ide/IdeProblems.cpp` next to `src/ide/IdeSearch.cpp`, and `tests/ProblemsTest.cpp` to `studio-tests`.
- Modify: `tests/StudioLayoutTest.cpp`: declare `int RunProblemsPaneTests(engine_core::Engine& engine);` beside the other declarations (~line 40), and add `failures += RunProblemsPaneTests(layout.simulation());` after `RunConflictsTests` (~line 913).

**Interfaces:**
- Consumes: Task 1's `problems_from`, `build_problems`, `problems_summary`, `problems_title`, `ProblemFilter`, `ProblemList`.
- Produces:

```cpp
struct ProblemsHost {
    // Opens the script and selects code points [column, column_end) of the 1-based line.
    std::function<void(std::uint32_t id, int line, int column, int column_end)> open;
};
class IdeProblems : public IdePane {
public:
    IdeProblems(engine_core::Engine& engine, ProblemsHost host);
    ~IdeProblems() override;
    SearchInput& filterInput() const;
    FindButton& errorsToggle() const;
    FindButton& warningsToggle() const;
    FindButton& infoToggle() const;
    jadefx::TreeView& tree() const;
    const ProblemList& list() const;
    std::string summary() const;
    bool playNoteShown() const;
    bool offNoticeShown() const;
    int rebuilds() const;            // how many times the rows were rebuilt, for tests
    void refresh();                  // gathers and rebuilds now
    bool openRow(const jadefx::TreeItem* item);
};
```

- [ ] **Step 1: Write the failing tests**

Create `tests/ProblemsTest.cpp`:

```cpp
#include "ide/IdeProblems.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

int gFailures = 0;

void expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++gFailures;
    }
}

// Pumps analysis until it has nothing left to publish, as the studio's frame does.
void settle(engine_core::Engine& engine) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!engine.analysis().idle() && std::chrono::steady_clock::now() < deadline) {
        engine.analysis().pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    engine.analysis().pump();
}

engine_core::Script& add_script(engine_core::DataModel& game, engine_core::InstanceId parent, const char* name,
                                const char* source) {
    engine_core::Script& script = game.create<engine_core::Script>();
    game.set_name(script.id(), name);
    script.set_source(source);
    game.set_parent(script.id(), parent);
    return script;
}

// A scene of its own with the pane in it, so its layout pass runs.
struct Harness {
    std::shared_ptr<jadefx::Scene> scene;
    std::shared_ptr<ide::IdeProblems> pane;
    std::vector<std::tuple<std::uint32_t, int, int, int>> opened;

    explicit Harness(engine_core::Engine& engine) {
        ide::ProblemsHost host;
        host.open = [this](std::uint32_t id, int line, int column, int column_end) {
            opened.emplace_back(id, line, column, column_end);
        };
        pane = jadefx::make<ide::IdeProblems>(engine, std::move(host));
        scene = jadefx::make<jadefx::Scene>(nullptr, 600, 400);
        scene->setRoot(pane);
        frame();
    }
    void frame() {
        static double t = 0;
        t += 0.1;
        scene->layout(600, 400, t);
    }
};

}  // namespace

int RunProblemsPaneTests(engine_core::Engine& engine) {
    gFailures = 0;
    engine_core::DataModel& game = engine.datamodel();
    const engine_core::InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& logic = game.create<engine_core::Folder>();
    game.set_name(logic.id(), "Logic");
    game.set_parent(logic.id(), workspace);
    engine_core::Script& broken = add_script(game, logic.id(), "Broken", "--!strict\nlocal é: number = \"x\"\n");
    engine_core::Script& warned = add_script(game, workspace, "Warned", "wiat(1)\n");
    engine_core::Script& clean = add_script(game, workspace, "Clean", "print(1)\n");
    settle(engine);

    Harness h(engine);
    h.pane->refresh();
    const ide::ProblemList& list = h.pane->list();
    expect(list.scripts.size() == 2, "the two scripts with problems are listed, the clean one is not");
    expect(list.scripts.size() == 2 && list.scripts[0].id == broken.id(), "the script with an error comes first");
    expect(list.scripts.size() == 2 && list.scripts[0].path == "Workspace.Logic", "its path leaves out game");
    expect(h.pane->title() == "Problems (2)", "the tab counts the error and the warning");
    expect(h.pane->summary() == "1 error, 1 warning in 2 scripts", "the summary says the same");
    expect(h.pane->tree().getRoot()->getChildren().size() == 2, "a tree row per script");

    // Opening the error: line 2, code points, so é is one column.
    const auto& rows = h.pane->tree().getRoot()->getChildren();
    if (!rows.empty() && !rows.items()[0]->getChildren().empty()) {
        h.pane->openRow(rows.items()[0]->getChildren().items()[0].get());
    }
    expect(h.opened.size() == 1 && std::get<0>(h.opened[0]) == broken.id() && std::get<1>(h.opened[0]) == 2,
           "opening a problem opens its script at its line");
    expect(h.opened.size() == 1 && std::get<2>(h.opened[0]) == 6, "at its code-point column");

    // A script row opens the script at its first problem.
    h.opened.clear();
    if (rows.size() == 2) {
        h.pane->openRow(rows.items()[1].get());
    }
    expect(h.opened.size() == 1 && std::get<0>(h.opened[0]) == warned.id() && std::get<1>(h.opened[0]) == 1,
           "opening a script row opens its first problem");

    // Toggles and filter.
    h.pane->warningsToggle().setChecked(false);
    h.frame();
    expect(h.pane->list().scripts.size() == 1, "Warnings off hides the warned script");
    h.pane->warningsToggle().setChecked(true);
    h.pane->filterInput().field().setText("wiat");
    h.frame();
    expect(h.pane->list().scripts.size() == 1 && h.pane->list().scripts[0].id == warned.id(), "the filter narrows");
    h.pane->filterInput().field().setText("");
    h.frame();

    // Collapsing survives a rebuild.
    if (!h.pane->tree().getRoot()->getChildren().empty()) {
        h.pane->tree().getRoot()->getChildren().items()[0]->setExpanded(false);
    }
    clean.set_source("print(2)\n");
    settle(engine);
    h.frame();
    expect(!h.pane->tree().getRoot()->getChildren().empty() &&
               !h.pane->tree().getRoot()->getChildren().items()[0]->isExpanded(),
           "a collapsed script stays collapsed after a rebuild");

    // Fixing the error updates the pane by itself.
    broken.set_source("--!strict\nlocal é: number = 1\n");
    settle(engine);
    h.frame();
    expect(h.pane->list().scripts.size() == 1 && h.pane->title() == "Problems (1)", "a fixed error leaves the list");

    // Many changes, one rebuild per layout pass.
    const int before = h.pane->rebuilds();
    for (int i = 0; i < 20; ++i) {
        add_script(game, workspace, ("Many" + std::to_string(i)).c_str(), "nope()\n");
    }
    settle(engine);
    h.frame();
    expect(h.pane->rebuilds() == before + 1, "many changes, one rebuild");

    // A destroyed script's row goes, and opening an old row does nothing harmful.
    const jadefx::TreeItem* stale = h.pane->tree().getRoot()->getChildren().empty()
                                        ? nullptr
                                        : h.pane->tree().getRoot()->getChildren().items()[0].get();
    game.destroy(warned.id());
    settle(engine);
    h.frame();
    bool listed = false;
    for (const ide::ScriptProblems& script : h.pane->list().scripts) {
        listed = listed || script.id == warned.id();
    }
    expect(!listed, "a destroyed script's row goes");
    h.pane->openRow(stale);

    // Play: checking stops and the note shows; the list keeps its rows.
    const std::size_t shown = h.pane->list().scripts.size();
    game.start_simulation();
    h.frame();
    expect(h.pane->playNoteShown(), "the play note shows during a playtest");
    expect(h.pane->list().scripts.size() == shown, "and the last results stay");
    game.stop_simulation();
    settle(engine);
    h.frame();
    expect(!h.pane->playNoteShown(), "the note goes after Stop");

    // Analysis off.
    engine.analysis().set_enabled(false);
    h.frame();
    expect(h.pane->offNoticeShown(), "analysis off shows the notice");
    engine.analysis().set_enabled(true);
    settle(engine);
    h.frame();
    expect(!h.pane->offNoticeShown(), "and on again hides it");

    // Clean up the scripts this test added.
    for (const ide::ScriptProblems& script : h.pane->list().scripts) {
        game.destroy(script.id);
    }
    game.destroy(clean.id());
    game.destroy(logic.id());
    settle(engine);
    return gFailures;
}
```

`IdePane::title()` already exists, and the test reads it as `h.pane->title()`.

Check the exact names before relying on them:
- `DataModel::start_simulation`, `stop_simulation`, `destroy`, `scene_service`, and `create<T>`, in `src/engine_core/DataModel.hpp`;
- `jadefx::Scene::setRoot` and `layout(w, h, t)`, as `StudioLayoutTest.cpp` uses them.

Adjust calls to the real names if they differ, and record each change in the report.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target studio-tests -j8`
Expected: compile error, `'ide/IdeProblems.hpp' file not found`.

- [ ] **Step 3: Add the colour tokens**

In `src/ide/IdeTheme.cpp`, after the last `"Search"` entry:

```cpp
        {"--ide-problems-error-color", "Problems", "Error"},
        {"--ide-problems-warning-color", "Problems", "Warning"},
        {"--ide-problems-info-color", "Problems", "Info"},
        {"--ide-problems-error-badge-color", "Problems", "Error count badge"},
        {"--ide-problems-warning-badge-color", "Problems", "Warning count badge"},
        {"--ide-problems-badge-text-color", "Problems", "Count badge text"},
        {"--ide-problems-detail-color", "Problems", "Path, line, and code"},
        {"--ide-problems-note-color", "Problems", "Playtest note"},
```

In every `resources/themes/*.css`, in its `:root` block after its `--ide-search-badge-text-color` line, declare all eight. Use values that fit that theme. Error is a red, warning an amber, and info a blue. The badges are those colours in a variant that reads behind the badge text colour. Detail and note may be `var(--ide-search-path-color)` and `var(--ide-search-status-color)`. For example, in `light.css`:

```css
    --ide-problems-error-color: #d32f2f;
    --ide-problems-warning-color: #c77c00;
    --ide-problems-info-color: #1976d2;
    --ide-problems-error-badge-color: #d32f2f;
    --ide-problems-warning-badge-color: #c77c00;
    --ide-problems-badge-text-color: #ffffff;
    --ide-problems-detail-color: var(--ide-search-path-color);
    --ide-problems-note-color: var(--ide-search-status-color);
```

`dark.css`:

```css
    --ide-problems-error-color: #f14c4c;
    --ide-problems-warning-color: #cca700;
    --ide-problems-info-color: #3794ff;
    --ide-problems-error-badge-color: #c72e2e;
    --ide-problems-warning-badge-color: #9e7f00;
    --ide-problems-badge-text-color: #ffffff;
    --ide-problems-detail-color: var(--ide-search-path-color);
    --ide-problems-note-color: var(--ide-search-status-color);
```

For the other seven themes, pick from each theme's own palette variables where it has them (nord's `--nord-nord11`, `--nord-nord13` and `--nord-nord10`, for example), and otherwise use the dark or light values above according to the theme's base.

- [ ] **Step 4: Create `src/ide/IdeProblems.hpp`**

```cpp
#pragma once

#include "FindBar.hpp"
#include "IdePane.hpp"
#include "ProblemsModel.hpp"
#include "ChangeFlag.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace engine_core {
class Engine;
}

namespace ide {

struct ProblemsHost {
    // Opens the script and selects code points [column, column_end) of the 1-based line.
    std::function<void(std::uint32_t id, int line, int column, int column_end)> open;
};

// Every error, warning, and info message the script checker has found, in a
// pane that docks like any other. Each script with a problem is a row with its
// name, where it is, and how many errors and warnings it has; its problems are
// rows under it. A filter field and Errors, Warnings, and Info toggles narrow
// the list. Clicking a problem, or Enter on it, opens its script there. The
// list follows the checker as it publishes, rebuilt at most once a frame.
// While a playtest runs the checker rests, and the pane says so and keeps the
// last results.
class IdeProblems : public IdePane {
public:
    IdeProblems(engine_core::Engine& engine, ProblemsHost host);
    ~IdeProblems() override;

    SearchInput& filterInput() const { return *filter_; }
    FindButton& errorsToggle() const { return *errors_; }
    FindButton& warningsToggle() const { return *warnings_; }
    FindButton& infoToggle() const { return *info_; }
    jadefx::TreeView& tree() const { return *tree_; }
    const ProblemList& list() const { return list_; }
    std::string summary() const;
    bool playNoteShown() const;
    bool offNoticeShown() const;
    // How many times the rows have been rebuilt. For tests.
    int rebuilds() const { return rebuilds_; }

    // Gathers the published problems and rebuilds now, instead of on a later layout.
    void refresh();
    // Opens what a row points at. False for an item that is not a row of this pane.
    bool openRow(const jadefx::TreeItem* item);

protected:
    void layoutChildren() override;
    void onOpen() override { filter_->focusAll(); }

private:
    struct Target {
        std::uint32_t id = 0;
        int line = 0;
        int column = 0;
        int column_end = 0;
    };

    ProblemFilter filter() const;
    void rebuild();
    void update_toggles();
    void clicked(const jadefx::MouseEvent& event);

    engine_core::Engine& engine_;
    ProblemsHost host_;
    std::uint64_t hook_ = 0;
    ChangeFlag changed_;
    std::shared_ptr<SearchInput> filter_;
    std::shared_ptr<FindButton> errors_;
    std::shared_ptr<FindButton> warnings_;
    std::shared_ptr<FindButton> info_;
    std::shared_ptr<jadefx::Label> summary_;
    std::shared_ptr<jadefx::Label> play_note_;
    std::shared_ptr<jadefx::Label> off_notice_;
    std::shared_ptr<jadefx::TreeView> tree_;
    std::shared_ptr<jadefx::TreeItem> root_;
    std::unordered_map<const jadefx::TreeItem*, Target> targets_;
    std::unordered_set<std::uint32_t> collapsed_;
    std::vector<ProblemSource> sources_;
    ProblemList list_;
    // What the last rebuild showed, to rebuild only when something changed.
    ProblemFilter shown_filter_;
    bool shown_playing_ = false;
    bool shown_enabled_ = true;
    bool built_once_ = false;
    int rebuilds_ = 0;
};

}  // namespace ide
```

- [ ] **Step 5: Create `src/ide/IdeProblems.cpp`**

Follow `IdeSearch.cpp`'s patterns:
- the CSS string in a `constexpr const char*`;
- `text_label` and the row-graphic helpers in the anonymous namespace;
- `ResultsTree` for Enter, copied into this file's anonymous namespace, since IdeSearch's copy is private to its file;
- `DataModelLock` with `kActionLockWait` for reading the place.

```cpp
#include "IdeProblems.hpp"
#include "LockWaits.hpp"

#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "IdeIcons.hpp"
#include "LuaSource.hpp"
#include "ScriptAnalysis.hpp"
#include "Strings.hpp"

#include <algorithm>
#include <utility>

namespace ide {
namespace {

constexpr double kRowHeight = 22;

constexpr const char* kProblemsRules = R"CSS(
.problems-pane {
    background-color: var(--ide-panel-color);
}
.problems-header {
    padding: 8px 8px 4px 8px;
    spacing: 2px;
}
.problems-summary {
    color: var(--ide-search-status-color);
    font-size: 12px;
    padding: 2px 8px 6px 8px;
}
.problems-note {
    color: var(--ide-problems-note-color);
    font-size: 12px;
    padding: 0 8px 6px 8px;
}
.problems-results {
    border-width: 1px 0 0 0;
    border-style: solid;
    border-color: var(--ide-search-divider-color);
}
.problems-detail {
    color: var(--ide-problems-detail-color);
    font-size: 12px;
}
.problems-badge {
    color: var(--ide-problems-badge-text-color);
    border-radius: 8px;
    font-size: 11px;
    padding: 0 6px;
}
.problems-badge.error {
    background-color: var(--ide-problems-error-badge-color);
}
.problems-badge.warning {
    background-color: var(--ide-problems-warning-badge-color);
}
.problems-glyph {
    min-width: 8px;
    min-height: 8px;
    max-width: 8px;
    max-height: 8px;
}
.problems-glyph.error {
    background-color: var(--ide-problems-error-color);
    border-radius: 4px;
}
.problems-glyph.warning {
    background-color: var(--ide-problems-warning-color);
    border-radius: 1px;
}
.problems-glyph.info {
    border-width: 2px;
    border-style: solid;
    border-color: var(--ide-problems-info-color);
    border-radius: 4px;
}
)CSS";

const char* severity_class(engine_core::Severity severity) {
    switch (severity) {
    case engine_core::Severity::Error:
        return "error";
    case engine_core::Severity::Warning:
        return "warning";
    default:
        return "info";
    }
}

std::shared_ptr<jadefx::Label> text_label(const std::string& text, const char* style_class) {
    auto label = jadefx::make<jadefx::Label>(text);
    if (style_class != nullptr) {
        label->getClassList().add(style_class);
    }
    label->setMouseTransparent(true);
    return label;
}

std::shared_ptr<jadefx::Node> glyph(engine_core::Severity severity) {
    auto shape = jadefx::make<jadefx::StackPane>();
    shape->getClassList().add("problems-glyph");
    shape->getClassList().add(severity_class(severity));
    shape->setPrefSize(8, 8);
    shape->setMouseTransparent(true);
    return shape;
}

std::shared_ptr<jadefx::Node> script_graphic(const ScriptProblems& script) {
    auto box = jadefx::make<jadefx::HBox>();
    box->setSpacing(6);
    box->setAlignment(jadefx::Pos::CenterLeft);
    box->setMouseTransparent(true);
    if (std::shared_ptr<jadefx::ImageView> icon = icon_view(script.class_name)) {
        icon->setPrefSize(16, 16);
        box->getChildren().add(icon);
    }
    box->getChildren().add(text_label(script.name, nullptr));
    if (!script.path.empty()) {
        box->getChildren().add(text_label(script.path, "problems-detail"));
    }
    if (script.errors > 0) {
        auto badge = text_label(std::to_string(script.errors), "problems-badge");
        badge->getClassList().add("error");
        box->getChildren().add(badge);
    }
    if (script.warnings > 0) {
        auto badge = text_label(std::to_string(script.warnings), "problems-badge");
        badge->getClassList().add("warning");
        box->getChildren().add(badge);
    }
    return box;
}

std::shared_ptr<jadefx::Node> problem_graphic(const Problem& problem) {
    auto box = jadefx::make<jadefx::HBox>();
    box->setSpacing(6);
    box->setAlignment(jadefx::Pos::CenterLeft);
    box->setMouseTransparent(true);
    box->getChildren().add(glyph(problem.severity));
    box->getChildren().add(text_label("Line " + std::to_string(problem.line), "problems-detail"));
    box->getChildren().add(text_label(problem.message, nullptr));
    box->getChildren().add(text_label(problem.code, "problems-detail"));
    return box;
}

// Enter on a row opens it.
class ProblemsTree : public jadefx::TreeView {
public:
    std::function<bool(jadefx::TreeItem*)> open;

protected:
    void handleKey(jadefx::KeyEvent& event) override {
        if (event.pressed && (event.key == jadefx::Key::Enter || event.key == jadefx::Key::KpEnter) && open &&
            open(getSelectedItem())) {
            event.consume();
            return;
        }
        TreeView::handleKey(event);
    }
};

// Every Script and ModuleScript under parent with published diagnostics,
// except the studio's own under Core. path is the chain below game.
void gather(const engine_core::DataModel& game, const engine_core::ScriptAnalysis& analysis,
            engine_core::InstanceId parent, const std::string& path, std::vector<ProblemSource>& out, int depth) {
    for (engine_core::InstanceId child = game.first_child(parent); child != 0; child = game.next_sibling(child)) {
        if (child == game.core()) {
            continue;
        }
        const std::string name = game.name(child);
        if (const auto* script = dynamic_cast<const engine_core::LuaSource*>(game.instance(child))) {
            const std::optional<std::string> checked = analysis.analyzed_source(child);
            if (checked) {
                ProblemSource source;
                source.id = child;
                source.name = name;
                source.class_name = script->class_name();
                source.path = path;
                source.problems = problems_from(*checked, analysis.diagnostics(child));
                if (!source.problems.empty()) {
                    out.push_back(std::move(source));
                }
            }
        }
        if (depth < 512) {
            gather(game, analysis, child, path.empty() ? name : path + "." + name, out, depth + 1);
        }
    }
}

}  // namespace

IdeProblems::IdeProblems(engine_core::Engine& engine, ProblemsHost host)
    : IdePane("Problems", true), engine_(engine), host_(std::move(host)) {
    setIconFile("Warning.png");
    getClassList().add("problems-pane");
    setStylesheet(std::string(kFindStylesheet) + kProblemsRules);
    setMinSize(150, 120);

    filter_ = jadefx::make<SearchInput>("Filter");
    filter_->setStyle("width: 100%;");
    errors_ = jadefx::make<FindButton>("", "Errors", "Show errors", true);
    warnings_ = jadefx::make<FindButton>("", "Warnings", "Show warnings", true);
    info_ = jadefx::make<FindButton>("", "Info", "Show info", true);
    for (const auto& toggle : {errors_, warnings_, info_}) {
        toggle->setChecked(true);
        toggle->setOnAction([this] { changed_.set(); });
    }
    auto toggles = jadefx::make<jadefx::HBox>();
    toggles->setSpacing(4);
    toggles->setAlignment(jadefx::Pos::CenterLeft);
    toggles->getChildren().add(errors_);
    toggles->getChildren().add(warnings_);
    toggles->getChildren().add(info_);

    auto header = jadefx::make<jadefx::VBox>();
    header->getClassList().add("problems-header");
    header->setSpacing(4);
    header->setStyle("width: 100%;");
    header->getChildren().add(filter_);
    header->getChildren().add(toggles);

    summary_ = text_label("", "problems-summary");
    summary_->setStyle("width: 100%;");
    play_note_ = text_label("Checking resumes when the playtest stops.", "problems-note");
    play_note_->setVisible(false);
    play_note_->setManaged(false);
    off_notice_ = text_label("Script analysis is off.", "problems-note");
    off_notice_->setVisible(false);
    off_notice_->setManaged(false);

    auto top = jadefx::make<jadefx::VBox>();
    top->setStyle("width: 100%;");
    top->getChildren().add(header);
    top->getChildren().add(summary_);
    top->getChildren().add(play_note_);
    top->getChildren().add(off_notice_);

    root_ = jadefx::make<jadefx::TreeItem>("");
    root_->setExpanded(true);
    auto tree = jadefx::make<ProblemsTree>();
    tree->open = [this](jadefx::TreeItem* item) { return openRow(item); };
    tree_ = tree;
    tree_->setRoot(root_);
    tree_->getClassList().add("problems-results");
    tree_->setShowRoot(false);
    tree_->setFixedCellSize(kRowHeight);
    tree_->setOnItemActivated([this](jadefx::TreeItem& item) { return openRow(&item); });
    tree_->setOnMouseClicked([this](const jadefx::MouseEvent& event) { clicked(event); });

    auto column = jadefx::make<jadefx::BorderPane>();
    Fill(*column);
    column->setTop(top);
    column->setCenter(tree_);
    getChildren().add(column);

    hook_ = engine_.analysis().diagnostics_changed().connect(
        [changed = changed_.setter()](engine_core::InstanceId) { changed(); });
}

IdeProblems::~IdeProblems() { engine_.analysis().diagnostics_changed().disconnect(hook_); }

ProblemFilter IdeProblems::filter() const {
    ProblemFilter out;
    out.text = filter_->text();
    out.errors = errors_->isChecked();
    out.warnings = warnings_->isChecked();
    out.info = info_->isChecked();
    return out;
}

std::string IdeProblems::summary() const { return summary_->getText(); }
bool IdeProblems::playNoteShown() const { return play_note_->isVisible(); }
bool IdeProblems::offNoticeShown() const { return off_notice_->isVisible(); }

void IdeProblems::refresh() {
    engine_core::DataModel& game = engine_.datamodel();
    std::vector<ProblemSource> gathered;
    {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionLockWait);
        if (!lock.owns()) {
            // The place is busy. The next layout tries again.
            changed_.set();
            return;
        }
        gather(game, engine_.analysis(), game.id(), "", gathered, 0);
    }
    sources_ = std::move(gathered);
    rebuild();
}

void IdeProblems::rebuild() {
    const ProblemFilter wanted = filter();
    list_ = build_problems(sources_, wanted);
    shown_filter_ = wanted;
    built_once_ = true;
    ++rebuilds_;

    std::optional<Target> selected;
    if (const jadefx::TreeItem* item = tree_->getSelectedItem()) {
        const auto found = targets_.find(item);
        if (found != targets_.end()) {
            selected = found->second;
        }
    }
    jadefx::TreeItem* reselect = nullptr;
    targets_.clear();
    std::vector<std::shared_ptr<jadefx::TreeItem>> rows;
    rows.reserve(list_.scripts.size());
    for (const ScriptProblems& script : list_.scripts) {
        auto row = jadefx::make<jadefx::TreeItem>("", script_graphic(script));
        const std::uint32_t id = script.id;
        row->setExpanded(collapsed_.count(id) == 0);
        row->setOnCollapsed([this, id](jadefx::TreeItem&) { collapsed_.insert(id); });
        row->setOnExpanded([this, id](jadefx::TreeItem&) { collapsed_.erase(id); });
        const Problem& first = script.problems.front();
        targets_[row.get()] = Target{id, 0, first.column, first.column_end};
        if (selected && selected->id == id && selected->line == 0) {
            reselect = row.get();
        }
        for (const Problem& problem : script.problems) {
            auto child = jadefx::make<jadefx::TreeItem>("", problem_graphic(problem));
            targets_[child.get()] = Target{id, problem.line, problem.column, problem.column_end};
            if (selected && selected->id == id && selected->line == problem.line &&
                selected->column == problem.column) {
                reselect = child.get();
            }
            row->getChildren().add(std::move(child));
        }
        rows.push_back(std::move(row));
    }
    root_->getChildren().setAll(std::move(rows));
    if (reselect != nullptr) {
        tree_->select(reselect);
    }
    summary_->setText(problems_summary(list_));
    setTitle(problems_title(list_) == "Problems" ? std::string() : problems_title(list_));
    update_toggles();
}

void IdeProblems::update_toggles() {
    errors_->setText("Errors (" + std::to_string(list_.matching.errors) + ")");
    warnings_->setText("Warnings (" + std::to_string(list_.matching.warnings) + ")");
    info_->setText("Info (" + std::to_string(list_.matching.info) + ")");
}

bool IdeProblems::openRow(const jadefx::TreeItem* item) {
    if (item == nullptr) {
        return false;
    }
    const auto found = targets_.find(item);
    if (found == targets_.end()) {
        return false;
    }
    Target where = found->second;
    if (where.line == 0) {
        // A script row opens its first shown problem.
        for (const ScriptProblems& script : list_.scripts) {
            if (script.id == where.id && !script.problems.empty()) {
                where.line = script.problems.front().line;
                break;
            }
        }
    }
    if (host_.open && where.line > 0) {
        // Copied first: opening a script can refresh the list and drop this row.
        host_.open(where.id, where.line, where.column, where.column_end);
    }
    return true;
}

void IdeProblems::clicked(const jadefx::MouseEvent& event) {
    if (event.button != 0 ||
        (event.mods & (jadefx::Key::ModShift | jadefx::Key::ModControl | jadefx::Key::ModSuper)) != 0) {
        return;
    }
    for (jadefx::Node* node = tree_->pick(event.x, event.y); node != nullptr && node != tree_.get();
         node = node->getParent()) {
        const std::string_view type = node->getElementType();
        if (type == "tree-disclosure-node") {
            return;
        }
        if (type == "tree-cell") {
            openRow(tree_->getSelectedItem());
            return;
        }
    }
}

void IdeProblems::layoutChildren() {
    const bool playing = engine_.datamodel().simulation_running();
    const bool enabled = engine_.analysis().enabled();
    if (playing != shown_playing_) {
        shown_playing_ = playing;
        play_note_->setVisible(playing);
        play_note_->setManaged(playing);
    }
    if (enabled != shown_enabled_) {
        shown_enabled_ = enabled;
        off_notice_->setVisible(!enabled);
        off_notice_->setManaged(!enabled);
        tree_->setVisible(enabled);
        changed_.set();
    }
    const ProblemFilter wanted = filter();
    const bool filter_changed = wanted.text != shown_filter_.text || wanted.errors != shown_filter_.errors ||
                                wanted.warnings != shown_filter_.warnings || wanted.info != shown_filter_.info;
    if (changed_.take() || !built_once_) {
        refresh();
    } else if (filter_changed) {
        rebuild();
    }
    IdePane::layoutChildren();
}

}  // namespace ide
```

Verify these against the real headers before building, and adapt any that differ, noting each change in the report:
- `FindButton`'s constructor with an empty icon name;
- that `FindButton` has `setText`, since it is a `jadefx::Label`;
- `jadefx::Node::setManaged` and `setVisible`;
- `jadefx::TreeView::select` and `getSelectedItem`;
- `TreeItem::setOnCollapsed` and `setOnExpanded`;
- `getChildren().setAll`;
- `icon_view`.

If `FindButton` cannot show text without an icon, use `jadefx::ToggleButton` with the same `isChecked`/`setChecked` contract, keeping the accessor names. `ChangeFlag` starts set, so the first layout gathers.

- [ ] **Step 6: Run the tests**

Run: `cmake --build build --target studio-tests -j8 && ./build/studio-tests`
Expected: exit 0, and no `FAIL` lines (ThemeTest included).

- [ ] **Step 7: Commit**

```bash
git add src/ide/IdeProblems.hpp src/ide/IdeProblems.cpp src/ide/IdeTheme.cpp resources/themes tests/ProblemsTest.cpp tests/StudioLayoutTest.cpp CMakeLists.txt
git commit -m "Add the Problems pane: every script's errors and warnings, grouped, filtered, and live"
```

---

### Task 3: The Problems window in the studio

**Files:**
- Modify: `src/ide/IdeLayout.hpp`: declare `std::shared_ptr<IdePane> make_problems();` next to `make_search()` (private), `WindowEntry* problems_window_ = nullptr;` next to `conflicts_window_`, and public `void show_problems();` next to `show_conflicts()`. Forward-declare `class IdeProblems;` beside `class IdeSearch;`.
- Modify: `src/ide/IdeLayout.cpp` (~line 262): register the entry.
- Modify: `src/ide/IdeLayoutEditing.cpp`: `make_problems()` and `show_problems()`.
- Modify: `src/ide/IdeLayoutInternal.hpp`: `#include "IdeProblems.hpp"` beside `#include "IdeSearch.hpp"`.
- Test: `tests/ProblemsTest.cpp`: add `RunProblemsWindowTests(ide::IdeLayout&, jadefx::Scene&)`, declared and called in `tests/StudioLayoutTest.cpp` after `RunProblemsPaneTests`.

**Interfaces:**
- Consumes: Task 2's `IdeProblems`, `ProblemsHost`.
- Produces: `void IdeLayout::show_problems();`, and the window entry named "Problems".

- [ ] **Step 1: Write the failing test**

Append to `tests/ProblemsTest.cpp`, adding `#include "ide/IdeLayout.hpp"` and `#include "ide/IdeDock.hpp"` at the top:

```cpp
int RunProblemsWindowTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    gFailures = 0;
    expect(scene.getElementsByClassName("problems-pane").empty(), "Problems starts closed");
    layout.show_problems();
    scene.layout(1280, 800, 50.0);
    const std::vector<jadefx::Node*> panes = scene.getElementsByClassName("problems-pane");
    expect(panes.size() == 1, "show_problems opens one Problems pane");
    auto* pane = panes.empty() ? nullptr : dynamic_cast<ide::IdeProblems*>(panes.front());
    expect(pane != nullptr && pane->name() == "Problems", "named Problems for the Window menu and layout.json");

    // It opens a script at the problem's line.
    engine_core::Engine& engine = layout.simulation();
    engine_core::DataModel& game = engine.datamodel();
    engine_core::Script& script =
        add_script(game, game.scene_service("Workspace"), "Opened", "local a = 1\nwiat(1)\n");
    settle(engine);
    scene.layout(1280, 800, 50.1);
    if (pane != nullptr) {
        pane->refresh();
        const auto& rows = pane->tree().getRoot()->getChildren();
        jadefx::TreeItem* row = nullptr;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!rows.items()[i]->getChildren().empty()) {
                row = rows.items()[i]->getChildren().items()[0].get();
            }
        }
        expect(pane->openRow(row), "a problem row opens");
    }
    scene.layout(1280, 800, 50.2);
    bool editor_open = false;
    for (jadefx::Node* node : scene.getElementsByClassName("ide-pane")) {
        auto* page = dynamic_cast<ide::IdePane*>(node);
        editor_open = editor_open || (page != nullptr && page->name() == "Opened");
    }
    expect(editor_open, "opening a problem opens its script in an editor");

    // Renaming a script renames its row.
    game.set_name(script.id(), "Renamed");
    settle(engine);
    scene.layout(1280, 800, 50.3);
    bool renamed = false;
    if (pane != nullptr) {
        for (const ide::ScriptProblems& listed : pane->list().scripts) {
            renamed = renamed || (listed.id == script.id() && listed.name == "Renamed");
        }
    }
    expect(renamed, "renaming a script renames its row");

    game.destroy(script.id());
    settle(engine);
    return gFailures;
}
```

`add_script` and `settle` are the helpers already in this file's anonymous namespace. Check how an editor pane names itself. The constructor sets "Script.lua", and the editor may rename itself to the script's name; if it does not, find the opened editor through `scene.getElementsByClassName` on the script editor's class and check its id. Adjust the assertion to whichever is real, and record it in the report.

Renaming a script changes no diagnostics, so `diagnostics_changed` may not fire. The rename invalidates the script, so a recheck publishes and fires it. If the test shows it does not, also set the pane's flag from a `DataModel` change you can observe cheaply, and record that in the report.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target studio-tests -j8`
Expected: compile error, `no member named 'show_problems' in 'ide::IdeLayout'`.

- [ ] **Step 3: Wire the window**

`src/ide/IdeLayout.cpp`, after `conflicts_window_ = &keep_closed(...)`:

```cpp
    problems_window_ = &keep_closed("Problems", "Warning.png", [this] { return make_problems(); });
    // In with the console, as Assets docks.
    problems_window_->home = [this] { return beside_console(); };
```

`src/ide/IdeLayoutEditing.cpp`, after `make_search()`:

```cpp
std::shared_ptr<IdePane> IdeLayout::make_problems() {
    ProblemsHost host;
    // As Search opens a match.
    host.open = [this](std::uint32_t id, int line, int column, int column_end) {
        edit(id);
        if (const std::shared_ptr<IdeScriptEditor> editor = open_editor(id)) {
            editor->showRange(line, column, column_end);
        }
    };
    return jadefx::make<IdeProblems>(runner_.simulation(), std::move(host));
}

void IdeLayout::show_problems() { open_window(*problems_window_); }
```

In `src/ide/IdeLayout.hpp`, put the public declaration under `show_conflicts()`, with the comment `// Opens the Problems window, or brings it forward.`

- [ ] **Step 4: Run the tests**

Run: `cmake --build build --target studio-tests AnarchyStudio -j8 && ./build/studio-tests`
Expected: exit 0, and no `FAIL` lines.

- [ ] **Step 5: Look at it**

Run `cmake --build build --target AnarchyStudio bundle-resources -j8` (without `bundle-resources`, icons and themes go missing). Open the studio on a project with scripts, open Window → Problems, and check each of these:
- it docks beside the console;
- the rows show glyphs, paths and badges in the Light and Dark themes;
- clicking a problem opens the script at the line;
- typing an error in a script adds a row shortly after.

- [ ] **Step 6: Commit**

```bash
git add src/ide/IdeLayout.hpp src/ide/IdeLayout.cpp src/ide/IdeLayoutEditing.cpp src/ide/IdeLayoutInternal.hpp tests/ProblemsTest.cpp tests/StudioLayoutTest.cpp
git commit -m "Open the Problems window from the Window menu, docked beside the console"
```
