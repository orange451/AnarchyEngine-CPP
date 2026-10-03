#pragma once

#include "Events.hpp"
#include "LuaApi.hpp"
#include "RunService.hpp"
#include "ScriptHost.hpp"
#include "TableSnapshot.hpp"
#include "TaskScheduler.hpp"

#include <atomic>
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
#include <unordered_set>
#include <vector>

struct lua_State;

namespace engine_core {

struct ScriptBindings;
void open_host_libraries(lua_State* state);

// Three Luau states, each with its own scheduler: the play VM for the play session,
// the console VM for the command line, and the plugin VM for plugins. Every caller of
// lua_* holds the DataModel write lock: SimulationThread's step, a paused edit (the
// command line arrives as one, through Engine::on_simulation), or RenderThread inside
// the Prepare window, which enters only the play and plugin VMs (render_step).
// A play script's task.wait sleeps on sim_clock, which advances by the Heartbeat dt.
// The console and plugin VMs keep their own clocks, which step_tools advances, so
// their threads wait, and their connections fire, while the play session is closed too.
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

    // The console and plugin VMs' heartbeat: their clocks += dt, wake their sleeps,
    // resume their ready threads. While the play VM is closed there is no play step,
    // so this also dispatches UserInputService's queued input, drains its events, and
    // then fires RunService.Heartbeat and drains again, all before the tool VMs resume.
    // While a play session is paused it does none of that: their waits keep time, but
    // signals wait for the session to resume, as the play VM's do. The engine calls it after heartbeat
    // while stepping, and on its own while paused, then only while a tool VM is open.
    void step_tools(double dt);
    // Whether the console or plugin VM is open, so step_tools has work. Any thread may ask.
    bool tools_open() const { return tools_open_.load(std::memory_order_relaxed); }
    // Forgets the frames drawn since the last step, so the next RenderStepped does not
    // count them. Only the console's sim-side fallback accumulates frames this way;
    // play and plugin handlers run in the window directly and are unaffected. The
    // engine calls it on resume, for the frames drawn while paused. Any thread may
    // call it.
    void drop_render_frames() { run_service_.drop_frames(); }
    // Pauses window delivery for play handlers: a paused session's signals wait,
    // but plugins keep their render step. The engine calls it from pause and
    // resume on whatever thread holds pause_mu_, so it is atomic.
    void set_render_paused(bool paused) { render_paused_.store(paused, std::memory_order_relaxed); }
    bool render_paused() const { return render_paused_.load(std::memory_order_relaxed); }

    double sim_clock() const { return play_.clock; }
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
        // For a print, the Script or ModuleScript whose code called it and the line it
        // was on. 0 when a command, the console, or the host printed the line.
        InstanceId script = 0;
        int line = 0;
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
    void append_output(OutputKind kind, std::string text, std::vector<OutputValue> values, InstanceId script = 0,
                       int line = 0);
    OutputBatch drain_output();

    // Every line also goes to a history that readers other than the console
    // share, such as the MCP server. Reading it takes nothing from the console.
    // Each line gets the next sequence number; the oldest lines drop past a cap.
    struct OutputHistory {
        // The sequence number of lines.front(), and the one the next line will get.
        std::uint64_t first = 0;
        std::uint64_t next = 0;
        std::vector<OutputLine> lines;
    };
    // Lines numbered since and later, at most limit of them.
    OutputHistory output_since(std::uint64_t since, std::size_t limit) const;
    // The sequence number the next line will get.
    std::uint64_t output_next() const;

    // One chunk against the live data model, in the console VM. Works while the play
    // session is closed. The caller is the simulation thread, or a paused edit.
    // It runs now until it finishes or yields. A yielded command, and whatever it
    // spawned or connected, goes on with step_tools, across play sessions, until it
    // ends or reset_console. print and an uncaught error join the log. No Script
    // instance is attached.
    void run_chunk(std::string_view source);
    // Closes the console VM: its threads stop and its connections go. The next
    // command opens a fresh one.
    void reset_console();

