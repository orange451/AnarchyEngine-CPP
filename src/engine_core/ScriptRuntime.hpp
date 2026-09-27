#pragma once

#include "Events.hpp"
#include "LuaApi.hpp"
#include "RunService.hpp"
#include "ScriptHost.hpp"
#include "TableSnapshot.hpp"
#include "TaskScheduler.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace engine_core {

struct ScriptBindings;
struct InputRecord;
void open_host_libraries(lua_State* state);

// One Luau state for the play session. SimulationThread is the only caller of lua_*.
// task.wait sleeps on sim_clock, which advances by the Heartbeat dt.
class ScriptRuntime : public ScriptHost {
public:
    struct Watch {
        InstanceId id = 0;
        std::uint32_t world = 0;
        bool valid = false;
    };

    ScriptRuntime() = default;
    ~ScriptRuntime() override;

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    void attach(DataModel& game, TaskScheduler& scheduler);
    void detach();

    // sim_clock += dt, wake sleeps, resume ready threads. dt is the Heartbeat step.
    void heartbeat(double dt);

    double sim_clock() const { return sim_clock_; }
    const std::string& last_error() const { return last_error_; }
    bool vm_open() const { return open_; }

    // One console line. Print is a script print(). Error is a compile failure or an
    // uncaught resume error. Command is text submitted from the Lua command line.
    // A caught error stays inside the script.
    enum class OutputKind { Print, Error, Command };

    // One print argument. text is its tostring. table is set when the argument is a table,
    // copied when print ran, so the console can open it later.
    struct OutputValue {
        std::string text;
        std::shared_ptr<const TableSnapshot> table;
    };

    struct OutputLine {
        OutputKind kind = OutputKind::Print;
        std::string text;
        // One entry per argument when a print had a table among them. Empty otherwise.
        // text is still the whole line, tab separated, for readers that only want text.
        std::vector<OutputValue> values;
        // Wall clock when the line was recorded. The console shows this on each row.
        std::chrono::system_clock::time_point time{};
    };

    struct OutputBatch {
        // Bumps when the log is cleared. The console replaces its text when this changes.
        std::uint64_t epoch = 0;
        std::vector<OutputLine> lines;
    };

    // Drops queued lines. Play-session start does this before any script from that session runs.
    std::uint64_t clear_output();
    // Copies the text. A missing trailing newline is added. Records the wall time.
    // Safe from the simulation thread and from the thread that runs the command line.
    void append_output(OutputKind kind, std::string text);
    // A print line that carries its arguments. The text is capped as above; the values are not.
    void append_output(OutputKind kind, std::string text, std::vector<OutputValue> values);
    OutputBatch drain_output();

    // One chunk against the live data model. Works while the play session is closed.
    // The caller is the simulation thread, or a paused edit.
    // print and an uncaught error join the log. No Script instance is attached.
    void run_chunk(std::string_view source);

    bool global_is_nil(const char* name);
    bool global_number(const char* name, double& out);
    bool global_boolean(const char* name, bool& out);
    Watch watch_global(const char* name);
    // Null when the watch's world_generation is not the live one, or the id is dead.
    // A mismatched generation does not touch the instance slot.
    DataModel* resolve_watch(Watch watch) const;

    void on_script_parent(Script& script, InstanceId previous, InstanceId next) override;
    void on_script_enabled(Script& script, bool enabled) override;
    void on_script_destroyed(Script& script) override;
    void on_child_named(InstanceId parent, InstanceId child, const std::string& name) override;

private:
    friend struct ScriptBindings;
    friend void open_host_libraries(lua_State* state);
    friend void push_registered(lua_State* state, ScriptRuntime* runtime, const LuaSlot& slot, InstanceId id,
                                std::uint32_t world);

    struct Thread {
        lua_State* co = nullptr;
        int anchor = -1;
        InstanceId script = 0;
        std::uint32_t generation = 0;
        enum class Park { None, Sleep, Signal, Defer, Child } park = Park::None;
        double due = 0;
        // Park::Child: WaitForChild on `wait_parent` for `wait_name`. `due` is the
        // timeout, infinite without one. `wait_warn_at` is the infinite-yield notice.
        // `wait_found` is the child on_child_named matched, not yet delivered.
        InstanceId wait_parent = 0;
        InstanceId wait_found = 0;
        std::uint32_t wait_world = 0;
        std::string wait_name;
        double wait_warn_at = 0;
        bool wait_warned = false;
        int nargs = 0;
        bool dead = false;
    };

    struct Start {
        InstanceId id = 0;
        std::uint32_t generation = 0;
    };

