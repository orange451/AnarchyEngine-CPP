#pragma once

#include "LuaApi.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace engine_core {

class DataModel;

// Studio-style static check of Script and ModuleScript source.
// This is not the compiler. A type warning still compiles and runs.
enum class Severity { Error, Warning, Information, Hint };

// Rule names a header `--!nolint` comment can name. Unknown is not a rule.
void lint_rule_names(std::vector<std::string>& out);

// 0-based, matching Luau's Location.
struct TextPos {
    std::uint32_t line = 0;
    std::uint32_t character = 0;
};

struct TextRange {
    TextPos start;
    TextPos end;
};

// `text` on one line: line breaks and tabs become spaces. Longer than
// `max_bytes`, it is cut at a whole character and ends in "...".
std::string one_line(std::string text, std::size_t max_bytes = std::string::npos);

struct Diagnostic {
    InstanceId script = 0;
    TextRange range;
    Severity severity = Severity::Error;
    std::string code;
    std::string message;
};

// One name Luau's own autocomplete offers at a position.
struct LuauSuggestion {
    std::string name;
    // property, binding, keyword, string, type, module, function, require path,
    // or hot comment.
    std::string kind;
    // The type as Luau prints it. Empty for a keyword.
    std::string type;
    // Luau recommends parentheses after the name.
    bool call = false;
    // Luau lists the name, but not for this operator: a method after '.', or a
    // field after ':'.
    bool wrong_index = false;
    // A function. `params` is its parameter list as written after its name,
    // such as "(amount: number)", without self when it is called with ':'.
    // `returns` is one type, "(a, b)" for several, or empty for none.
    bool function = false;
    std::string params;
    std::string returns;
    // The same parameters one by one, as name and type. A name may be empty.
    std::vector<std::pair<std::string, std::string>> param_list;
    bool variadic = false;
    // The registered class the name is a member of, or for a library's member
    // the library, such as "task". Empty for a table's own field.
    std::string owner;
    // Luau finds the type matches what the position expects.
    bool type_correct = false;
    // A method, declared to take self.
    bool method = false;
    // The registered class of the value, such as Folder for a child instance.
    std::string class_name;
    // The name its definition wrote after `function`, such as module:Test or
    // module.new. Empty for an anonymous function or a host function.
    std::string defined_as;
    // Its returns disagree, as `return 1` in one place and `return "x"` in
    // another. `returns` then claims nothing.
    bool returns_disagree = false;
    // Its body returns no value anywhere, as `function() end`.
    bool returns_none = false;
    // It takes a receiver first, so a ':' call suits it: written with ':', or
    // a first parameter annotated or named self with a type that says what.
    bool takes_receiver = false;
    // A local in scope at the position, and where it was declared, as
    // line * 65536 + column, so the nearest can come first.
    bool local = false;
    std::uint32_t declared = 0;
    // A global this script defines, as `function take() end` does.
    bool defined_here = false;
    // The type a local's declaration wrote, such as Diet. Empty when none.
    std::string written_type;
    // A type name's `type` statement as written, such as
    // `type Diet = "herbivore" | "carnivore"`. Empty when not found.
    std::string declaration;
    // What Luau writes for a function it generates for a function-typed
    // argument, such as `function(a: number)  end`.
    std::string insert;
};

struct LuauTypeAt;

struct LuauCompletion {
    // False when the editor checker did not answer in time, or analysis cannot run.
    bool ran = false;
    // expression, statement, property, type, keyword, string, hot comment, or unknown.
    std::string context;
    std::vector<LuauSuggestion> items;
    std::string error;
    // After '.' or ':', what the expression before it is. Empty otherwise.
    std::string receiver_class;
    std::string receiver_global;
    bool receiver_instance_known = false;
    InstanceId receiver_instance = 0;
    // The receiver's type when it is a primitive, such as string.
    std::string receiver_type;
};