    // A plugin is an instance whose Scripts run in the plugin VM, apart from the play VM
    // and the command line: while the play session is closed too, and wherever the
    // instance sits, parented or not. Registering runs the root when it is a Script,
    // then every Script under it, depth first in sibling order, each until it finishes
    // or yields. A disabled Script is skipped, and a ModuleScript runs only through
    // require. A plugin Script may wait, spawn, and connect; its threads go on with
    // step_tools until it is destroyed or the plugin is unregistered. print and errors
    // join the log. Plugins share the plugin VM's _G and shared. It closes when the
    // last plugin goes. The caller is the simulation thread, or a paused edit. False
    // when root is not a live instance or is registered already; nothing runs then.
    bool register_plugin(InstanceId root);
    // Selection.SelectionChanged, RunService.Started, and RunService.Stopped.
    // Null before attach.
    Signal* host_signal(HostSignal which);
    // Scripts in Core run in the plugin VM, as plugins: one that enters Core
    // while Enabled starts, and one that leaves, is disabled, or is destroyed
    // stops. The script host's hooks queue them, and this registers or
    // unregisters what is queued. step_tools calls it first, and tools_open()
    // is true while something waits. SimulationThread, outside any Lua call.
    void start_core_scripts();
    // Stops the plugin's threads and connections. False when root is not registered.
    bool unregister_plugin(InstanceId root);
    bool is_plugin(InstanceId root) const;
    // Registered roots in the order they were registered. A root that died drops out.
    std::vector<InstanceId> plugins() const;
    bool plugin_vm_open() const { return plugin_.state != nullptr; }

    bool global_is_nil(const char* name);
    bool global_number(const char* name, double& out);
    bool global_boolean(const char* name, bool& out);
    Watch watch_global(const char* name);
    // Null when the watch's world_generation is not the live one, or the id is dead.
    // A mismatched generation does not touch the instance slot.
    DataModel* resolve_watch(Watch watch) const;

    void on_moved(InstanceId id) override;
    void on_script_enabled(Script& script, bool enabled) override;
    void on_script_destroyed(Script& script) override;
    void on_child_named(InstanceId parent, InstanceId child, const std::string& name) override;

private:
    friend struct ScriptBindings;
    friend void open_host_libraries(lua_State* state);
    friend void push_registered(lua_State* state, ScriptRuntime* runtime, const LuaSlot& slot, InstanceId id,
                                std::uint32_t world);

    struct Vm;

    struct Thread {
        // The VM whose scheduler runs this thread.
        Vm* vm = nullptr;
        lua_State* co = nullptr;
        // The registry reference that keeps co alive until the thread is released.
        // -1 is LUA_NOREF, none.
        int anchor = -1;
        // What the coroutine's thread data and task handles hold instead of a pointer.
        // Lua can keep the coroutine, or a handle, after this thread is released, and
        // can even resume a killed coroutine. A released serial finds no thread.
        // Serials are unique across the VMs.
        std::uint64_t serial = 0;
        // Who owns the thread, which owner_ok checks. In the play VM, a Script and its
        // start_generation, or 0 for none. In the console VM, 0 and 0. In the plugin VM,
        // the plugin Script and its registration's serial.
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

    // A console or plugin connection. It is kept across play sessions, so the VM
    // disconnects it itself. script and owner are those of the thread that made it.
    struct Kept {
        InstanceId script = 0;
        std::uint32_t owner = 0;
        Connection connection;
    };

    enum class VmKind { Play, Console, Plugin };

    // One Luau state and its scheduler.
    struct Vm {
        explicit Vm(VmKind vm_kind) : kind(vm_kind) {}
        const VmKind kind;
        lua_State* state = nullptr;
        std::size_t memory_used = 0;
        double clock = 0;
        bool closing = false;
        // Play only: set by halt. No script starts again until the next play session.
        bool halted = false;
        std::list<Thread> threads;
        std::list<Thread*> ready;
        std::list<Thread*> sleep;
        std::list<Thread*> defer;
        // WaitForChild threads keyed by the parent they wait on, so a reparent or
        // rename elsewhere is one lookup.
        std::unordered_map<InstanceId, std::vector<Thread*>> child_waits;
        std::vector<Thread*> child_found;
        double next_child_timer = std::numeric_limits<double>::infinity();
        // ModuleScript results, registry refs in state. The play VM keeps them for the
        // session. The console clears them before each command and the plugin VM before
        // each registration, so a module edited since is read again.
        std::unordered_map<InstanceId, int> require_cache;
        // Lives as long as state. A HeldRef whose VM has closed releases nothing.
        std::shared_ptr<void> token;
        std::vector<Kept> kept;
        // kept drops disconnected entries when it reaches this size.
        std::size_t kept_prune_at = 64;
    };