    static constexpr std::uint64_t kScriptTimeout = 50000;
    static constexpr int kResumeBudget = 32;
    static constexpr std::size_t kMemoryLimit = 64 * 1024 * 1024;

    static Thread* thread_from(lua_State* state);
    static bool gate(InstanceId script, std::uint32_t generation, void* userdata);
    static void* allocate(void* userdata, void* pointer, std::size_t old_size, std::size_t new_size);
    static void* allocate_console(void* userdata, void* pointer, std::size_t old_size, std::size_t new_size);
    static void interrupt(lua_State* state, int gc);
    static void panic(lua_State* state, int code);
    static int lua_print(lua_State* state);

    void on_end_of_drain();
    void on_start();
    void on_stop();
    void assert_lua_thread() const;
    void open_vm();
    void close_vm();
    // The command line keeps its own state so game is available while the play VM is closed.
    lua_State* create_state(bool console);
    void ensure_console();
    void close_console();
    void refresh_game(lua_State* state);
    void eval_chunk(lua_State* state, std::string_view source);
    void kill_script(InstanceId id);
    void enqueue_start(Script& script);
    void launch_starts();
    void launch_one(const Start& start);
    void flush_defer();
    void wake_sleeps();
    // WaitForChild. deliver resumes threads on_child_named matched. The timer
    // pass handles timeouts and the notice, and runs only when one is due.
    void park_child_wait(Thread& thread);
    void forget_child_wait(Thread& thread);
    void drop_dead_child_waits();
    void deliver_child_waits();
    void wake_child_timers();
    void resume_budget();
    void resume_one(Thread& thread);
    void drop_dead(std::list<Thread*>& queue);
    void ready(Thread& thread);
    void make_ready(Thread& thread, const char* result);
    void make_ready_number(Thread& thread, double result);
    bool thread_ok(const Thread& thread) const;
    Thread& new_thread(InstanceId script, std::uint32_t generation);
    void set_script_global(lua_State* co, InstanceId script);
    void remember_error(lua_State* state);
    // remember_error plus a console line. require uses remember_error alone so a caught
    // failure is not logged twice when the caller resumes.
    void report_error(lua_State* state);
    void push_instance(lua_State* state, InstanceId id);
    DataModel* resolve_id(InstanceId id, std::uint32_t world) const;
    void fire_phase(Phase phase, double dt);
    void invoke_listener(int ref, InstanceId script, std::uint32_t generation, const char* text, bool pass_number,
                         double number);
    // An InputService signal: the listener gets an InputObject and gameProcessedEvent.
    void invoke_listener_input(int ref, InstanceId script, std::uint32_t generation, const InputRecord& record);
    // Null when the gate refuses. The listener is on the new thread's stack.
    Thread* start_listener(int ref, InstanceId script, std::uint32_t generation);
    void run_listener(Thread& thread);
    void make_ready_input(Thread& thread, const InputRecord& record);
    // The InputObject for the record being delivered, or null outside an InputService handler.
    const InputRecord* delivered_input() const;
    int require_module(lua_State* state, InstanceId module_id);

    DataModel* game_ = nullptr;
    TaskScheduler* scheduler_ = nullptr;
    lua_State* state_ = nullptr;
    lua_State* console_state_ = nullptr;
    bool open_ = false;
    bool closing_ = false;
    int lua_depth_ = 0;
    double sim_clock_ = 0;
    std::uint64_t steps_ = 0;
    std::size_t memory_used_ = 0;
    std::size_t console_memory_used_ = 0;
    std::string last_error_;

    // Guards output_ and output_epoch_. Never take the DataModel lock while holding this.
    std::mutex output_mu_;
    std::deque<OutputLine> output_;
    std::uint64_t output_epoch_ = 0;

    std::list<Thread> threads_;
    std::list<Thread*> ready_;
    std::list<Thread*> sleep_;
    std::list<Thread*> defer_;
    // WaitForChild threads keyed by the parent they wait on, so a reparent or
    // rename elsewhere is one lookup.
    std::unordered_map<InstanceId, std::vector<Thread*>> child_waits_;
    std::vector<Thread*> child_found_;
    double next_child_timer_ = std::numeric_limits<double>::infinity();
    std::vector<Start> starts_;
    std::unordered_map<InstanceId, int> require_cache_;
    // The command line's own modules, refs in console_state_. A VM cannot hold another VM's
    // values, so the console runs a ModuleScript itself. Cleared before each command, so a
    // module edited while stopped is read again.
    std::unordered_map<InstanceId, int> console_require_cache_;
    std::vector<InstanceId> loading_;

    // The phase signals scripts reach through game:GetService("RunService").
    RunService run_service_;
};

}  // namespace engine_core
