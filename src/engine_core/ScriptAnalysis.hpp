#pragma once

#include "types.hpp"

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

// Default when a script has no `--!` hot comment on the first lines.
enum class TypeMode { NoCheck, NonStrict, Strict };

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

// Incremental analysis of every Lua source in one DataModel.
// Source is copied on the gameplay thread. A background worker parses, lints,
// and typechecks that copy. pump() is the only publisher. It runs on the
// simulation thread or the UI thread, never on RenderThread, and never inside
// lua_resume or Prepare.
class ScriptAnalysis {
public:
    explicit ScriptAnalysis(DataModel& model);
    ~ScriptAnalysis();

    ScriptAnalysis(const ScriptAnalysis&) = delete;
    ScriptAnalysis& operator=(const ScriptAnalysis&) = delete;

    void set_enabled(bool enabled);
    bool enabled() const;

    void set_default_mode(TypeMode mode);
    TypeMode default_mode() const;

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

    class DiagnosticsSignal {
    public:
        std::uint64_t connect(std::function<void(InstanceId)> handler);
        void disconnect(std::uint64_t token);

    private:
        friend class ScriptAnalysis;
        ScriptAnalysis* owner_ = nullptr;
    };

    DiagnosticsSignal& diagnostics_changed() { return signal_; }

private:
    struct State;

    void ensure_worker();
    void shutdown();
    void run();
    void fire(const std::vector<InstanceId>& ids);
    void replace_requires(InstanceId script, const std::vector<InstanceId>& targets);
    void forget_requires(InstanceId script);
    void collect_dependents(InstanceId id, std::vector<InstanceId>& out, std::unordered_set<InstanceId>& seen) const;

    DataModel& model_;
    std::unique_ptr<State> state_;
    DiagnosticsSignal signal_;
};

}  // namespace engine_core