    struct Plugin {
        InstanceId root = 0;
        std::uint32_t serial = 0;
    };

    // Interrupts, loop back-edges and calls, one resume may take before it is
    // stopped as a runaway. The same budget LuaEngine gives a chunk.
    static constexpr std::uint64_t kScriptTimeout = 1000000;
    static constexpr int kResumeBudget = 32;
    static constexpr std::size_t kMemoryLimit = 64 * 1024 * 1024;
    // The world a console or plugin handle carries. It resolves by id alone, so the
    // handle outlives a play session; the id's slot generation still tells a dead one.
    static constexpr std::uint32_t kAnyWorld = 0xffffffffu;

    // A registry reference C++ holds on a VM, such as a Connect callback.
    struct HeldRef;

    // Null when state is not a script thread, or its thread was released.
    static Thread* thread_from(lua_State* state);
    Thread* find_thread(std::uint64_t serial) const;
    // The VM state belongs to. Null for none of them.
    Vm* vm_from(lua_State* state);
    // The play VM's EventQueue gate for tagged connections.
    static bool gate(InstanceId script, std::uint32_t generation, void* userdata);
    // Whether a thread owned by script and generation may run in vm. See Thread.
    bool owner_ok(const Vm& vm, InstanceId script, std::uint32_t generation) const;
    static void* allocate(void* userdata, void* pointer, std::size_t old_size, std::size_t new_size);
    static void interrupt(lua_State* state, int gc);
    static int lua_print(lua_State* state);
    void print_source(lua_State* state, InstanceId& script, int& line) const;

    void on_end_of_drain();
    void on_start();
    void on_stop();
    void assert_lua_thread() const;
    void open_vm();
    void close_vm();
    // The console and plugin VMs open on first use.
    void ensure_state(Vm& vm);
    lua_State* create_state(Vm& vm);
    // Closes vm's state: its threads, modules, and kept connections go with it.
    void close_state(Vm& vm);
    void update_tools_open();
    // Queues every Script in id's subtree that start_core_scripts must look at.
    void note_core(InstanceId id);
    void refresh_game(lua_State* state);
    // game and workspace. The global table must be writable.
    void set_root_globals(lua_State* state);
    // Loads source on a new thread of vm owned by script and generation, with `script`
    // set to script, and runs it now until it finishes or yields. chunk names it in errors.
    void launch_chunk(Vm& vm, std::string_view source, const std::string& chunk, InstanceId script,
                      std::uint32_t generation);
    void run_plugin(const Plugin& plugin);
    // Stops vm's threads and kept connections that match. owner 0 matches any owner.
    void kill_owned(Vm& vm, InstanceId script, std::uint32_t owner);
    // Records a console or plugin connection so its VM can disconnect it.
    void keep(Vm& vm, InstanceId script, std::uint32_t owner, const Connection& connection);
    void kill_script(InstanceId id);
    // Whether a script at id is under Workspace, Scripts, or Gui, where scripts run.
    bool runs_here(InstanceId id) const;
    void enqueue_start(Script& script);
    void launch_starts();
    void launch_one(const Start& start);
    // One scheduler pass over a console or plugin VM: timers, then the ready threads.
    void step_side(Vm& vm, double dt);
    void flush_defer(Vm& vm);
    void wake_sleeps(Vm& vm);
    // WaitForChild. deliver resumes threads on_child_named matched. The timer
    // pass handles timeouts and the notice, and runs only when one is due.
    void park_child_wait(Thread& thread);
    void forget_child_wait(Thread& thread);
    void drop_dead_child_waits(Vm& vm);
    void deliver_child_waits(Vm& vm);
    void wake_child_timers(Vm& vm);
    void resume_budget(Vm& vm);
    void resume_one(Thread& thread);
    void drop_dead(std::list<Thread*>& queue);
    void drop_dead_queues(Vm& vm);
    // Frees threads that finished or were killed, and lets the collector have
    // their coroutines. Does nothing while Lua is on the stack.
    void release_dead_threads(Vm& vm);
    // Luau throws when memory runs out in a call C++ makes outside lua_resume or
    // lua_pcall. The runtime's own work then stops every script in that VM and says why.
    template <typename Fn>
    void guarded(Vm& vm, Fn&& fn);
    void halt(Vm& vm, const char* why);
    void ready(Thread& thread);
    // Takes a thread off every wait before it is made ready. False when it is
    // dead or has no coroutine.
    bool unpark(Thread& thread);
    void make_ready(Thread& thread, const char* result);
    void make_ready_number(Thread& thread, double result);
    // Resumes a Wait with an event's values as its results. Null resumes it with none.
    void make_ready_args(Thread& thread, const EventArgs* args);
    // Pushes dt and resumes the thread now. Window Wait handlers use it: in the
    // window there is no later drain to resume a ready thread, so the resume is
    // the delivery.
    void resume_waiting_now(Thread& thread, double dt);
    bool thread_ok(const Thread& thread) const;
    Thread& new_thread(Vm& vm, InstanceId script, std::uint32_t generation);
    void set_script_global(lua_State* co, InstanceId script);
    void remember_error(lua_State* state);
    // remember_error plus a console line. require uses remember_error alone so a caught
    // failure is not logged twice when the caller resumes.
    void report_error(lua_State* state);
    void push_instance(lua_State* state, InstanceId id);
    DataModel* resolve_id(InstanceId id, std::uint32_t world) const;
    void fire_phase(Phase phase, double dt);
    void invoke_listener(Vm& vm, int ref, InstanceId script, std::uint32_t generation, const char* text,
                         bool pass_number, double number);
    // An event with values: the listener gets each one, in order. Null gets none.
    void invoke_listener_args(Vm& vm, int ref, InstanceId script, std::uint32_t generation, const EventArgs* args);
    // Null when the owner may not run. The listener is on the new thread's stack.
    Thread* start_listener(Vm& vm, int ref, InstanceId script, std::uint32_t generation);
    void run_listener(Thread& thread);
    int require_module(lua_State* state, InstanceId module_id);