// What Luau's type checker knows about the name or expression at a position.
struct LuauTypeAt {
    bool ran = false;
    bool found = false;
    // The local, global, or member written there.
    std::string name;
    // local, global, member, parameter, or expression.
    std::string kind;
    // The type as Luau prints it, and a function's parts as LuauSuggestion has
    // them. For a member, `described.owner` is its class or library.
    LuauSuggestion described;
    // The registered class of the value, and the instance it is, when Luau
    // knows. The place's instances each have their own type. `raw_class` is
    // the value's own class, before walking up to a registered one, such as a
    // signal's type.
    std::string class_name;
    std::string raw_class;
    // The value is this global library function, as `local make = Instance.new`
    // is Instance.new.
    std::string function_owner;
    std::string function_name;
    bool instance_known = false;
    InstanceId instance = 0;
    // For a member, the instance whose member it is, as FindFirstChild's receiver,
    // and the object's name when it is written as one, such as `module`.
    bool object_instance_known = false;
    InstanceId object_instance = 0;
    std::string object_name;
    std::string error;
};

// What Luau says about one buffer, from one check: the completion at the
// caret, and the type at each other offset asked about.
struct LuauFacts {
    bool ran = false;
    std::string error;
    LuauCompletion completion;
    std::vector<LuauTypeAt> types;
};

// A Luau answer on its way from the editor checker. `ready` turns true once,
// after the checker has written `facts`; read them only then. A request a newer
// one in its lane replaced is ready with nothing in it.
struct LuauAnswer {
    std::atomic<bool> ready{false};
    LuauFacts facts;
};

// Incremental analysis of every Lua source in one DataModel, whether or not anything shows it.
// pump() copies the tree, sources and all, under the DataModel lock, once for
// every change since its last copy. A coordinator thread takes due
// scripts in batches and parses, lints, and type-checks them on a pool of
// threads; a second thread answers Luau requests from editors. pump() is the
// only publisher. It runs on the gameplay thread, as
// DataModel::gameplay_thread counts it (the simulation thread, or the thread
// running a paused edit), or on the UI thread. Never on
// RenderThread, and never inside lua_resume or Prepare.
//
// The place is checked in Edit mode only. While the simulation runs nothing is
// captured or checked: Play cancels a running batch, and changes queue and wait.
// Each script keeps its last result from Edit mode. After Stop the next pump()
// captures the restored tree, and the place checker diffs it against the one
// it last checked and redoes what Play cancelled.
class ScriptAnalysis {
public:
    // `threads` type-check the place at once; 0 means one fewer than the hardware has, and at least one.
    explicit ScriptAnalysis(DataModel& game, unsigned threads = 0);
    ~ScriptAnalysis();

    ScriptAnalysis(const ScriptAnalysis&) = delete;
    ScriptAnalysis& operator=(const ScriptAnalysis&) = delete;

    void set_enabled(bool enabled);
    bool enabled() const;

    // Source, name, or parent changed. Also used after place restore. Cheap:
    // it queues the script and what requires it, if it is in the place, and
    // the next pump() captures the tree once for every script queued since.
    void invalidate(InstanceId script);
    void invalidate_all();
    // The tree changed around the scripts: a parent, a name, an order, a
    // destroy. A script's answer to FindFirstChild depends on that even when
    // its own source did not change. Cheap and safe to call often: the next
    // pump() takes one new snapshot of the tree, and the scripts whose last
    // check reached what changed are checked again.
    // Waits while the simulation runs; Stop restores the authored tree.
    void note_world_changed();
    // DataModel calls these, under its write lock, after simulation_running
    // changes. Play cancels the running batch. Stop marks the tree changed, so
    // the next pump() brings analysis up to date with the restored place.
    void note_play_started();
    void note_play_stopped();
    // The instance is gone. Drops its diagnostics; a result a running batch
    // still finishes for it is never published.
    void remove(InstanceId script);
    // Modules the place checker holds, as of its last batch: one per script it
    // has checked in the authored tree it last saw.
    std::size_t cached_modules() const;

    std::vector<Diagnostic> diagnostics() const;
    std::vector<Diagnostic> diagnostics(InstanceId script) const;
    std::vector<Diagnostic> get_diagnostics_for_line(InstanceId script, std::uint32_t line) const;

    // Source the published diagnostics were checked against.
    // Empty when this script has no published result yet.
    std::optional<std::string> analyzed_source(InstanceId script) const;

