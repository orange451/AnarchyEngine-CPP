#pragma once

#include "LuaApi.hpp"
#include "types.hpp"

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
};

struct LuauCompletion {
    // False when the worker did not answer in time, or analysis cannot run.
    bool ran = false;
    // expression, statement, property, type, keyword, string, hot comment, or unknown.
    std::string context;
    std::vector<LuauSuggestion> items;
    std::string error;
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

    // Luau's own autocomplete for `source` as the text of `script`, at byte
    // `offset`, with `world` as the place. It runs on the analysis worker, ahead
    // of queued checks, and waits up to `wait` for the answer. The buffer's
    // types are dropped afterwards, so an unsaved edit never reaches another
    // script's diagnostics. Any thread.
    LuauCompletion luau_complete(const std::vector<LuaNode>& world, InstanceId script, std::string source,
                                 std::size_t offset, std::chrono::milliseconds wait);

private:
    struct State;

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