    DataModel* game_ = nullptr;
    TaskScheduler* scheduler_ = nullptr;
    // The phase jobs attach binds, which detach unbinds.
    std::vector<TaskScheduler::JobId> phase_jobs_;
    Vm play_{VmKind::Play};
    Vm console_{VmKind::Console};
    Vm plugin_{VmKind::Plugin};
    std::atomic<bool> tools_open_{false};
    // The play VM is open: a play session is running.
    bool open_ = false;
    // Lua frames on the C++ stack, any VM. Threads are released only at 0.
    int lua_depth_ = 0;
    // The render job that runs window handlers. RenderThread, inside Prepare.
    void render_step(double dt);
    // True while render_step invokes handlers. Bindings branch on it: a Wait on
    // the window signal resumes here, and a refused write raises instead of
    // deferring silently.
    bool in_render_window_ = false;
    std::atomic<bool> render_paused_{false};
    std::uint64_t steps_ = 0;
    std::string last_error_;

    // Guards output_, output_epoch_, and the history. Never take the DataModel lock while holding this.
    mutable std::mutex output_mu_;
    std::deque<OutputLine> output_;
    std::uint64_t output_epoch_ = 0;
    std::deque<OutputLine> history_;
    std::uint64_t history_next_ = 0;

    std::unordered_map<std::uint64_t, Thread*> by_serial_;
    std::uint64_t next_serial_ = 0;
    std::vector<Start> starts_;
    // Scripts started this session, queued or running. Moving one does not start it again.
    std::unordered_set<InstanceId> started_;
    std::vector<Plugin> plugins_;
    // Scripts in Core waiting for start_core_scripts, and those it registered.
    std::vector<InstanceId> core_pending_;
    std::unordered_set<InstanceId> core_scripts_;
    std::uint32_t plugin_serial_ = 0;
    std::vector<InstanceId> loading_;

    // The phase signals scripts reach through game:GetService("RunService").
    RunService run_service_;
    // Selection.SelectionChanged. The selection changes on any thread, so
    // step_tools fires it, and Started and Stopped, for what changed since
    // the step before.
    Signal selection_changed_;
    std::uint64_t selection_revision_ = 0;
    bool was_running_ = false;
    // ChangeHistoryService's OnUndo, OnRedo, OnRecordingStarted, and
    // OnRecordingFinished, fired from its C++ signals of the same names.
    Signal history_undo_;
    Signal history_redo_;
    Signal history_started_;
    Signal history_finished_;
    std::uint64_t history_links_[4] = {0, 0, 0, 0};
    void fire_host_changes();
};

}  // namespace engine_core
