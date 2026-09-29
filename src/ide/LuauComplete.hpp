#pragma once

#include "LuaApi.hpp"

#include <chrono>
#include <memory>
#include <optional>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine_core {
class ScriptAnalysis;
struct LuauAnswer;
struct LuauFacts;
}  // namespace engine_core

namespace ide {

// Where the caret is asking for a completion.
enum class CompleteSite {
    None,
    Member,     // After '.' or ':'.
    Name,       // A local, global, or keyword.
    Type,       // A type name after ':' or '::'.
    Argument,   // A string argument of GetService, FindFirstChild, or Instance.new.
    Directive,  // A header `--!` comment: strict, nonstrict, nocheck, nolint, native, optimize.
    Require,    // A `.` that starts a line at the top of a script: a ModuleScript or service to declare.
};

struct CompletionItem {
    std::string name;
    std::string detail;
    // What accepting writes in place of the typed text. Empty writes `name`.
    std::string insert;
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
    // The value being written wants this row: a string its type allows, a key
    // of the table type being built, or a name of the type it expects.
    bool expected = false;
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
    // Byte range of `signature` naming the parameter being typed, drawn bold.
    // -1 when no parameter is active.
    int signature_bold_begin = -1;
    int signature_bold_end = -1;
    // Before a name is typed, the list opens by itself with its expected rows
    // alone: a type's strings after `local diet: Diet = `, or a table type's
    // keys after `{`. Never at the start of a line, where Enter makes a line.
    bool open_expected = false;
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

// Completion from Luau's own type checker, with the registry's docs, children,
// services, requires, and directives. plan_completion reads the text alone and
// says what Luau must answer; finish_completion builds the list from those
// answers. A UI asks between the two without waiting. `list` is what shows
// before an answer, complete when nothing is asked.
//
// `world` is the live instance tree and `script_id` the script being edited;
// `script_global` is false on the command line, where `script` is nil.
// Members come from the class registry for registered classes and from Luau
// for everything else, so a required module's table, a metatable object, a
// loop variable, and a generic result complete as Luau types them. A function
// a module replaces after defining it reads as the replacement.
// GetService("...") completes registered services, FindFirstChild("...") the
// receiver's children, and Instance.new("...") the classes it can create.
// Connect(function) completes the signal's callback arguments, and any other
// function-typed argument a function with its parameters. A value of a written
// type offers what that type expects first: the strings a literal type allows,
// in quotes or not, a table type's keys, and names of that type. Keywords are
// those the position can hold, such as `then` after a condition. A function row
// carries its return and the sentence its hover shows. A header comment `--!`
// completes strict, nonstrict, nocheck, nolint, native, and optimize; a `.`
// that starts a line at the top of a script completes the ModuleScripts and
// services to declare, writing `local Config = require(game.Folder.Config)`.
struct CompletionPlanState;
struct CompletionPlan {
    CompletionList list;
    bool needs_luau = false;
    // The completion at this byte offset, or none when it is npos, and the type
    // at each of `offsets`, as ScriptAnalysis::luau_facts takes them.
    std::size_t caret_offset = std::string::npos;
    std::vector<std::size_t> offsets;
    // The text to send: the source with open blocks closed at its end.
    std::string luau_source;
    // The site, prefix, and range the finished list will have, as far as the
    // text says, for a popup to narrow to meanwhile. No site when unknown.
    CompletionList frame;
    std::shared_ptr<const CompletionPlanState> state;
};
CompletionPlan plan_completion(std::string_view source, int caret, const std::vector<engine_core::LuaNode>& world,
                               std::uint32_t script_id, bool script_global);
CompletionList finish_completion(const CompletionPlan& plan, const engine_core::LuauFacts& facts);

struct HoverPlanState;
struct HoverPlan {
    HoverInfo info;
    bool needs_luau = false;
    std::vector<std::size_t> offsets;
    std::string luau_source;
    std::shared_ptr<const HoverPlanState> state;
};
HoverPlan plan_hover(std::string_view source, int index, const std::vector<engine_core::LuaNode>& world,
                     std::uint32_t script_id, bool script_global);
HoverInfo finish_hover(const HoverPlan& plan, const engine_core::LuauFacts& facts);

// A completion list Luau is still answering for. A UI asks on a keystroke and
// shows the list on a later frame, while the text and caret are still what it
// asked about, so typing never waits on the type checker.
struct PendingCompletion {
    std::string source;
    int caret = 0;
    bool force = false;
    // Whether the popup was open once it was asked, and how often it had
    // closed. The answer shows only while both hold, so a popup closed since
    // stays closed.
    bool shown = false;
    std::uint64_t dismissals = 0;
    CompletionPlan plan;
    std::shared_ptr<const engine_core::LuauAnswer> answer;
};
// Plans the list at the caret. A list that needs no type check is `now`, and
// nothing is pending; otherwise Luau is asked in `lane`, one per UI, so a
// newer keystroke there replaces a request still waiting.
std::optional<PendingCompletion> ask_completion(CompletionList& now, engine_core::ScriptAnalysis& analysis,
                                                std::string_view source, int caret,
                                                const std::vector<engine_core::LuaNode>& world,
                                                std::uint32_t script_id, bool script_global, bool force,
                                                const char* lane);
// The list once Luau has answered. Nothing while it has not.
std::optional<CompletionList> take_completion(const PendingCompletion& pending);

// A hover Luau is still answering for.
struct PendingHover {
    std::string source;
    HoverPlan plan;
    std::shared_ptr<const engine_core::LuauAnswer> answer;
};
// Plans the hover at `index`. One that needs no type check is `now`.
std::optional<PendingHover> ask_hover(HoverInfo& now, engine_core::ScriptAnalysis& analysis, std::string_view source,
                                      int index, const std::vector<engine_core::LuaNode>& world,
                                      std::uint32_t script_id);
// The hover once Luau has answered. Nothing while it has not.
std::optional<HoverInfo> take_hover(const PendingHover& pending);

// Both steps at once, waiting on `analysis` up to `wait`. With no analysis, a
// shared one serves, as tests use.
CompletionList complete_luau(std::string_view source, int caret, const std::vector<engine_core::LuaNode>& world = {},
                             std::uint32_t script_id = 0, bool script_global = true,
                             engine_core::ScriptAnalysis* analysis = nullptr,
                             std::chrono::milliseconds wait = std::chrono::seconds(20));
HoverInfo hover_luau(std::string_view source, int index, const std::vector<engine_core::LuaNode>& world = {},
                     std::uint32_t script_id = 0, bool script_global = true,
                     engine_core::ScriptAnalysis* analysis = nullptr,
                     std::chrono::milliseconds wait = std::chrono::seconds(20));

}  // namespace ide
