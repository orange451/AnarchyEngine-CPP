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
};

struct CompletionList {
    CompleteSite site = CompleteSite::None;
    // Code-point range the accepted name replaces. Same unit as CodeArea.
    int replace_begin = 0;
    int replace_end = 0;
    std::string prefix;
    std::vector<CompletionItem> items;
    // Code-point position of the '(' of the call the caret is inside, and the
    // argument it is in. -1 outside a call.
    int call_open = -1;
    int call_argument = 0;
    // Member site: the resolver knew what the receiver holds, so an empty list
    // means it has no such member. False when it could not follow the value.
    bool receiver_known = false;
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
};

// One parameter of a call's signature. `name` may be empty.
struct SignatureParam {
    std::string name;
    std::string type_name;
};

// Writes `list.signature` for a call with these parameters, with `active` drawn
// bold, as the resolver writes its own.
void set_signature(CompletionList& list, const std::vector<SignatureParam>& params, bool variadic, int active);

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
HoverInfo resolver_hover(std::string_view source, int index, const std::vector<engine_core::LuaNode>& world = {},
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
// A `.` that starts a line outside every function, block, and bracket completes
// the ModuleScripts in `world` and the registered services whose names start with
// what follows it. Accepting writes the whole declaration over the dot and name:
// `local Config = require(game.Folder.Config)` or
// `local UserInputService = game:GetService("UserInputService")`.
// `script_global` is false on the command line, where `script` is nil. The
// command line does not complete requires.
CompletionList resolver_complete(std::string_view source, int caret, const std::vector<engine_core::LuaNode>& world = {},
                             std::uint32_t script_id = 0, bool script_global = true);

// Completion from Luau's own type checker, with the registry's docs, children,
// services, requires, and directives. plan_completion reads the text alone and
// says what Luau must answer; finish_completion builds the list from those
// answers. A UI asks between the two without waiting. `list` is what shows
// before an answer, complete when nothing is asked.
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
    // Whether the popup was open once it was asked. The answer shows only while
    // that still holds, so a popup closed since stays closed.
    bool shown = false;
    CompletionPlan plan;
    std::shared_ptr<const engine_core::LuauAnswer> answer;
};
// Plans the list at the caret. A list that needs no type check is `now`, and
// nothing is pending; otherwise Luau is asked in `analysis`'s completion lane.
std::optional<PendingCompletion> ask_completion(CompletionList& now, engine_core::ScriptAnalysis& analysis,
                                                std::string_view source, int caret,
                                                const std::vector<engine_core::LuaNode>& world,
                                                std::uint32_t script_id, bool script_global, bool force);
// The list once Luau has answered. Nothing while it has not.
std::optional<CompletionList> take_completion(const PendingCompletion& pending);
// Waits up to `wait` for the answer, as an accept does before it reads the
// popup. True when it arrived. kSettleWait is how long an accept waits.
inline constexpr std::chrono::milliseconds kSettleWait{1000};
bool settle_completion(const PendingCompletion& pending, std::chrono::milliseconds wait);

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