    // Captures the tree when a change waits for it, publishes finished checks,
    // and fires diagnostics_changed. Does not run analysis.
    void pump();

    // One row per diagnostic: name | severity | code | message | line.
    // The printed line is 1-based.
    void print_report(std::ostream& out) const;

    unsigned threads() const;
    // How many results pump() has published for this script. For tests.
    std::uint64_t checks(InstanceId script) const;
    // The instances the script's last published check typed an expression as,
    // sorted. A tree change at one of them, or among its children, rechecks it.
    std::vector<InstanceId> reached(InstanceId script) const;

    // A script is queued or waiting out the debounce, the place checker is in
    // a batch, or a tree change is waiting for pump() or the place checker.
    // While the simulation runs, queued work waits for Stop, and only a batch
    // Play cancelled and that is still finishing counts.
    bool busy() const;
    // busy() is false and pump() has published every finished check.
    bool idle() const;
    // This script has a published result, and no newer check of it is queued,
    // running, or waiting for pump(). A tree change pump() has not taken yet
    // counts as newer. A script outside the place is never checked: settled
    // with no result. While the simulation runs nothing is checked before Stop,
    // so a script is settled once no batch Play cancelled still holds it: with
    // its last result from Edit mode, which may be for an older source, or with
    // none, as a script a playtest added has. MCP reports a script with no
    // result for its current source as not checked then, rather than waiting.
    bool settled(InstanceId script) const;

    class DiagnosticsSignal {
    public:
        std::uint64_t connect(std::function<void(InstanceId)> handler);
        void disconnect(std::uint64_t token);

    private:
        friend class ScriptAnalysis;
        ScriptAnalysis* owner_ = nullptr;
    };

    DiagnosticsSignal& diagnostics_changed() { return signal_; }

    // One type check of `source` as the text of `script`, with `world` as the
    // place, that answers Luau's autocomplete at byte `caret` (none when it is
    // npos) and the type at each byte of `offsets`, in that order. It runs on
    // the editor checker, never behind a check of the place, and waits up to `wait`. The
    // buffer's types are dropped afterwards, so an unsaved edit never reaches
    // another script's diagnostics. Any thread.
    LuauFacts luau_facts(const std::vector<LuaNode>& world, InstanceId script, std::string source, std::size_t caret,
                         std::vector<std::size_t> offsets, std::chrono::milliseconds wait);
    // The same without waiting. Poll the answer's `ready`. A new request in
    // `lane` replaces one there that the editor checker has not started, so typing
    // never queues more than one per lane.
    std::shared_ptr<const LuauAnswer> luau_facts_later(const std::vector<LuaNode>& world, InstanceId script,
                                                       std::string source, std::size_t caret,
                                                       std::vector<std::size_t> offsets, const char* lane);

private:
    struct State;
    struct LuauRequest;
    // Queues a Luau request without waiting. Null when analysis has stopped.
    std::shared_ptr<LuauRequest> queue_luau(const std::vector<LuaNode>& world, InstanceId script, std::string source,
                                            std::size_t caret, const char* lane, std::vector<std::size_t> offsets);

    void ensure_threads();
    void shutdown();
    void run_editor();
    void run_place();
    // pump() captures the tree once for every script queued since the last
    // capture, and when `tree_changed`, lets the place checker diff it against
    // the last. Captures nothing while the simulation runs. Under the
    // DataModel lock.
    void capture_tree(bool tree_changed);
    void fire(const std::vector<InstanceId>& ids);
    // Queues the scripts among these that are in the place for the place
    // checker, which takes them once pump() has captured the tree. While the
    // simulation runs they wait for Stop; a script only the play tree has is
    // removed at Stop before anything takes it. Gameplay thread, or a thread
    // that holds the DataModel lock.
    void schedule(const std::vector<InstanceId>& ids);
    void replace_requires(InstanceId script, const std::vector<InstanceId>& targets);
    void forget_requires(InstanceId script);
    void collect_dependents(InstanceId id, std::vector<InstanceId>& out, std::unordered_set<InstanceId>& seen) const;

    DataModel& game_;
    std::unique_ptr<State> state_;
    DiagnosticsSignal signal_;
    unsigned threads_;
};

}  // namespace engine_core
