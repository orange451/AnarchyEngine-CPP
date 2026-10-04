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
    // It shows an error. One the toggles or the filter hide does not count.
    bool has_error = false;
    int errors = 0;
    int warnings = 0;
};

struct ProblemList {
    // Scripts showing an error first, then the rest; within each, by path then name.
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
