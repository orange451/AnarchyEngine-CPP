#pragma once

#include "LuaApi.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ide {

// Where the caret is asking for a completion.
enum class CompleteSite {
    None,
    Member,     // After '.' or ':'.
    Name,       // A local, global, or keyword.
    Type,       // A type name after ':' or '::'.
    Argument,   // A string argument of GetService, FindFirstChild, or Instance.new.
    Directive,  // A header `--!` comment: strict, nonstrict, nocheck, nolint, native, optimize.
};

struct CompletionItem {
    std::string name;
    std::string detail;
    // True when accepting the name can insert a call's parentheses.
    bool call = false;
    // `name` already contains the punctuation, as in `function(dt)`.
    // Typing `(` does not accept it.
    bool snippet = false;
    // Signature for the highlighted row. The same line the hover tooltip uses.
    // Empty when this row has nothing to explain.
    std::string title;
    // What a function returns: a type such as "number", or "returns nothing".
    // Empty when this row is not a function, or the return is not known.
    std::string returns;
    // One sentence. The same explanation the hover tooltip shows.
    std::string summary;
};

struct CompletionList {
    CompleteSite site = CompleteSite::None;
    // Code-point range the accepted name replaces. Same unit as CodeArea.
    int replace_begin = 0;
    int replace_end = 0;
    std::string prefix;
    std::vector<CompletionItem> items;
    // Wrapping quote of an argument completion. 0 for every other site.
    char close_quote = 0;
    // The closing quote is not in the buffer yet. Accepting can type it.
    bool unclosed = false;
    // Parameter list of the call being written, such as "(a: string, b: Instance)".
    // Shown above the rows. Accepting does not insert it.
    std::string signature;
};

// Text for the popup shown while the pointer rests on a name.
struct HoverInfo {
    bool found = false;
    // Code-point range of the name under the pointer. Same unit as CodeArea.
    int begin = 0;
    int end = 0;
    // `count: number`, `function task.wait(seconds: number)`, or `task`.
    std::string title;
    // `local`, `parameter`, `library`, `type`, or `returns nothing`.
    std::string detail;
    // One sentence for a library or a host function. Empty when there is nothing to add.
    std::string summary;
};

// The name at code-point `index`. A variable reports its type. A function reports
// its parameters and return. A library such as `task` reports what it is.
// A function brought in by require uses every value of its first return. A later
// return does not replace that list. `local x, y = Module:Test()` types each
// name from the value in that position.
// `script_global` is false on the command line, where `script` is nil.
HoverInfo hover_luau(std::string_view source, int index, const std::vector<engine_core::LuaNode>& world = {},
                     std::uint32_t script_id = 0, bool script_global = true);

// `world` is the live instance tree. `script_id` is the script being edited.
// Members come from the class registry and from the libraries the play VM loads.
// require of a ModuleScript runs that source and completes whatever it returns.
// GetService("...") completes registered services. FindFirstChild("...") completes
// the receiver's children. Instance.new("...") completes classes Instance.new can
// create. Connect(function) completes the signal's callback
// arguments, so Heartbeat offers function(dt). A function written in the source
// keeps its parameters: the body uses each annotation as the parameter's type,
// and a call lists those parameters. A function row also carries the return
// and the one-sentence explanation the hover tooltip shows. Two or more return
// values are shown as `(number, string)`. A header comment `--!` completes
// strict, nonstrict, nocheck, nolint, native, and optimize. `--!nolint` then
// completes lint rule names, and `--!optimize` completes levels 0, 1, and 2.
// A directive after the first statement is ignored, so it is not completed.
// `script_global` is false on the command line, where `script` is nil.
CompletionList complete_luau(std::string_view source, int caret, const std::vector<engine_core::LuaNode>& world = {},
                             std::uint32_t script_id = 0, bool script_global = true);

}  // namespace ide
