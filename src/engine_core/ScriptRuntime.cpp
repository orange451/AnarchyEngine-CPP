#include "ScriptRuntime.hpp"

#include "ScriptBindings.hpp"

#include "ChangeHistoryService.hpp"
#include "Contract.hpp"
#include "Enum.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "LuaUserdata.hpp"
#include "LuauSandbox.hpp"
#include "Matrix4.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"
#include "SelectionService.hpp"
#include "UserInputService.hpp"
#include "Vector2.hpp"
#include "Vector3.hpp"

#include "lualib.h"
#include "luacode.h"

// halt() and guarded() catch the exception Luau throws when memory runs out
// outside lua_resume. A longjmp build would abort instead.
#if LUA_USE_LONGJMP
#error "ScriptRuntime expects Luau built with C++ exceptions"
#endif

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace engine_core {

void push_registered(lua_State* state, ScriptRuntime* runtime, const LuaSlot& slot, InstanceId id, std::uint32_t world);

namespace {

// Pushes an event's values in order and says how many. The values are copied
// onto the stack, so the event may go once this returns.
int push_event_args(lua_State* state, ScriptRuntime* runtime, const EventArgs* args) {
    if (args == nullptr || args->empty()) {
        return 0;
    }
    const int count = static_cast<int>(args->size());
    lua_checkstack(state, count);
    for (const LuaSlot& slot : *args) {
        push_registered(state, runtime, slot, 0, 0);
    }
    return count;
}

void clear_require_cache(lua_State* state, std::unordered_map<InstanceId, int>& cache) {
    for (const auto& entry : cache) {
        if (entry.second != LUA_REFNIL) {
            lua_unref(state, entry.second);
        }
    }
    cache.clear();
}

}  // namespace

ScriptRuntime::~ScriptRuntime() { detach(); }

void ScriptRuntime::halt(Vm& vm, const char* why) {
    for (Thread& thread : vm.threads) {
        thread.dead = true;
    }
    vm.ready.clear();
    vm.sleep.clear();
    vm.defer.clear();
    vm.child_waits.clear();
    vm.child_found.clear();
    vm.next_child_timer = std::numeric_limits<double>::infinity();
    const char* who = "Scripts";
    if (vm.kind == VmKind::Play) {
        if (game_ != nullptr) {
            game_->events().disconnect_scripted();
        }
        starts_.clear();
        vm.halted = true;
    } else {
        // The console and plugins stop what runs now. A later command or plugin starts again.
        for (Kept& kept : vm.kept) {
            kept.connection.disconnect();
        }
        vm.kept.clear();
        who = vm.kind == VmKind::Console ? "Console" : "Plugins";
    }
    if (vm.state != nullptr) {
        // A throw can leave what it was pushing on the main thread's stack.
        lua_settop(vm.state, 0);
    }
    last_error_ = std::string(who) + " stopped: " + (why != nullptr ? why : "Luau failed");
    append_output(OutputKind::Error, last_error_);
}

void ScriptRuntime::attach(DataModel& game, TaskScheduler& scheduler) {
    if (game_ != nullptr) {
        return;
    }
    game_ = &game;
    scheduler_ = &scheduler;
    scheduler.reserve(8);
    game.set_stop_hook([this] { on_stop(); });
    game.set_start_hook([this] { on_start(); });
    game.set_script_host(this);
    game.events().set_after_drain([this] { on_end_of_drain(); });
    game.events().set_script_gate(&ScriptRuntime::gate, this);
    run_service_.bind(game.events());
    game.events().host_signal(&selection_changed_);
    selection_revision_ = game.selection().revision();
    was_running_ = game.simulation_running();
    game.input().bind(game.events());
    // Input is kept whenever a runtime is attached: plugins hear it in edit mode.
    game.input().set_active(true);
    for (Phase phase : {Phase::PreAnimation, Phase::PreSimulation, Phase::PostSimulation, Phase::Heartbeat}) {
        phase_jobs_.push_back(scheduler.bind(phase, [this, phase](double dt) { fire_phase(phase, dt); }));
    }
    // On RenderThread this runs window handlers and counts the frame for the
    // console fallback. Scripts in the window hold the same write lock the sim
    // step holds, so the two never run Lua at once.
    phase_jobs_.push_back(scheduler.bind(Phase::RenderStepped, [this](double dt) { render_step(dt); }));
}

void ScriptRuntime::detach() {
    close_state(console_);
    close_state(plugin_);
    plugins_.clear();
    if (game_ != nullptr) {
        game_->events().disconnect_scripted();
    }
    close_vm();
    if (game_ != nullptr) {
        run_service_.release(game_->events());
        game_->events().release_signal(selection_changed_);
        game_->input().release(game_->events());
        game_->input().set_active(false);
        game_->set_stop_hook(nullptr);
        game_->set_start_hook(nullptr);
        game_->set_script_host(nullptr);
        game_->events().set_after_drain(nullptr);
        game_->events().set_script_gate(nullptr, nullptr);
        game_ = nullptr;
    }
    if (scheduler_ != nullptr) {
        for (TaskScheduler::JobId id : phase_jobs_) {
            scheduler_->unbind(id);
        }
    }
    phase_jobs_.clear();
    scheduler_ = nullptr;
}

void ScriptRuntime::heartbeat(double dt) {
    if (!open_ || play_.closing) {
        return;
    }
    assert_lua_thread();
    if (dt < 0) {
        dt = 0;
    }
    play_.clock += dt;
    guarded(play_, [&] {
        wake_sleeps(play_);
        deliver_child_waits(play_);
        wake_child_timers(play_);
        launch_starts();
        flush_defer(play_);
        resume_budget(play_);
        if (!starts_.empty()) {
            launch_starts();
            resume_budget(play_);
        }
    });
    release_dead_threads(play_);
}

void ScriptRuntime::step_tools(double dt) {
    if (game_ == nullptr) {
        return;
    }
    start_core_scripts();
    fire_host_changes();
    // While the play VM is closed no play step fires Heartbeat or drains, so this does.
    // During play, and while a play session is paused, the play step's own do that.
    const bool own_step = !open_;
    const bool tools_open = console_.state != nullptr || plugin_.state != nullptr;
    if (!own_step && !tools_open) {
        return;
    }
    assert_lua_thread();
    if (dt < 0) {
        dt = 0;
    }
    if (own_step) {
        // Edit-mode input, dispatched before Heartbeat so a plugin's Heartbeat sees this
        // step's presses. Its events drain first. A caller that steps with no tool VM
        // open, such as the running engine loop or a test, still empties the queue here.
        // The paused engine loop, which is how edit mode runs, only calls this while a
        // tool VM is open, so there the queue waits, capped, until one opens.
        game_->input().dispatch(game_->events());
        if (!tools_open) {
            run_service_.drop_frames();
            game_->events().drain();
            return;
        }
        // Roblox order: input, RenderStepped, then the step.
        run_service_.fire_render_stepped(game_->events());
        run_service_.fire(game_->events(), Phase::Heartbeat, dt);
        game_->events().drain();
    }
    step_side(console_, dt);
    step_side(plugin_, dt);
    if (own_step) {
        game_->events().drain();
    }
}

void ScriptRuntime::step_side(Vm& vm, double dt) {
    if (vm.state == nullptr || vm.closing) {
        return;
    }
    vm.clock += dt;
    guarded(vm, [&] {
        wake_sleeps(vm);
        deliver_child_waits(vm);
        wake_child_timers(vm);
        flush_defer(vm);
        resume_budget(vm);
    });
    release_dead_threads(vm);
}

bool ScriptRuntime::global_is_nil(const char* name) {
    bool nil = true;
    with_global(play_.state, name, [&](lua_State* state) { nil = lua_isnil(state, -1); });
    return nil;
}

bool ScriptRuntime::global_number(const char* name, double& out) {
    bool ok = false;
    with_global(play_.state, name, [&](lua_State* state) {
        ok = lua_isnumber(state, -1);
        if (ok) {
            out = lua_tonumber(state, -1);
        }
    });
    return ok;
}

bool ScriptRuntime::global_boolean(const char* name, bool& out) {
    bool ok = false;
    with_global(play_.state, name, [&](lua_State* state) {
        ok = lua_isboolean(state, -1);
        if (ok) {
            out = lua_toboolean(state, -1) != 0;
        }
    });
    return ok;
}

ScriptRuntime::Watch ScriptRuntime::watch_global(const char* name) {
    Watch watch;
    with_global(play_.state, name, [&](lua_State* state) {
        if (auto* ud = static_cast<InstanceUd*>(test_userdata(state, -1, kInstanceMeta))) {
            watch.id = ud->id;
            watch.world = ud->world;
            watch.valid = true;
        }
    });
    return watch;
}

DataModel* ScriptRuntime::resolve_watch(Watch watch) const {
    if (!watch.valid) {
        return nullptr;
    }
    return resolve_id(watch.id, watch.world);
}

void ScriptRuntime::on_moved(InstanceId id) {
    // Core's Scripts run whether or not the place plays. Nothing moves out of
    // Core but the children of an instance destroyed there, which are left with
    // no parent, so a move anywhere else costs no walk.
    if (game_ != nullptr && (game_->core_holds(id) ||
                             (!core_scripts_.empty() && game_->parent(id) == DataModel::kNoParent))) {
        note_core(id);
    }
    if (game_ == nullptr || !game_->simulation_running() || play_.closing) {
        return;
    }
    // A script that leaves Workspace, Scripts, and Gui stops. One that arrives
    // starts from the top, unless it already runs this session: a move between
    // them does not run it again.
    std::vector<Script*> scripts;
    std::vector<InstanceId> pending{id};
    while (!pending.empty()) {
        const InstanceId next = pending.back();
        pending.pop_back();
        if (auto* script = dynamic_cast<Script*>(game_->instance(next))) {
            scripts.push_back(script);
        }
        for (InstanceId child = game_->first_child(next); child != 0; child = game_->next_sibling(child)) {
            pending.push_back(child);
        }
    }
    for (Script* script : scripts) {
        if (!runs_here(script->id())) {
            kill_script(script->id());
        } else if (started_.count(script->id()) == 0) {
            enqueue_start(*script);
        }
    }
}

bool ScriptRuntime::runs_here(InstanceId id) const {
    // The ancestor just under game decides.
    InstanceId top = id;
    for (InstanceId up = game_->parent(top); up != 0; up = game_->parent(top)) {
        if (up == DataModel::kNoParent) {
            return false;
        }
        top = up;
    }
    const DataModel* service = game_->instance(top);
    if (service == nullptr || !service->is_scene_service()) {
        return false;
    }
    const std::string_view name = service->class_name();
    return name == "Workspace" || name == "Scripts" || name == "Gui";
}

void ScriptRuntime::on_script_enabled(Script& script, bool enabled) {
    if (game_ != nullptr && (game_->core_holds(script.id()) || core_scripts_.count(script.id()) != 0)) {
        note_core(script.id());
        return;
    }
    if (game_ == nullptr || !game_->simulation_running() || play_.closing) {
        return;
    }
    kill_script(script.id());
    if (enabled) {
        enqueue_start(script);
    }
}

void ScriptRuntime::on_script_destroyed(Script& script) {
    core_scripts_.erase(script.id());
    if (!play_.closing) {
        kill_script(script.id());
    }
    // A plugin Script that is destroyed stops, with the modules it required.
    if (plugin_.state != nullptr && !plugin_.closing) {
        kill_owned(plugin_, script.id(), 0);
    }
}

ScriptRuntime::Thread* ScriptRuntime::thread_from(lua_State* state) {
    const ScriptRuntime* runtime = runtime_from(state);
    return runtime != nullptr ? runtime->find_thread(data_serial(lua_getthreaddata(state))) : nullptr;
}

ScriptRuntime::Thread* ScriptRuntime::find_thread(std::uint64_t serial) const {
    const auto found = by_serial_.find(serial);
    return found != by_serial_.end() ? found->second : nullptr;
}

ScriptRuntime::Vm* ScriptRuntime::vm_from(lua_State* state) {
    if (state == nullptr) {
        return nullptr;
    }
    lua_State* main = lua_mainthread(state);
    for (Vm* vm : {&play_, &console_, &plugin_}) {
        if (vm->state != nullptr && vm->state == main) {
            return vm;
        }
    }
    return nullptr;
}

bool ScriptRuntime::gate(InstanceId script, std::uint32_t generation, void* userdata) {
    auto* self = static_cast<ScriptRuntime*>(userdata);
    if (self == nullptr || self->game_ == nullptr || self->play_.closing || self->play_.halted) {
        return false;
    }
    auto* object = dynamic_cast<Script*>(self->game_->instance(script));
    if (object == nullptr || !object->enabled()) {
        return false;
    }
    return object->start_generation() == generation;
}

bool ScriptRuntime::owner_ok(const Vm& vm, InstanceId script, std::uint32_t generation) const {
    if (game_ == nullptr || vm.state == nullptr || vm.closing) {
        return false;
    }
    switch (vm.kind) {
    case VmKind::Play:
        return gate(script, generation, const_cast<ScriptRuntime*>(this));
    case VmKind::Console:
        return true;
    case VmKind::Plugin:
        if (script != 0 && !game_->alive(script)) {
            return false;
        }
        return std::any_of(plugins_.begin(), plugins_.end(),
                           [&](const Plugin& plugin) { return plugin.serial == generation; });
    }
    return false;
}

void* ScriptRuntime::allocate(void* userdata, void* pointer, std::size_t old_size, std::size_t new_size) {
    auto* vm = static_cast<Vm*>(userdata);
    return budget_realloc(vm->memory_used, kMemoryLimit, pointer, old_size, new_size);
}

void ScriptRuntime::interrupt(lua_State* state, int gc) {
    if (gc >= 0) {
        return;
    }
    auto* self = runtime_from(state);
    if (self == nullptr) {
        return;
    }
    const Vm* vm = self->vm_from(state);
    if (vm == nullptr || vm->closing) {
        return;
    }
    if (self->steps_ < kScriptTimeout) {
        ++self->steps_;
        return;
    }
    luaL_error(state, "ScriptTimeout");
}

void ScriptRuntime::on_end_of_drain() {
    if (open_ && !play_.closing) {
        assert_lua_thread();
        guarded(play_, [&] {
            launch_starts();
            deliver_child_waits(play_);
            flush_defer(play_);
            resume_budget(play_);
            if (!starts_.empty()) {
                launch_starts();
                resume_budget(play_);
            }
        });
        release_dead_threads(play_);
    }
    // A handler of the console or a plugin that waited on a signal resumes here.
    for (Vm* vm : {&console_, &plugin_}) {
        if (vm->state == nullptr || vm->closing) {
            continue;
        }
        assert_lua_thread();
        guarded(*vm, [&] {
            deliver_child_waits(*vm);
            flush_defer(*vm);
            resume_budget(*vm);
        });
        release_dead_threads(*vm);
    }
}

void ScriptRuntime::on_start() {
    // The previous session's lines are dropped before this session's scripts run.
    clear_output();
    open_vm();
    play_.clock = 0;
    started_.clear();
    if (game_ == nullptr) {
        return;
    }
    // Input from before Test, or from the last session, is not this session's.
    game_->input().reset();
    game_->input().set_active(true);
    std::vector<Script*> scripts;
    game_->for_each_instance([&](DataModel& instance) {
        if (auto* script = dynamic_cast<Script*>(&instance)) {
            scripts.push_back(script);
        }
    });
    for (Script* script : scripts) {
        enqueue_start(*script);
    }
}

void ScriptRuntime::on_stop() {
    if (game_ != nullptr) {
        game_->events().disconnect_scripted();
        // Still active, for the plugins; this drops what the session left queued.
        game_->input().set_active(true);
        game_->input().reset();
    }
    started_.clear();
    close_vm();
}

void ScriptRuntime::render_step(double dt) {
    run_service_.note_frame(dt);
    if (game_ == nullptr) {
        return;
    }
    Signal* window = run_service_.window_signal();
    if (!window->id().valid()) {
        return;
    }
    // A paused play session's handlers wait for resume; plugins keep stepping.
    const bool include_play = open_ && !play_.closing && !render_paused_.load(std::memory_order_relaxed);
    run_service_.set_window_dt(dt);
    in_render_window_ = true;
    game_->events().invoke_render(*window, include_play);
    in_render_window_ = false;
}

void ScriptRuntime::assert_lua_thread() const {
    // Lua runs wherever the DataModel write lock is held: the simulation step,
    // a paused edit, or the render thread inside the Prepare window.
    if (game_ != nullptr && game_->prerender_window() && thread_role() == ThreadRole::Render) {
        return;
    }
    if (thread_role() == ThreadRole::Render) {
        contract_fail("Lua runs on SimulationThread");
    }
    if (game_ != nullptr && !game_->on_gameplay_thread()) {
        contract_fail("Lua runs on SimulationThread");
    }
    if (game_ != nullptr && game_->prerender_window()) {
        contract_fail("Lua runs on SimulationThread");
    }
}

void open_host_libraries(lua_State* state) {
    open_sandbox_libraries(state, &ScriptRuntime::lua_print);

    const int top = lua_gettop(state);
    auto metatable = [&](const char* name) {
        luaL_newmetatable(state, name);
        return lua_gettop(state);
    };
    const int instance_mt = metatable(kInstanceMeta);
    lua_pushcfunction(state, &ScriptBindings::instance_index, "index");
    lua_setfield(state, instance_mt, "__index");
    lua_pushcfunction(state, &ScriptBindings::instance_newindex, "newindex");
    lua_setfield(state, instance_mt, "__newindex");
    lua_pushcfunction(state, &ScriptBindings::instance_tostring, "tostring");
    lua_setfield(state, instance_mt, "__tostring");
    // getmetatable(instance) gives this string instead of the table.
    lua_pushstring(state, "The metatable is locked");
    lua_setfield(state, instance_mt, "__metatable");
    lua_setreadonly(state, instance_mt, 1);

    const int signal_mt = metatable(kSignalMeta);
    lua_pushcfunction(state, &ScriptBindings::signal_index, "index");
    lua_setfield(state, signal_mt, "__index");
    lua_setreadonly(state, signal_mt, 1);

    const int connection_mt = metatable(kConnectionMeta);
    lua_pushcfunction(state, &ScriptBindings::connection_index, "index");
    lua_setfield(state, connection_mt, "__index");
    lua_setreadonly(state, connection_mt, 1);

    const int thread_mt = metatable(kThreadMeta);
    lua_pushcfunction(state, &ScriptBindings::thread_index, "index");
    lua_setfield(state, thread_mt, "__index");
    lua_setreadonly(state, thread_mt, 1);

    const int service_mt = metatable(kServiceMeta);
    lua_pushcfunction(state, &ScriptBindings::service_index, "index");
    lua_setfield(state, service_mt, "__index");
    lua_pushcfunction(state, &ScriptBindings::service_newindex, "newindex");
    lua_setfield(state, service_mt, "__newindex");
    lua_setreadonly(state, service_mt, 1);

    const int input_mt = metatable(kInputObjectMeta);
    lua_pushcfunction(state, &ScriptBindings::input_object_index, "index");
    lua_setfield(state, input_mt, "__index");
    lua_pushcfunction(state, &ScriptBindings::input_object_tostring, "tostring");
    lua_setfield(state, input_mt, "__tostring");
    lua_setreadonly(state, input_mt, 1);
    lua_settop(state, top);

    // One userdata per instance, as Roblox does, so == and rawequal hold and an
    // instance works as a table key. Weak values let unused handles collect.
    lua_newtable(state);
    lua_newtable(state);
    lua_pushstring(state, "v");
    lua_setfield(state, -2, "__mode");
    lua_setreadonly(state, -1, 1);
    lua_setmetatable(state, -2);
    lua_setfield(state, LUA_REGISTRYINDEX, kInstanceCache);

    lua_newtable(state);
    lua_pushcfunction(state, &ScriptBindings::task_wait, "wait");
    lua_setfield(state, -2, "wait");
    lua_pushcfunction(state, &ScriptBindings::task_spawn, "spawn");
    lua_setfield(state, -2, "spawn");
    lua_pushcfunction(state, &ScriptBindings::task_defer, "defer");
    lua_setfield(state, -2, "defer");
    lua_pushcfunction(state, &ScriptBindings::task_delay, "delay");
    lua_setfield(state, -2, "delay");
    lua_pushcfunction(state, &ScriptBindings::task_cancel, "cancel");
    lua_setfield(state, -2, "cancel");
    lua_setglobal(state, "task");

    lua_newtable(state);
    lua_pushcfunction(state, &ScriptBindings::instance_new, "new");
    lua_setfield(state, -2, "new");
    lua_note_result("Instance", "new", "", true);
    lua_setglobal(state, "Instance");

    lua_pushcfunction(state, &ScriptBindings::require, "require");
    lua_setglobal(state, "require");

    // The play state replaces these after sandboxing. The reflection state keeps
    // them, so completion sees the same globals the command line has.
    lua_newtable(state);
    lua_setglobal(state, "_G");
    lua_newtable(state);
    lua_setglobal(state, "shared");

    open_enum(state);
    open_color3(state);
    open_vector2(state);
    open_vector3(state);
    open_matrix4(state);
}

lua_State* ScriptRuntime::create_state(Vm& vm) {
    // Each VM counts its own memory against its own budget.
    lua_State* state = lua_newstate(&ScriptRuntime::allocate, &vm);
    if (state == nullptr) {
        return nullptr;
    }
    try {
        lua_Callbacks* callbacks = lua_callbacks(state);
        callbacks->userdata = this;
        open_host_libraries(state);

        luaL_sandbox(state);
        lua_setreadonly(state, LUA_GLOBALSINDEX, 0);
        lua_newtable(state);
        lua_setglobal(state, "_G");
        lua_newtable(state);
        lua_setglobal(state, "shared");
        set_root_globals(state);
        lua_setreadonly(state, LUA_GLOBALSINDEX, 1);
        lua_callbacks(state)->interrupt = &ScriptRuntime::interrupt;
        return state;
    } catch (...) {
        lua_close(state);
        throw;
    }
}

void ScriptRuntime::open_vm() {
    if (play_.state != nullptr || game_ == nullptr) {
        return;
    }
    lua_State* state = create_state(play_);
    if (state == nullptr) {
        throw std::runtime_error("could not create the Luau state");
    }
    play_.state = state;
    steps_ = 0;
    last_error_.clear();
    play_.token = std::make_shared<char>('\0');
    play_.halted = false;
    open_ = true;
}

void ScriptRuntime::ensure_state(Vm& vm) {
    if (vm.state != nullptr || game_ == nullptr) {
        return;
    }
    vm.state = create_state(vm);
    if (vm.state != nullptr) {
        vm.token = std::make_shared<char>('\0');
    }
    update_tools_open();
}

void ScriptRuntime::close_state(Vm& vm) {
    if (vm.state == nullptr) {
        vm.clock = 0;
        return;
    }
    vm.closing = true;
    vm.ready.clear();
    vm.sleep.clear();
    vm.defer.clear();
    vm.child_waits.clear();
    vm.child_found.clear();
    vm.next_child_timer = std::numeric_limits<double>::infinity();
    vm.require_cache.clear();
    for (const Thread& thread : vm.threads) {
        by_serial_.erase(thread.serial);
    }
    vm.threads.clear();
    // lua_close takes every reference with it; a handler dropped later has none to release.
    vm.token.reset();
    for (Kept& kept : vm.kept) {
        kept.connection.disconnect();
    }
    vm.kept.clear();
    vm.kept_prune_at = 64;
    lua_Callbacks* callbacks = lua_callbacks(vm.state);
    callbacks->interrupt = nullptr;
    callbacks->userdata = nullptr;
    lua_State* state = vm.state;
    vm.state = nullptr;
    if (&vm == &play_) {
        open_ = false;
    }
    lua_close(state);
    vm.closing = false;
    vm.clock = 0;
    vm.memory_used = 0;
    update_tools_open();
}

void ScriptRuntime::update_tools_open() {
    tools_open_.store(console_.state != nullptr || plugin_.state != nullptr || !core_pending_.empty(),
                      std::memory_order_relaxed);
}

void ScriptRuntime::note_core(InstanceId id) {
    if (game_ == nullptr) {
        return;
    }
    std::vector<InstanceId> pending{id};
    while (!pending.empty()) {
        const InstanceId next = pending.back();
        pending.pop_back();
        if (dynamic_cast<Script*>(game_->instance(next)) != nullptr &&
            (game_->core_holds(next) || core_scripts_.count(next) != 0)) {
            core_pending_.push_back(next);
        }
        for (InstanceId child = game_->first_child(next); child != 0; child = game_->next_sibling(child)) {
            pending.push_back(child);
        }
    }
    update_tools_open();
}

Signal* ScriptRuntime::host_signal(HostSignal which) {
    if (game_ == nullptr) {
        return nullptr;
    }
    switch (which) {
    case HostSignal::SelectionChanged:
        return &selection_changed_;
    case HostSignal::Started:
        return run_service_.started();
    case HostSignal::Stopped:
        return run_service_.stopped();
    }
    return nullptr;
}

void ScriptRuntime::fire_host_changes() {
    EventQueue& events = game_->events();
    const std::uint64_t revision = game_->selection().revision();
    if (revision != selection_revision_) {
        selection_revision_ = revision;
        events.emit_args(selection_changed_.id(), 0, {});
    }
    const bool running = game_->simulation_running();
    if (running != was_running_) {
        was_running_ = running;
        Signal* signal = running ? run_service_.started() : run_service_.stopped();
        if (signal != nullptr) {
            events.emit_args(signal->id(), 0, {});
        }
    }
}

void ScriptRuntime::start_core_scripts() {
    if (game_ == nullptr || core_pending_.empty()) {
        return;
    }
    std::vector<InstanceId> pending;
    pending.swap(core_pending_);
    const InstanceId core = game_->core();
    for (InstanceId id : pending) {
        auto* script = dynamic_cast<Script*>(game_->instance(id));
        // A Script under another Script in Core runs with that one, as a plugin's Scripts do.
        bool nested = false;
        for (InstanceId up = game_->parent(id); up != 0 && up != DataModel::kNoParent && up != core;
             up = game_->parent(up)) {
            if (dynamic_cast<Script*>(game_->instance(up)) != nullptr) {
                nested = true;
                break;
            }
        }
        const bool want = script != nullptr && script->enabled() && game_->core_holds(id) && !nested;
        const bool have = core_scripts_.count(id) != 0;
        if (want && !have) {
            if (register_plugin(id)) {
                core_scripts_.insert(id);
            }
        } else if (!want && have) {
            core_scripts_.erase(id);
            unregister_plugin(id);
        }
    }
    update_tools_open();
}

void ScriptRuntime::refresh_game(lua_State* state) {
    if (state == nullptr) {
        return;
    }
    // The global table is sealed. game's userdata carries the world generation,
    // so a command after stop has to see the restored world, not the one from startup.
    lua_setreadonly(state, LUA_GLOBALSINDEX, 0);
    set_root_globals(state);
    lua_setreadonly(state, LUA_GLOBALSINDEX, 1);
}

void ScriptRuntime::set_root_globals(lua_State* state) {
    push_instance(state, 0);
    lua_setglobal(state, "game");
    push_instance(state, game_->scene_service("Workspace"));
    lua_setglobal(state, "workspace");
}

void ScriptRuntime::close_vm() {
    if (play_.state == nullptr) {
        open_ = false;
        play_.clock = 0;
        return;
    }
    starts_.clear();
    loading_.clear();
    close_state(play_);
    lua_depth_ = 0;
    steps_ = 0;
}

void ScriptRuntime::kill_owned(Vm& vm, InstanceId script, std::uint32_t owner) {
    auto matches = [&](InstanceId thread_script, std::uint32_t thread_owner) {
        return (script == 0 || thread_script == script) && (owner == 0 || thread_owner == owner);
    };
    for (Thread& thread : vm.threads) {
        if (matches(thread.script, thread.generation)) {
            thread.dead = true;
        }
    }
    drop_dead_queues(vm);
    for (auto it = vm.kept.begin(); it != vm.kept.end();) {
        if (matches(it->script, it->owner)) {
            it->connection.disconnect();
            it = vm.kept.erase(it);
        } else {
            ++it;
        }
    }
}

void ScriptRuntime::keep(Vm& vm, InstanceId script, std::uint32_t owner, const Connection& connection) {
    if (vm.kept.size() >= vm.kept_prune_at) {
        vm.kept.erase(std::remove_if(vm.kept.begin(), vm.kept.end(),
                                     [](const Kept& kept) { return !kept.connection.connected(); }),
                      vm.kept.end());
        vm.kept_prune_at = std::max<std::size_t>(64, vm.kept.size() * 2);
    }
    vm.kept.push_back(Kept{script, owner, connection});
}

void ScriptRuntime::kill_script(InstanceId id) {
    if (game_ != nullptr) {
        game_->events().disconnect_script(id);
    }
    starts_.erase(std::remove_if(starts_.begin(), starts_.end(),
                                 [&](const Start& start) { return start.id == id; }),
                  starts_.end());
    started_.erase(id);
    for (Thread& thread : play_.threads) {
        if (thread.script == id) {
            thread.dead = true;
        }
    }
    drop_dead_queues(play_);
    if (game_ == nullptr || play_.closing) {
        return;
    }
    if (auto* script = dynamic_cast<Script*>(game_->instance(id))) {
        script->bump_start_generation();
    }
}

void ScriptRuntime::enqueue_start(Script& script) {
    if (game_ == nullptr || !open_ || play_.closing || play_.halted || !game_->simulation_running()) {
        return;
    }
    if (!script.enabled() || !runs_here(script.id())) {
        return;
    }
    const std::uint32_t generation = script.bump_start_generation();
    started_.insert(script.id());
    starts_.erase(std::remove_if(starts_.begin(), starts_.end(),
                                 [&](const Start& start) { return start.id == script.id(); }),
                  starts_.end());
    starts_.push_back(Start{script.id(), generation});
}

void ScriptRuntime::launch_starts() {
    std::vector<Start> batch;
    batch.swap(starts_);
    for (const Start& start : batch) {
        launch_one(start);
    }
}

void ScriptRuntime::launch_one(const Start& start) {
    if (game_ == nullptr || play_.state == nullptr || play_.halted) {
        return;
    }
    auto* script = dynamic_cast<Script*>(game_->instance(start.id));
    if (script == nullptr || script->start_generation() != start.generation || !script->enabled()) {
        return;
    }
    if (!runs_here(script->id()) || !game_->simulation_running()) {
        return;
    }
    const Bytecode bytecode = compile_luau(script->source());
    if (!bytecode) {
        last_error_ = "could not compile script";
        const std::string script_name = game_->name(script->id());
        append_output(OutputKind::Error, script_name.empty() ? last_error_ : script_name + ": " + last_error_);
        return;
    }
    Thread& thread = new_thread(play_, script->id(), start.generation);
    const std::string chunk = "=" + game_->name(script->id());
    const int loaded = luau_load(thread.co, chunk.c_str(), bytecode.data.get(), bytecode.size, 0);
    if (loaded != LUA_OK) {
        report_error(thread.co);
        thread.dead = true;
        return;
    }
    thread.nargs = 0;
    ready(thread);
}

void ScriptRuntime::launch_chunk(Vm& vm, std::string_view source, const std::string& chunk, InstanceId script,
                                 std::uint32_t generation) {
    // A plugin Script's own failures say which one, as a play script's do.
    const std::string prefix = script != 0 ? chunk.substr(1) + ": " : std::string();
    const Bytecode bytecode = compile_luau(source);
    if (!bytecode) {
        append_output(OutputKind::Error, prefix + "could not compile script");
        return;
    }
    guarded(vm, [&] {
        Thread& thread = new_thread(vm, script, generation);
        const int loaded = luau_load(thread.co, chunk.c_str(), bytecode.data.get(), bytecode.size, 0);
        if (loaded != LUA_OK) {
            report_error(thread.co);
            thread.dead = true;
            return;
        }
        thread.nargs = 0;
        // The first run is now, so its prints come before the caller's next line, and
        // what it spawned runs after it, as in one pass of a play step.
        if (lua_depth_ > 0) {
            ready(thread);
        } else {
            resume_one(thread);
            resume_budget(vm);
        }
    });
    release_dead_threads(vm);
}

void ScriptRuntime::flush_defer(Vm& vm) {
    for (Thread* thread : vm.defer) {
        if (thread->dead || !thread_ok(*thread)) {
            thread->dead = true;
            continue;
        }
        thread->park = Thread::Park::None;
        ready(*thread);
    }
    vm.defer.clear();
}

void ScriptRuntime::wake_sleeps(Vm& vm) {
    for (auto it = vm.sleep.begin(); it != vm.sleep.end();) {
        Thread* thread = *it;
        if (thread->dead || !thread_ok(*thread)) {
            thread->dead = true;
            it = vm.sleep.erase(it);
            continue;
        }
        if (thread->due <= vm.clock) {
            thread->park = Thread::Park::None;
            it = vm.sleep.erase(it);
            ready(*thread);
        } else {
            ++it;
        }
    }
}

void ScriptRuntime::resume_budget(Vm& vm) {
    int left = kResumeBudget;
    while (left > 0 && !vm.ready.empty()) {
        Thread* thread = vm.ready.front();
        vm.ready.pop_front();
        if (thread->dead || !thread_ok(*thread)) {
            thread->dead = true;
            continue;
        }
        resume_one(*thread);
        --left;
    }
}

void ScriptRuntime::resume_one(Thread& thread) {
    if (thread.dead || thread.co == nullptr) {
        thread.dead = true;
        return;
    }
    assert_lua_thread();
    thread.park = Thread::Park::None;
    const int nargs = thread.nargs;
    thread.nargs = 0;
    steps_ = 0;
    ++lua_depth_;
    const int status = lua_resume(thread.co, nullptr, nargs);
    --lua_depth_;
    if (thread.dead) {
        drop_dead_queues(*thread.vm);
        return;
    }
    if (status == LUA_YIELD) {
        if (thread.park == Thread::Park::None) {
            thread.park = Thread::Park::Defer;
            thread.vm->defer.push_back(&thread);
        }
        return;
    }
    if (status != LUA_OK) {
        report_error(thread.co);
        thread.dead = true;
        return;
    }
    thread.dead = true;
}

void ScriptRuntime::drop_dead(std::list<Thread*>& queue) {
    for (auto it = queue.begin(); it != queue.end();) {
        if ((*it)->dead) {
            it = queue.erase(it);
        } else {
            ++it;
        }
    }
}

void ScriptRuntime::drop_dead_queues(Vm& vm) {
    drop_dead(vm.ready);
    drop_dead(vm.sleep);
    drop_dead(vm.defer);
    drop_dead_child_waits(vm);
}

void ScriptRuntime::release_dead_threads(Vm& vm) {
    // A binding may still hold a Thread while Lua runs.
    if (lua_depth_ > 0 || vm.state == nullptr) {
        return;
    }
    drop_dead_queues(vm);
    for (auto it = vm.threads.begin(); it != vm.threads.end();) {
        if (!it->dead) {
            ++it;
            continue;
        }
        // The coroutine may live on in a script variable. Its serial then finds no thread.
        by_serial_.erase(it->serial);
        if (it->anchor != LUA_NOREF) {
            lua_unref(vm.state, it->anchor);
        }
        it = vm.threads.erase(it);
    }
}

void ScriptRuntime::ready(Thread& thread) {
    if (thread.dead) {
        return;
    }
    std::list<Thread*>& queue = thread.vm->ready;
    for (Thread* queued : queue) {
        if (queued == &thread) {
            return;
        }
    }
    queue.push_back(&thread);
}

bool ScriptRuntime::unpark(Thread& thread) {
    if (thread.dead || thread.co == nullptr) {
        return false;
    }
    thread.vm->sleep.remove(&thread);
    thread.vm->defer.remove(&thread);
    forget_child_wait(thread);
    thread.park = Thread::Park::None;
    return true;
}

void ScriptRuntime::make_ready(Thread& thread, const char* result) {
    if (!unpark(thread)) {
        return;
    }
    if (result != nullptr) {
        lua_pushstring(thread.co, result);
        thread.nargs = 1;
    }
    ready(thread);
}

void ScriptRuntime::make_ready_number(Thread& thread, double result) {
    if (!unpark(thread)) {
        return;
    }
    lua_pushnumber(thread.co, result);
    thread.nargs = 1;
    ready(thread);
}

void ScriptRuntime::make_ready_args(Thread& thread, const EventArgs* args) {
    if (!unpark(thread)) {
        return;
    }
    thread.nargs = push_event_args(thread.co, this, args);
    ready(thread);
}

void ScriptRuntime::resume_waiting_now(Thread& thread, double dt) {
    make_ready_number(thread, dt);
    // make_ready_number queues the thread for the next drain; there is none in
    // the window, so take it back off the ready list the way the drain-side
    // resume does, and resume it directly instead.
    // Resuming directly is safe here because the window is never entered from
    // inside Lua: lua_depth_ is 0 whenever this runs.
    thread.vm->ready.remove(&thread);
    resume_one(thread);
}

void ScriptRuntime::park_child_wait(Thread& thread) {
    Vm& vm = *thread.vm;
    thread.park = Thread::Park::Child;
    thread.wait_found = 0;
    vm.child_waits[thread.wait_parent].push_back(&thread);
    vm.next_child_timer = std::min(vm.next_child_timer, thread.due);
    if (!thread.wait_warned) {
        vm.next_child_timer = std::min(vm.next_child_timer, thread.wait_warn_at);
    }
}

void ScriptRuntime::forget_child_wait(Thread& thread) {
    Vm& vm = *thread.vm;
    vm.child_found.erase(std::remove(vm.child_found.begin(), vm.child_found.end(), &thread), vm.child_found.end());
    const auto found = vm.child_waits.find(thread.wait_parent);
    if (found == vm.child_waits.end()) {
        return;
    }
    std::vector<Thread*>& waiters = found->second;
    waiters.erase(std::remove(waiters.begin(), waiters.end(), &thread), waiters.end());
    if (waiters.empty()) {
        vm.child_waits.erase(found);
    }
}

void ScriptRuntime::drop_dead_child_waits(Vm& vm) {
    vm.child_found.erase(
        std::remove_if(vm.child_found.begin(), vm.child_found.end(), [](Thread* thread) { return thread->dead; }),
        vm.child_found.end());
    for (auto it = vm.child_waits.begin(); it != vm.child_waits.end();) {
        std::vector<Thread*>& waiters = it->second;
        waiters.erase(std::remove_if(waiters.begin(), waiters.end(), [](Thread* thread) { return thread->dead; }),
                      waiters.end());
        it = waiters.empty() ? vm.child_waits.erase(it) : std::next(it);
    }
}

// Runs inside set_parent and set_name, maybe while Lua is running, so it only
// moves matched threads to child_found. deliver_child_waits resumes them.
void ScriptRuntime::on_child_named(InstanceId parent, InstanceId child, const std::string& name) {
    for (Vm* vm : {&play_, &console_, &plugin_}) {
        if (vm->state == nullptr || vm->closing) {
            continue;
        }
        const auto found = vm->child_waits.find(parent);
        if (found == vm->child_waits.end()) {
            continue;
        }
        std::vector<Thread*>& waiters = found->second;
        for (auto it = waiters.begin(); it != waiters.end();) {
            Thread* thread = *it;
            if (thread->dead || thread->wait_name != name) {
                ++it;
                continue;
            }
            thread->wait_found = child;
            vm->child_found.push_back(thread);
            it = waiters.erase(it);
        }
        if (waiters.empty()) {
            vm->child_waits.erase(found);
        }
    }
}

void ScriptRuntime::deliver_child_waits(Vm& vm) {
    if (vm.child_found.empty()) {
        return;
    }
    std::vector<Thread*> found;
    found.swap(vm.child_found);
    for (Thread* thread : found) {
        if (thread->dead || !thread_ok(*thread) || thread->co == nullptr) {
            thread->dead = true;
            continue;
        }
        // The match may have moved or been renamed again before this drain.
        InstanceId child = thread->wait_found;
        thread->wait_found = 0;
        if (resolve_id(child, thread->wait_world) == nullptr || game_->parent(child) != thread->wait_parent ||
            game_->name(child) != thread->wait_name) {
            child = resolve_id(thread->wait_parent, thread->wait_world) != nullptr
                        ? game_->find_first_child(thread->wait_parent, thread->wait_name)
                        : 0;
        }
        if (child == 0) {
            park_child_wait(*thread);
            continue;
        }
        push_instance(thread->co, child);
        thread->nargs = 1;
        thread->park = Thread::Park::None;
        ready(*thread);
    }
}

void ScriptRuntime::wake_child_timers(Vm& vm) {
    if (vm.clock < vm.next_child_timer) {
        return;
    }
    vm.next_child_timer = std::numeric_limits<double>::infinity();
    std::vector<Thread*> timed_out;
    for (auto it = vm.child_waits.begin(); it != vm.child_waits.end();) {
        std::vector<Thread*>& waiters = it->second;
        for (auto wait = waiters.begin(); wait != waiters.end();) {
            Thread* thread = *wait;
            if (thread->dead || !thread_ok(*thread) || thread->co == nullptr) {
                thread->dead = true;
                wait = waiters.erase(wait);
                continue;
            }
            if (thread->due <= vm.clock) {
                timed_out.push_back(thread);
                wait = waiters.erase(wait);
                continue;
            }
            if (!thread->wait_warned && thread->wait_warn_at <= vm.clock) {
                thread->wait_warned = true;
                const std::string parent = resolve_id(thread->wait_parent, thread->wait_world) != nullptr
                                               ? game_->name(thread->wait_parent)
                                               : std::string("<destroyed>");
                append_output(OutputKind::Print,
                              "Infinite yield possible on '" + parent + ":WaitForChild(\"" + thread->wait_name + "\")'");
            }
            vm.next_child_timer = std::min(vm.next_child_timer, thread->due);
            if (!thread->wait_warned) {
                vm.next_child_timer = std::min(vm.next_child_timer, thread->wait_warn_at);
            }
            ++wait;
        }
        it = waiters.empty() ? vm.child_waits.erase(it) : std::next(it);
    }
    for (Thread* thread : timed_out) {
        lua_pushnil(thread->co);
        thread->nargs = 1;
        thread->park = Thread::Park::None;
        ready(*thread);
    }
}

bool ScriptRuntime::thread_ok(const Thread& thread) const {
    if (thread.dead || game_ == nullptr || thread.vm == nullptr) {
        return false;
    }
    if (thread.vm->kind != VmKind::Play) {
        return owner_ok(*thread.vm, thread.script, thread.generation);
    }
    // A play thread with no Script stays until it finishes or the VM closes.
    if (thread.script == 0) {
        return true;
    }
    auto* script = dynamic_cast<Script*>(game_->instance(thread.script));
    if (script == nullptr || !script->enabled()) {
        return false;
    }
    return script->start_generation() == thread.generation;
}

ScriptRuntime::Thread& ScriptRuntime::new_thread(Vm& vm, InstanceId script, std::uint32_t generation) {
    vm.threads.emplace_back();
    Thread& thread = vm.threads.back();
    thread.vm = &vm;
    thread.script = script;
    thread.generation = generation;
    thread.anchor = LUA_NOREF;
    thread.serial = ++next_serial_;
    by_serial_[thread.serial] = &thread;
    try {
        // Luau reports a failed allocation by throwing, not by returning null.
        lua_State* co = lua_newthread(vm.state);
        thread.co = co;
        thread.anchor = lua_ref(vm.state, -1);
        lua_pop(vm.state, 1);
        lua_setthreaddata(co, serial_data(thread.serial));
        luaL_sandboxthread(co);
        set_script_global(co, script);
    } catch (...) {
        // Half made: never run, and released with the other dead threads.
        thread.dead = true;
        throw;
    }
    return thread;
}

void ScriptRuntime::set_script_global(lua_State* co, InstanceId script) {
    if (script == 0) {
        lua_pushnil(co);
    } else {
        push_instance(co, script);
    }
    lua_setglobal(co, "script");
}

void ScriptRuntime::remember_error(lua_State* state) {
    if (state == nullptr) {
        last_error_ = "script error";
        return;
    }
    const char* text = lua_tostring(state, -1);
    last_error_ = text != nullptr ? text : "script error";
}

void ScriptRuntime::report_error(lua_State* state) {
    remember_error(state);
    append_output(OutputKind::Error, last_error_);
}

std::uint64_t ScriptRuntime::clear_output() {
    std::lock_guard<std::mutex> guard(output_mu_);
    output_.clear();
    return ++output_epoch_;
}

void ScriptRuntime::append_output(OutputKind kind, std::string text) { append_output(kind, std::move(text), {}); }

void ScriptRuntime::append_output(OutputKind kind, std::string text, std::vector<OutputValue> values, InstanceId script,
                                  int line) {
    if (text.empty()) {
        return;
    }
    if (text.size() > kMaxOutputBytes) {
        text.resize(fit_utf8(text, kMaxOutputBytes));
        text.append("...\n");
    } else if (text.back() != '\n') {
        text.push_back('\n');
    }
    std::lock_guard<std::mutex> guard(output_mu_);
    while (output_.size() >= kMaxOutputLines) {
        output_.pop_front();
    }
    output_.push_back(
        OutputLine{kind, std::move(text), std::move(values), std::chrono::system_clock::now(), script, line});
    while (history_.size() >= kMaxOutputLines) {
        history_.pop_front();
    }
    history_.push_back(output_.back());
    ++history_next_;
}

ScriptRuntime::OutputHistory ScriptRuntime::output_since(std::uint64_t since, std::size_t limit) const {
    std::lock_guard<std::mutex> guard(output_mu_);
    OutputHistory out;
    out.next = history_next_;
    const std::uint64_t oldest = history_next_ - history_.size();
    out.first = std::max(since, oldest);
    if (out.first > history_next_) {
        out.first = history_next_;
    }
    const std::size_t begin = static_cast<std::size_t>(out.first - oldest);
    const std::size_t end = std::min(history_.size(), begin + limit);
    for (std::size_t i = begin; i < end; ++i) {
        out.lines.push_back(history_[i]);
    }
    return out;
}

std::uint64_t ScriptRuntime::output_next() const {
    std::lock_guard<std::mutex> guard(output_mu_);
    return history_next_;
}

ScriptRuntime::OutputBatch ScriptRuntime::drain_output() {
    std::lock_guard<std::mutex> guard(output_mu_);
    OutputBatch batch;
    batch.epoch = output_epoch_;
    batch.lines.reserve(output_.size());
    while (!output_.empty()) {
        batch.lines.push_back(std::move(output_.front()));
        output_.pop_front();
    }
    return batch;
}

void ScriptRuntime::run_chunk(std::string_view source) {
    assert_lua_thread();
    if (game_ == nullptr) {
        append_output(OutputKind::Error, "Lua is not running");
        return;
    }
    try {
        ensure_state(console_);
    } catch (const std::exception& ex) {
        append_output(OutputKind::Error, ex.what());
        return;
    }
    if (console_.state == nullptr) {
        append_output(OutputKind::Error, "could not create the Luau state");
        return;
    }
    guarded(console_, [&] {
        refresh_game(console_.state);
        clear_require_cache(console_.state, console_.require_cache);
    });
    launch_chunk(console_, source, "=console", 0, 0);
}

void ScriptRuntime::reset_console() {
    assert_lua_thread();
    close_state(console_);
}

bool ScriptRuntime::register_plugin(InstanceId root) {
    assert_lua_thread();
    if (game_ == nullptr || !game_->alive(root)) {
        return false;
    }
    // An id that died may come back as another instance, which is not a plugin.
    for (auto it = plugins_.begin(); it != plugins_.end();) {
        if (game_->alive(it->root)) {
            ++it;
            continue;
        }
        const std::uint32_t serial = it->serial;
        it = plugins_.erase(it);
        kill_owned(plugin_, 0, serial);
    }
    const bool registered = std::any_of(plugins_.begin(), plugins_.end(),
                                        [&](const Plugin& plugin) { return plugin.root == root; });
    if (registered) {
        return false;
    }
    // Serials start at 1: kill_owned takes 0 for any owner.
    if (++plugin_serial_ == 0) {
        ++plugin_serial_;
    }
    const Plugin plugin{root, plugin_serial_};
    plugins_.push_back(plugin);
    try {
        ensure_state(plugin_);
    } catch (const std::exception& ex) {
        append_output(OutputKind::Error, ex.what());
        return true;
    }
    if (plugin_.state == nullptr) {
        append_output(OutputKind::Error, "could not create the Luau state");
        return true;
    }
    run_plugin(plugin);
    return true;
}

bool ScriptRuntime::unregister_plugin(InstanceId root) {
    assert_lua_thread();
    const auto found =
        std::find_if(plugins_.begin(), plugins_.end(), [&](const Plugin& plugin) { return plugin.root == root; });
    if (found == plugins_.end()) {
        return false;
    }
    const std::uint32_t serial = found->serial;
    plugins_.erase(found);
    kill_owned(plugin_, 0, serial);
    if (game_ != nullptr) {
        for (auto it = plugins_.begin(); it != plugins_.end();) {
            if (game_->alive(it->root)) {
                ++it;
                continue;
            }
            const std::uint32_t dead = it->serial;
            it = plugins_.erase(it);
            kill_owned(plugin_, 0, dead);
        }
    }
    if (plugins_.empty()) {
        close_state(plugin_);
    } else {
        release_dead_threads(plugin_);
    }
    return true;
}

bool ScriptRuntime::is_plugin(InstanceId root) const {
    return game_ != nullptr && game_->alive(root) &&
           std::any_of(plugins_.begin(), plugins_.end(), [&](const Plugin& plugin) { return plugin.root == root; });
}

std::vector<InstanceId> ScriptRuntime::plugins() const {
    std::vector<InstanceId> out;
    if (game_ == nullptr) {
        return out;
    }
    for (const Plugin& plugin : plugins_) {
        if (game_->alive(plugin.root)) {
            out.push_back(plugin.root);
        }
    }
    return out;
}

void ScriptRuntime::run_plugin(const Plugin& plugin) {
    // The order is fixed before any Script runs, since one may add, move, or destroy instances.
    std::vector<InstanceId> order;
    std::vector<InstanceId> pending{plugin.root};
    while (!pending.empty()) {
        const InstanceId next = pending.back();
        pending.pop_back();
        order.push_back(next);
        const std::vector<InstanceId> children = game_->get_children(next);
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    guarded(plugin_, [&] {
        refresh_game(plugin_.state);
        clear_require_cache(plugin_.state, plugin_.require_cache);
    });
    for (InstanceId id : order) {
        auto* script = dynamic_cast<Script*>(game_->instance(id));
        if (script == nullptr || !script->enabled() || plugin_.state == nullptr) {
            continue;
        }
        launch_chunk(plugin_, script->source(), "=" + game_->name(id), id, plugin.serial);
    }
}

// The script whose code called print, and the line it was on. A chunk's functions keep the
// globals it was loaded with, and those hold its `script`, so a ModuleScript's function
// names the module even when a Script calls it. C functions in between, such as pcall, are skipped.
void ScriptRuntime::print_source(lua_State* state, InstanceId& script, int& line) const {
    script = 0;
    line = 0;
    lua_Debug debug{};
    for (int level = 1; level < 16 && lua_getinfo(state, level, "slf", &debug) != 0; ++level) {
        if (lua_iscfunction(state, -1)) {
            lua_pop(state, 1);
            continue;
        }
        lua_getfenv(state, -1);
        if (lua_istable(state, -1)) {
            lua_rawgetfield(state, -1, "script");
            const auto* ud = static_cast<const InstanceUd*>(test_userdata(state, -1, kInstanceMeta));
            if (ud != nullptr && resolve_id(ud->id, ud->world) != nullptr && ud->id != 0) {
                script = ud->id;
                line = debug.currentline;
            }
            lua_pop(state, 1);
        }
        lua_pop(state, 2);
        return;
    }
}

int ScriptRuntime::lua_print(lua_State* state) {
    ScriptRuntime* runtime = runtime_from(state);
    const int count = lua_gettop(state);
    std::string line;
    std::vector<OutputValue> values;
    bool has_table = false;
    for (int index = 1; index <= count; ++index) {
        std::size_t length = 0;
        const char* text = luaL_tolstring(state, index, &length);
        if (index > 1) {
            line.push_back('\t');
        }
        OutputValue value;
        if (text != nullptr && length > 0) {
            line.append(text, length);
            const std::string_view whole(text, length);
            value.text.assign(whole.substr(0, fit_utf8(whole, kMaxOutputBytes)));
        }
        lua_pop(state, 1);
        if (snapshots_as_table(state, index)) {
            value.table = snapshot_table(state, index);
            has_table = true;
        }
        values.push_back(std::move(value));
    }
    line.push_back('\n');
    if (!has_table) {
        values.clear();
    }
    if (runtime != nullptr) {
        InstanceId script = 0;
        int at = 0;
        runtime->print_source(state, script, at);
        runtime->append_output(OutputKind::Print, std::move(line), std::move(values), script, at);
    }
    return 0;
}

void ScriptRuntime::push_instance(lua_State* state, InstanceId id) {
    // A console or plugin handle outlives the play session, so it is not tied to a world.
    const Vm* vm = vm_from(state);
    std::uint32_t world = game_ != nullptr ? game_->world_generation() : 0;
    if (vm != nullptr && vm->kind != VmKind::Play) {
        world = kAnyWorld;
    }
    lua_getfield(state, LUA_REGISTRYINDEX, kInstanceCache);
    const bool cached = lua_istable(state, -1);
    if (cached) {
        lua_pushnumber(state, static_cast<double>(id));
        lua_rawget(state, -2);
        const auto* hit = static_cast<InstanceUd*>(test_userdata(state, -1, kInstanceMeta));
        // A handle from an earlier world generation names a different instance.
        if (hit != nullptr && hit->world == world) {
            lua_remove(state, -2);
            return;
        }
        lua_pop(state, 1);
    }
    auto* ud = static_cast<InstanceUd*>(lua_newuserdata(state, sizeof(InstanceUd)));
    ud->id = id;
    ud->world = world;
    luaL_getmetatable(state, kInstanceMeta);
    lua_setmetatable(state, -2);
    if (cached) {
        lua_pushnumber(state, static_cast<double>(id));
        lua_pushvalue(state, -2);
        lua_rawset(state, -4);
        lua_remove(state, -2);
    } else {
        lua_remove(state, -2);
    }
}

DataModel* ScriptRuntime::resolve_id(InstanceId id, std::uint32_t world) const {
    if (game_ == nullptr || (world != kAnyWorld && world != game_->world_generation())) {
        return nullptr;
    }
    if (id == 0) {
        return game_;
    }
    return game_->instance(id);
}

void ScriptRuntime::fire_phase(Phase phase, double dt) {
    if (!open_ || play_.closing || game_ == nullptr) {
        return;
    }
    // A frame's input reaches scripts first, in the drain after PreAnimation,
    // so everything later in the step reads the keys as they are now.
    // RenderStepped follows the input, as in Roblox, and comes before the step.
    if (phase == Phase::PreAnimation) {
        game_->input().dispatch(game_->events());
        run_service_.fire_render_stepped(game_->events());
    }
    run_service_.fire(game_->events(), phase, dt);
}

ScriptRuntime::Thread* ScriptRuntime::start_listener(Vm& vm, int ref, InstanceId script, std::uint32_t generation) {
    if (vm.state == nullptr || vm.closing || (vm.kind == VmKind::Play && !open_)) {
        return nullptr;
    }
    if (!owner_ok(vm, script, generation)) {
        return nullptr;
    }
    Thread& thread = new_thread(vm, script, generation);
    lua_getref(vm.state, ref);
    lua_xmove(vm.state, thread.co, 1);
    return &thread;
}

void ScriptRuntime::run_listener(Thread& thread) {
    if (lua_depth_ > 0) {
        ready(thread);
        return;
    }
    resume_one(thread);
}

void ScriptRuntime::invoke_listener(Vm& vm, int ref, InstanceId script, std::uint32_t generation, const char* text,
                                    bool pass_number, double number) {
    guarded(vm, [&] {
        Thread* thread = start_listener(vm, ref, script, generation);
        if (thread == nullptr) {
            return;
        }
        if (text != nullptr) {
            lua_pushstring(thread->co, text);
            thread->nargs = 1;
        } else if (pass_number) {
            lua_pushnumber(thread->co, number);
            thread->nargs = 1;
        }
        run_listener(*thread);
    });
}

void ScriptRuntime::invoke_listener_args(Vm& vm, int ref, InstanceId script, std::uint32_t generation,
                                         const EventArgs* args) {
    guarded(vm, [&] {
        Thread* thread = start_listener(vm, ref, script, generation);
        if (thread == nullptr) {
            return;
        }
        thread->nargs = push_event_args(thread->co, this, args);
        run_listener(*thread);
    });
}

int ScriptRuntime::require_module(lua_State* state, InstanceId module_id) {
    if (game_ == nullptr) {
        luaL_error(state, "require has no data model");
    }
    auto* module = dynamic_cast<ModuleScript*>(game_->instance(module_id));
    if (module == nullptr) {
        luaL_error(state, "require expects a ModuleScript");
    }
    // The module runs in the caller's VM and is cached there: the play VM for scripts,
    // the console's VM for the command line, the plugin VM for plugins.
    Vm* vm = vm_from(state);
    if (vm == nullptr) {
        luaL_error(state, "require has no VM for this thread");
    }
    std::unordered_map<InstanceId, int>& cache = vm->require_cache;
    const auto cached = cache.find(module_id);
    if (cached != cache.end()) {
        if (cached->second == LUA_REFNIL) {
            lua_pushnil(state);
        } else {
            lua_getref(state, cached->second);
        }
        return 1;
    }
    for (InstanceId loading : loading_) {
        if (loading == module_id) {
            luaL_error(state, "require cycle");
        }
    }
    loading_.push_back(module_id);
    struct PopLoading {
        std::vector<InstanceId>& stack;
        ~PopLoading() {
            if (!stack.empty()) {
                stack.pop_back();
            }
        }
    } pop{loading_};

    // The module gets a scheduler thread owned as its caller is, so its signals belong to that owner.
    Thread* caller = ScriptRuntime::thread_from(state);
    const InstanceId owner = caller != nullptr ? caller->script : 0;
    const std::uint32_t generation = caller != nullptr ? caller->generation : 0;
    Thread* thread = &new_thread(*vm, owner, generation);
    set_script_global(thread->co, module_id);
    lua_State* co = thread->co;
    struct Finish {
        Thread* thread;
        ~Finish() {
            if (thread != nullptr) {
                thread->dead = true;
            }
        }
    } finish{thread};

    const Bytecode bytecode = compile_luau(module->source());
    if (!bytecode) {
        luaL_error(state, "could not compile ModuleScript");
    }
    const std::string chunk = "=" + game_->name(module_id);
    const int loaded = luau_load(co, chunk.c_str(), bytecode.data.get(), bytecode.size, 0);
    if (loaded != LUA_OK) {
        remember_error(co);
        const std::string message = last_error_;
        luaL_error(state, "%s", message.c_str());
    }
    const int status = lua_resume(co, state, 0);
    if (status == LUA_YIELD) {
        luaL_error(state, "ModuleScript yielded");
    }
    if (status != LUA_OK) {
        remember_error(co);
        const std::string message = last_error_;
        luaL_error(state, "%s", message.c_str());
    }
    if (lua_gettop(co) <= 0 || lua_isnil(co, 1)) {
        cache[module_id] = LUA_REFNIL;
        lua_pushnil(state);
        return 1;
    }
    lua_pushvalue(co, 1);
    lua_xmove(co, vm->state, 1);
    const int ref = lua_ref(vm->state, -1);
    lua_pop(vm->state, 1);
    cache[module_id] = ref;
    lua_getref(state, ref);
    return 1;
}

}  // namespace engine_core
