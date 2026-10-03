# Problems Pane Design

2026-10-03 · The second of two projects. The first (`2026-10-03-parallel-script-analysis-design.md`) made the checker cover every script in the place, at all times in Edit mode, on a thread pool. This one shows its results: a dockable pane listing every error, warning, and info message in every script, like Eclipse's Problems and Error Log views.

## Goal

A developer sees every problem in the game's scripts in one place, without opening any script, and gets to any of them in one click. The list stays current as they edit, and costs nothing extra: it only reads what the checker already publishes.

## Decisions

| Question | Decision |
| --- | --- |
| Layout | Grouped by script, a tree like the Search pane. A script row is collapsible; its problems are rows under it. |
| Script row | Script or ModuleScript icon, name, path (the parent chain from `game`, dimmed, e.g. `Workspace.Logic`), and badges with its error and warning counts. |
| Problem row | A severity glyph, `Line N`, the message on one line, and the rule code dimmed at the end (`Type`, `Syntax`, `Lint/UnknownGlobal`, ...). |
| Severities shown | Error, Warning, Information. Hints (unused locals, functions, imports) are left out: the editor dims them, and they would swamp the list. |
| Order | Scripts with an error first, then scripts with only warnings or info; within each, by path then name. Within a script: errors, then warnings, then info, each by line then column. |
| Filter | A field matching, case-insensitively, the message, the script's name, its path, or the code. A script row shows when any of its problems match. |
| Severity toggles | Errors (n), Warnings (n), Info (n) buttons, each on by default; counts are of what the filter field passes. |
| Summary | "3 errors, 5 warnings in 4 scripts", "1 warning in 1 script", "No problems", or "No problems match" when the filter or toggles hide everything. Info is counted only when there is some. |
| Tab title | "Problems (n)", n being errors plus warnings in the place, unfiltered; "Problems" when there are none. As Conflicts does with `setTitle`. |
| Open | Clicking a problem row, or Enter on it, opens the script with the problem's range selected. Clicking a script row, or Enter on it, opens the script at its first shown problem. |
| Live | The pane listens to `ScriptAnalysis::diagnostics_changed` and rebuilds once on its next layout, however many scripts changed, so a large batch causes one rebuild per frame at most. |
| Kept across rebuilds | Collapsed scripts, by id. The selected row, when its script and line are still shown. |
| Play | Checking stops while a playtest runs. The pane keeps showing the last Edit-mode results, with a note above the list: "Checking resumes when the playtest stops." It updates by itself after Stop. |
| Analysis off | The list is replaced by "Script analysis is off." |
| Where | Window menu → Problems. Closed until opened; its home is beside the console, as Assets. The tab icon is `Warning.png`. |
| Glyphs | Drawn as small shapes in theme colours, not image files: a filled circle for an error, a triangle for a warning, a ring for info. |
| Theme | New colours in the theme editor's list, under a "Problems" group: error, warning, info, error badge, warning badge, badge text, path and code text, play note. |

## Architecture

### 1. `ProblemsModel` (pure, `src/ide/ProblemsModel.hpp/.cpp`)

Turns diagnostics into the rows the pane shows. No JadeFX, no Engine.

```cpp
struct ProblemSource {
    std::uint32_t id;
    std::string name;
    std::string class_name;
    std::string path;
    std::vector<engine_core::Diagnostic> diagnostics;
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
    std::uint32_t id;
    std::string name, class_name, path;
    std::vector<engine_core::Diagnostic> problems;
    int errors, warnings, info;
};

struct ProblemList {
    std::vector<ScriptProblems> scripts;
    ProblemCounts shown;
    ProblemCounts total;
};

ProblemList build_problems(std::vector<ProblemSource> sources, const ProblemFilter& filter);
std::string problems_summary(const ProblemList& list);
std::string problems_title(const ProblemList& list);
```

All of the ordering, filtering, hint removal, counting, and wording lives here, so it is tested exhaustively without a window.

### 2. `IdeProblems` (`src/ide/IdeProblems.hpp/.cpp`)

An `IdePane` shaped like `IdeSearch`.

- **Top:** the filter field (the same `SearchInput` as Search) and the three toggles, then the summary label. Below them, the play note and the analysis-off notice, each hidden unless needed.
- **Body:** a `TreeView` whose items are built from a `ProblemList`, with the same row graphics style as Search's script and line rows.
- **Host:** `ProblemsHost` carries one callback, `std::function<void(std::uint32_t id, int line, int column, int column_end)> open`. That is the callback Search uses, and `IdeLayout` fills it the same way.
- **Gathering:** on rebuild, the pane reads `analysis.diagnostics()` and the place (name, class, and path of each script with diagnostics), on the UI thread, as the editor reads its marks. Lines and columns become 1-based code points for `open`, through the same conversion the editor's marks use (`ScriptMarks`).
- **Change detection:** a `diagnostics_changed` connection sets a flag. `layoutChildren` rebuilds when the flag is set, and also when the filter, a toggle, play state, or enabled state changed. The connection is dropped in the destructor.

### 3. Wiring (`IdeLayout`)

`make_problems()` builds the pane with its host. `keep_closed("Problems", "Warning.png", ...)` registers it, with its home `beside_console()`. `layout.json` and the Window menu know it by the name "Problems".

### 4. Theme

The new colour tokens go in the shipped themes and in `IdeTheme.cpp`'s editor list, under "Problems", as the Search entries are.

## Testing

`tests/ProblemsTest.cpp` in `studio-tests` style (as `ConflictsTest`).

**`ProblemsModel`:**
- grouping and order (error scripts first, path order, severity then line within a script);
- hints dropped;
- each toggle;
- the filter on message, name, path, and code, case-insensitive;
- shown and total counts;
- every summary wording (none, filtered to none, singular and plural, info only when present);
- the title.

**`IdeProblems` against a real `Engine`:**
- scripts with problems appear, and the title counts them;
- editing a script to fix its error updates the pane after the checker settles;
- opening a problem row calls `open` with the right 1-based line and columns;
- collapse state survives a rebuild;
- the play note shows during a playtest and the list keeps the last results;
- analysis off shows the notice.

## Out of scope

- Runtime errors from the Output window.
- Quick fixes, and ignoring or suppressing a problem from the pane.
- Copy and export.
- Showing hints (a later toggle if wanted).
