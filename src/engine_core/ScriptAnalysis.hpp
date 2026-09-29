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

// Which scripts are analyzed. All: every Script and ModuleScript. Open: the
// watched scripts and every ModuleScript they require, recursively, because a
// required module's types are part of the watched script's check.
enum class AnalysisScope { All, Open };

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
    // False when the worker did not answer in time, or analysis cannot run.
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

// A Luau answer on its way from the analysis worker. `ready` turns true once,
// after the worker has written `facts`; read them only then. A request a newer
// one in its lane replaced is ready with nothing in it.
struct LuauAnswer {
    std::atomic<bool> ready{false};
    LuauFacts facts;
};

// Incremental analysis of every Lua source in one DataModel.
// Source is copied on the gameplay thread. A background worker parses, lints,
// and typechecks that copy. pump() is the only publisher. It runs on the
// gameplay thread, as DataModel::gameplay_thread counts it (the simulation
// thread, or the thread running a paused edit), or on the UI thread. Never on
// RenderThread, and never inside lua_resume or Prepare.
class ScriptAnalysis {
public:
    explicit ScriptAnalysis(DataModel& game);
    ~ScriptAnalysis();

    ScriptAnalysis(const ScriptAnalysis&) = delete;
    ScriptAnalysis& operator=(const ScriptAnalysis&) = delete;

    void set_enabled(bool enabled);
    bool enabled() const;

    // All is the default. Switching to Open drops every result outside the
    // watched scripts and their required modules.
    void set_scope(AnalysisScope scope);
    AnalysisScope scope() const;
    // An editor is showing this script. It is checked against the current tree
    // on the next pump(), whatever result it had. Counted: each watch needs an unwatch.
    void watch(InstanceId script);
    // In Open scope, the last unwatch drops this script's result, and the
    // result of every module no other watched script still requires.
    void unwatch(InstanceId script);

    // Source, name, or parent changed. Also used after place restore.
    void invalidate(InstanceId script);
    void invalidate_all();
    // The tree changed around the scripts: a parent, a name, an order, a
    // destroy. A script's answer to FindFirstChild depends on that even when
    // its own source did not change. Cheap and safe to call often: the next
    // pump() rechecks every script against one new snapshot of the tree.
    // Waits while the simulation runs; Stop restores the authored tree.
    void note_world_changed();
    // The instance is gone. Drops its diagnostics and cancels its job.
    void remove(InstanceId script);
    // Modules the checker holds, as of its last job: one per script it has
    // checked in the tree it last saw.
    std::size_t cached_modules() const;

    std::vector<Diagnostic> diagnostics() const;
    std::vector<Diagnostic> diagnostics(InstanceId script) const;
    std::vector<Diagnostic> get_diagnostics_for_line(InstanceId script, std::uint32_t line) const;

    // Source the published diagnostics were checked against.
    // Empty when this script has no published result yet.
    std::optional<std::string> analyzed_source(InstanceId script) const;

    // Applies finished jobs and fires diagnostics_changed. Does not run analysis.
    void pump();

    // One row per diagnostic: name | severity | code | message | line.
    // The printed line is 1-based.
    void print_report(std::ostream& out) const;

    // A snapshot is waiting out the debounce, the worker is inside a job, or a
    // tree change is waiting for pump().
    bool busy() const;
    // busy() is false and pump() has published every finished job.
    bool idle() const;
    // This script has a published result, and no newer check of it is queued,
    // running, or waiting for pump(). A tree change pump() has not taken yet
    // counts as newer while the simulation is stopped.
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
    // the analysis worker, ahead of queued checks, and waits up to `wait`. The
    // buffer's types are dropped afterwards, so an unsaved edit never reaches
    // another script's diagnostics. Any thread.
    LuauFacts luau_facts(const std::vector<LuaNode>& world, InstanceId script, std::string source, std::size_t caret,
                         std::vector<std::size_t> offsets, std::chrono::milliseconds wait);
    // The same without waiting. Poll the answer's `ready`. A new request in
    // `lane` replaces one there that the worker has not started, so typing
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

    void ensure_worker();
    void shutdown();
    void run();
    void fire(const std::vector<InstanceId>& ids);
    // Captures the tree once and queues these scripts. Gameplay thread, or a
    // thread that holds the DataModel lock.
    void schedule(const std::vector<InstanceId>& ids);
    // Open scope with the state mutex held: the watched scripts and every
    // script they reach through recorded requires.
    std::unordered_set<InstanceId> active_locked() const;
    // Open scope with the state mutex held: forgets every script outside
    // active_locked(). Returns the ones that had a published result.
    std::vector<InstanceId> drop_inactive_locked();
    void replace_requires(InstanceId script, const std::vector<InstanceId>& targets);
    void forget_requires(InstanceId script);
    void collect_dependents(InstanceId id, std::vector<InstanceId>& out, std::unordered_set<InstanceId>& seen) const;

    DataModel& game_;
    std::unique_ptr<State> state_;
    DiagnosticsSignal signal_;
};

}  // namespace engine_core
