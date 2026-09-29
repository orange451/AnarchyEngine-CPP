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

ScriptRuntime::~ScriptRuntime() { detach(); }

void ScriptRuntime::halt(const char* why) {
    if (game_ != nullptr) {
        game_->events().disconnect_scripted();
    }
    for (Thread& thread : threads_) {
        thread.dead = true;
    }
    ready_.clear();
    sleep_.clear();
    defer_.clear();
    child_waits_.clear();
    child_found_.clear();
    next_child_timer_ = std::numeric_limits<double>::infinity();
    starts_.clear();
    halted_ = true;
    if (state_ != nullptr) {
        // A throw can leave what it was pushing on the main thread's stack.
        lua_settop(state_, 0);
    }
    last_error_ = std::string("Scripts stopped: ") + (why != nullptr ? why : "Luau failed");
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
    game.input().bind(game.events());
    for (Phase phase : {Phase::PreAnimation, Phase::PreSimulation, Phase::PostSimulation, Phase::Heartbeat}) {
        phase_jobs_.push_back(scheduler.bind(phase, [this, phase](double dt) { fire_phase(phase, dt); }));
    }
}

void ScriptRuntime::detach() {
    close_console();
    if (game_ != nullptr) {
        game_->events().disconnect_scripted();
    }
    close_vm();
    if (game_ != nullptr) {
        run_service_.release(game_->events());
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
    if (!open_ || closing_) {
        return;
    }
    assert_lua_thread();
    if (dt < 0) {
        dt = 0;
    }
    sim_clock_ += dt;
    guarded([&] {
        wake_sleeps();
        deliver_child_waits();
        wake_child_timers();
        launch_starts();
        flush_defer();
        resume_budget();
        if (!starts_.empty()) {
            launch_starts();
            resume_budget();
        }
    });
    release_dead_threads();
}

bool ScriptRuntime::global_is_nil(const char* name) {
    bool nil = true;
    with_global(state_, name, [&](lua_State* state) { nil = lua_isnil(state, -1); });
    return nil;
}

bool ScriptRuntime::global_number(const char* name, double& out) {
    bool ok = false;
    with_global(state_, name, [&](lua_State* state) {
        ok = lua_isnumber(state, -1);
        if (ok) {
            out = lua_tonumber(state, -1);
        }
    });
    return ok;
}

bool ScriptRuntime::global_boolean(const char* name, bool& out) {
    bool ok = false;
    with_global(state_, name, [&](lua_State* state) {
        ok = lua_isboolean(state, -1);
        if (ok) {
            out = lua_toboolean(state, -1) != 0;
        }
    });
    return ok;
}

ScriptRuntime::Watch ScriptRuntime::watch_global(const char* name) {
    Watch watch;
    with_global(state_, name, [&](lua_State* state) {
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

void ScriptRuntime::on_script_parent(Script& script, InstanceId, InstanceId next) {
    if (game_ == nullptr || !game_->simulation_running() || closing_) {
        return;
    }
    // As in Roblox, a running script keeps running wherever it moves, out of the
    // tree too. One that has not run this session starts once it has a parent.
    if (next != DataModel::kNoParent && started_.count(script.id()) == 0) {
        enqueue_start(script);
    }
}

void ScriptRuntime::on_script_enabled(Script& script, bool enabled) {
    if (game_ == nullptr || !game_->simulation_running() || closing_) {
        return;
    }
    kill_script(script.id());
    if (enabled) {
        enqueue_start(script);
    }
}

void ScriptRuntime::on_script_destroyed(Script& script) {
    if (closing_) {
        return;
    }
    kill_script(script.id());
}

ScriptRuntime::Thread* ScriptRuntime::thread_from(lua_State* state) {
    const ScriptRuntime* runtime = runtime_from(state);
    return runtime != nullptr ? runtime->find_thread(data_serial(lua_getthreaddata(state))) : nullptr;
}

ScriptRuntime::Thread* ScriptRuntime::find_thread(std::uint64_t serial) const {
    const auto found = by_serial_.find(serial);
    return found != by_serial_.end() ? found->second : nullptr;
}

bool ScriptRuntime::gate(InstanceId script, std::uint32_t generation, void* userdata) {
    auto* self = static_cast<ScriptRuntime*>(userdata);
    if (self == nullptr || self->game_ == nullptr || self->closing_ || self->halted_) {
        return false;
    }
    auto* object = dynamic_cast<Script*>(self->game_->instance(script));
    if (object == nullptr || !object->enabled()) {
        return false;
    }
    return object->start_generation() == generation;
}

void* ScriptRuntime::allocate(void* userdata, void* pointer, std::size_t old_size, std::size_t new_size) {
    auto* self = static_cast<ScriptRuntime*>(userdata);
    return budget_realloc(self->memory_used_, kMemoryLimit, pointer, old_size, new_size);
}

void* ScriptRuntime::allocate_console(void* userdata, void* pointer, std::size_t old_size, std::size_t new_size) {
    auto* self = static_cast<ScriptRuntime*>(userdata);
    return budget_realloc(self->console_memory_used_, kMemoryLimit, pointer, old_size, new_size);
}

void ScriptRuntime::interrupt(lua_State* state, int gc) {
    if (gc >= 0) {
        return;
    }
    auto* self = runtime_from(state);
    if (self == nullptr || self->closing_) {
        return;
    }
    if (self->steps_ < kScriptTimeout) {
        ++self->steps_;
        return;
    }
    luaL_error(state, "ScriptTimeout");
}

void ScriptRuntime::on_end_of_drain() {
    if (!open_ || closing_) {
        return;
    }
    assert_lua_thread();
    guarded([&] {
        launch_starts();
        deliver_child_waits();
        flush_defer();
        resume_budget();
        if (!starts_.empty()) {
            launch_starts();
            resume_budget();
        }
    });
    release_dead_threads();
}

void ScriptRuntime::on_start() {
    // The previous session's lines are dropped before this session's scripts run.
    clear_output();
    open_vm();
    sim_clock_ = 0;
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
        game_->input().set_active(false);
        game_->input().reset();
    }
    started_.clear();
    close_vm();
}

void ScriptRuntime::assert_lua_thread() const {
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
}

lua_State* ScriptRuntime::create_state(bool console) {
    lua_State* state = lua_newstate(console ? &ScriptRuntime::allocate_console : &ScriptRuntime::allocate, this);
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
        push_instance(state, 0);
        lua_setglobal(state, "game");
        lua_setreadonly(state, LUA_GLOBALSINDEX, 1);
        lua_callbacks(state)->interrupt = &ScriptRuntime::interrupt;
        return state;
    } catch (...) {
        lua_close(state);
        throw;
    }
}

void ScriptRuntime::open_vm() {
    if (state_ != nullptr || game_ == nullptr) {
        return;
    }
    lua_State* state = create_state(false);
    if (state == nullptr) {
        throw std::runtime_error("could not create the Luau state");
    }
    state_ = state;
    steps_ = 0;
    last_error_.clear();
    vm_token_ = std::make_shared<char>('\0');
    halted_ = false;
    open_ = true;
}

void ScriptRuntime::ensure_console() {
    if (console_state_ != nullptr || game_ == nullptr) {
        return;
    }
    console_state_ = create_state(true);
}

void ScriptRuntime::close_console() {
    if (console_state_ == nullptr) {
        return;
    }
    lua_State* state = console_state_;
    console_state_ = nullptr;
    console_require_cache_.clear();
    lua_Callbacks* callbacks = lua_callbacks(state);
    callbacks->interrupt = nullptr;
    callbacks->userdata = nullptr;
    lua_close(state);
    console_memory_used_ = 0;
}

void ScriptRuntime::refresh_game(lua_State* state) {
    if (state == nullptr) {
        return;
    }
    // The global table is sealed. game's userdata carries the world generation,
    // so a command after stop has to see the restored world, not the one from startup.
    lua_setreadonly(state, LUA_GLOBALSINDEX, 0);
    push_instance(state, 0);
    lua_setglobal(state, "game");
    lua_setreadonly(state, LUA_GLOBALSINDEX, 1);
}

void ScriptRuntime::close_vm() {
    if (state_ == nullptr) {
        open_ = false;
        sim_clock_ = 0;
        return;
    }
    closing_ = true;
    ready_.clear();
    sleep_.clear();
    defer_.clear();
    child_waits_.clear();
    child_found_.clear();
    next_child_timer_ = std::numeric_limits<double>::infinity();
    starts_.clear();
    require_cache_.clear();
    loading_.clear();
    threads_.clear();
    by_serial_.clear();
    // lua_close takes every reference with it; a handler dropped later has none to release.
    vm_token_.reset();
    lua_Callbacks* callbacks = lua_callbacks(state_);
    callbacks->interrupt = nullptr;
    callbacks->userdata = nullptr;
    lua_State* state = state_;
    state_ = nullptr;
    open_ = false;
    lua_close(state);
    closing_ = false;
    sim_clock_ = 0;
    memory_used_ = 0;
    lua_depth_ = 0;
    steps_ = 0;
}

void ScriptRuntime::kill_script(InstanceId id) {
    if (game_ != nullptr) {
        game_->events().disconnect_script(id);
    }
    starts_.erase(std::remove_if(starts_.begin(), starts_.end(),
                                 [&](const Start& start) { return start.id == id; }),
                  starts_.end());
    started_.erase(id);
    for (Thread& thread : threads_) {
        if (thread.script == id) {
            thread.dead = true;
        }
    }
    drop_dead(ready_);
    drop_dead(sleep_);
    drop_dead(defer_);
    drop_dead_child_waits();
    if (game_ == nullptr || closing_) {
        return;
    }
    if (auto* script = dynamic_cast<Script*>(game_->instance(id))) {
        script->bump_start_generation();
    }
}

void ScriptRuntime::enqueue_start(Script& script) {
    if (game_ == nullptr || !open_ || closing_ || halted_ || !game_->simulation_running()) {
        return;
    }
    if (!script.enabled() || game_->parent(script.id()) == DataModel::kNoParent) {
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
    if (game_ == nullptr || state_ == nullptr || halted_) {
        return;
    }
    auto* script = dynamic_cast<Script*>(game_->instance(start.id));
    if (script == nullptr || script->start_generation() != start.generation || !script->enabled()) {
        return;
    }
    if (game_->parent(script->id()) == DataModel::kNoParent || !game_->simulation_running()) {
        return;
    }
    const Bytecode bytecode = compile_luau(script->source());
    if (!bytecode) {
        last_error_ = "could not compile script";
        const std::string script_name = game_->name(script->id());
        append_output(OutputKind::Error, script_name.empty() ? last_error_ : script_name + ": " + last_error_);
        return;
    }
    Thread& thread = new_thread(script->id(), start.generation);
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

void ScriptRuntime::flush_defer() {
    for (Thread* thread : defer_) {
        if (thread->dead || !thread_ok(*thread)) {
            thread->dead = true;
            continue;
        }
        thread->park = Thread::Park::None;
        ready(*thread);
    }
    defer_.clear();
}

void ScriptRuntime::wake_sleeps() {
    for (auto it = sleep_.begin(); it != sleep_.end();) {
        Thread* thread = *it;
        if (thread->dead || !thread_ok(*thread)) {
            thread->dead = true;
            it = sleep_.erase(it);
            continue;
        }
        if (thread->due <= sim_clock_) {
            thread->park = Thread::Park::None;
            it = sleep_.erase(it);
            ready(*thread);
        } else {
            ++it;
        }
    }
}

void ScriptRuntime::resume_budget() {
    int left = kResumeBudget;
    while (left > 0 && !ready_.empty()) {
        Thread* thread = ready_.front();
        ready_.pop_front();
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
        drop_dead(ready_);
        drop_dead(sleep_);
        drop_dead(defer_);
        drop_dead_child_waits();
        return;
    }
    if (status == LUA_YIELD) {
        if (thread.park == Thread::Park::None) {
            thread.park = Thread::Park::Defer;
            defer_.push_back(&thread);
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

void ScriptRuntime::release_dead_threads() {
    // A binding may still hold a Thread while Lua runs.
    if (lua_depth_ > 0 || state_ == nullptr) {
        return;
    }
    drop_dead(ready_);
    drop_dead(sleep_);
    drop_dead(defer_);
    drop_dead_child_waits();
    for (auto it = threads_.begin(); it != threads_.end();) {
        if (!it->dead) {
            ++it;
            continue;
        }
        // The coroutine may live on in a script variable. Its serial then finds no thread.
        by_serial_.erase(it->serial);
        if (it->anchor != LUA_NOREF) {
            lua_unref(state_, it->anchor);
        }
        it = threads_.erase(it);
    }
}

void ScriptRuntime::ready(Thread& thread) {
    if (thread.dead) {
        return;
    }
    for (Thread* queued : ready_) {
        if (queued == &thread) {
            return;
        }
    }
    ready_.push_back(&thread);
}

bool ScriptRuntime::unpark(Thread& thread) {
    if (thread.dead || thread.co == nullptr) {
        return false;
    }
    sleep_.remove(&thread);
    defer_.remove(&thread);
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

void ScriptRuntime::park_child_wait(Thread& thread) {
    thread.park = Thread::Park::Child;
    thread.wait_found = 0;
    child_waits_[thread.wait_parent].push_back(&thread);
    next_child_timer_ = std::min(next_child_timer_, thread.due);
    if (!thread.wait_warned) {
        next_child_timer_ = std::min(next_child_timer_, thread.wait_warn_at);
    }
}

void ScriptRuntime::forget_child_wait(Thread& thread) {
    child_found_.erase(std::remove(child_found_.begin(), child_found_.end(), &thread), child_found_.end());
    const auto found = child_waits_.find(thread.wait_parent);
    if (found == child_waits_.end()) {
        return;
    }
    std::vector<Thread*>& waiters = found->second;
    waiters.erase(std::remove(waiters.begin(), waiters.end(), &thread), waiters.end());
    if (waiters.empty()) {
        child_waits_.erase(found);
    }
}

void ScriptRuntime::drop_dead_child_waits() {
    child_found_.erase(std::remove_if(child_found_.begin(), child_found_.end(), [](Thread* thread) { return thread->dead; }),
                       child_found_.end());
    for (auto it = child_waits_.begin(); it != child_waits_.end();) {
        std::vector<Thread*>& waiters = it->second;
        waiters.erase(std::remove_if(waiters.begin(), waiters.end(), [](Thread* thread) { return thread->dead; }),
                      waiters.end());
        it = waiters.empty() ? child_waits_.erase(it) : std::next(it);
    }
}

// Runs inside set_parent and set_name, maybe while Lua is running, so it only
// moves matched threads to child_found_. deliver_child_waits resumes them.
void ScriptRuntime::on_child_named(InstanceId parent, InstanceId child, const std::string& name) {
    if (!open_ || closing_) {
        return;
    }
    const auto found = child_waits_.find(parent);
    if (found == child_waits_.end()) {
        return;
    }
    std::vector<Thread*>& waiters = found->second;
    for (auto it = waiters.begin(); it != waiters.end();) {
        Thread* thread = *it;
        if (thread->dead || thread->wait_name != name) {
            ++it;
            continue;
        }
        thread->wait_found = child;
        child_found_.push_back(thread);
        it = waiters.erase(it);
    }
    if (waiters.empty()) {
        child_waits_.erase(found);
    }
}

void ScriptRuntime::deliver_child_waits() {
    if (child_found_.empty()) {
        return;
    }
    std::vector<Thread*> found;
    found.swap(child_found_);
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

void ScriptRuntime::wake_child_timers() {
    if (sim_clock_ < next_child_timer_) {
        return;
    }
    next_child_timer_ = std::numeric_limits<double>::infinity();
    std::vector<Thread*> timed_out;
    for (auto it = child_waits_.begin(); it != child_waits_.end();) {
        std::vector<Thread*>& waiters = it->second;
        for (auto wait = waiters.begin(); wait != waiters.end();) {
            Thread* thread = *wait;
            if (thread->dead || !thread_ok(*thread) || thread->co == nullptr) {
                thread->dead = true;
                wait = waiters.erase(wait);
                continue;
            }
            if (thread->due <= sim_clock_) {
                timed_out.push_back(thread);
                wait = waiters.erase(wait);
                continue;
            }
            if (!thread->wait_warned && thread->wait_warn_at <= sim_clock_) {
                thread->wait_warned = true;
                const std::string parent = resolve_id(thread->wait_parent, thread->wait_world) != nullptr
                                               ? game_->name(thread->wait_parent)
                                               : std::string("<destroyed>");
                append_output(OutputKind::Print,
                              "Infinite yield possible on '" + parent + ":WaitForChild(\"" + thread->wait_name + "\")'");
            }
            next_child_timer_ = std::min(next_child_timer_, thread->due);
            if (!thread->wait_warned) {
                next_child_timer_ = std::min(next_child_timer_, thread->wait_warn_at);
            }
            ++wait;
        }
        it = waiters.empty() ? child_waits_.erase(it) : std::next(it);
    }
    for (Thread* thread : timed_out) {
        lua_pushnil(thread->co);
        thread->nargs = 1;
        thread->park = Thread::Park::None;
        ready(*thread);
    }
}

bool ScriptRuntime::thread_ok(const Thread& thread) const {
    if (thread.dead || game_ == nullptr) {
        return false;
    }
    // The command line is not a Script. It stays until the chunk finishes or the VM closes.
    if (thread.script == 0) {
        return true;
    }
    auto* script = dynamic_cast<Script*>(game_->instance(thread.script));
    if (script == nullptr || !script->enabled()) {
        return false;
    }
    return script->start_generation() == thread.generation;
}

ScriptRuntime::Thread& ScriptRuntime::new_thread(InstanceId script, std::uint32_t generation) {
    threads_.emplace_back();
    Thread& thread = threads_.back();
    thread.script = script;
    thread.generation = generation;
    thread.anchor = LUA_NOREF;
    thread.serial = ++next_serial_;
    by_serial_[thread.serial] = &thread;
    try {
        // Luau reports a failed allocation by throwing, not by returning null.
        lua_State* co = lua_newthread(state_);
        thread.co = co;
        thread.anchor = lua_ref(state_, -1);
        lua_pop(state_, 1);
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

void ScriptRuntime::eval_chunk(lua_State* state, std::string_view source) {
    if (state == nullptr) {
        append_output(OutputKind::Error, "could not create the Luau state");
        return;
    }
    const Bytecode bytecode = compile_luau(source);
    if (!bytecode) {
        append_output(OutputKind::Error, "could not compile script");
        return;
    }
    lua_State* co = lua_newthread(state);
    const int anchor = lua_ref(state, -1);
    lua_pop(state, 1);
    luaL_sandboxthread(co);
    const int loaded = luau_load(co, "=console", bytecode.data.get(), bytecode.size, 0);
    if (loaded != LUA_OK) {
        report_error(co);
        lua_unref(state, anchor);
        return;
    }
    const std::uint64_t saved_steps = steps_;
    steps_ = 0;
    const int status = lua_resume(co, nullptr, 0);
    steps_ = saved_steps;
    if (status == LUA_YIELD) {
        append_output(OutputKind::Error, "command yielded");
    } else if (status != LUA_OK) {
        report_error(co);
    }
    lua_unref(state, anchor);
}

void ScriptRuntime::run_chunk(std::string_view source) {
    assert_lua_thread();
    if (game_ == nullptr) {
        append_output(OutputKind::Error, "Lua is not running");
        return;
    }
    try {
        ensure_console();
    } catch (const std::exception& ex) {
        append_output(OutputKind::Error, ex.what());
        return;
    }
    if (console_state_ == nullptr) {
        append_output(OutputKind::Error, "could not create the Luau state");
        return;
    }
    refresh_game(console_state_);
    for (const auto& entry : console_require_cache_) {
        if (entry.second != LUA_REFNIL) {
            lua_unref(console_state_, entry.second);
        }
    }
    console_require_cache_.clear();
    eval_chunk(console_state_, source);
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
    const std::uint32_t world = game_ != nullptr ? game_->world_generation() : 0;
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
    if (game_ == nullptr || world != game_->world_generation()) {
        return nullptr;
    }
    if (id == 0) {
        return game_;
    }
    return game_->instance(id);
}

void ScriptRuntime::fire_phase(Phase phase, double dt) {
    if (!open_ || closing_ || game_ == nullptr) {
        return;
    }
    // A frame's input reaches scripts first, in the drain after PreAnimation,
    // so everything later in the step reads the keys as they are now.
    if (phase == Phase::PreAnimation) {
        game_->input().dispatch(game_->events());
    }
    run_service_.fire(game_->events(), phase, dt);
}

ScriptRuntime::Thread* ScriptRuntime::start_listener(int ref, InstanceId script, std::uint32_t generation) {
    if (!open_ || closing_ || state_ == nullptr) {
        return nullptr;
    }
    if (!gate(script, generation, this)) {
        return nullptr;
    }
    Thread& thread = new_thread(script, generation);
    lua_getref(state_, ref);
    lua_xmove(state_, thread.co, 1);
    return &thread;
}

void ScriptRuntime::run_listener(Thread& thread) {
    if (lua_depth_ > 0) {
        ready(thread);
        return;
    }
    resume_one(thread);
}

void ScriptRuntime::invoke_listener(int ref, InstanceId script, std::uint32_t generation, const char* text,
                                    bool pass_number, double number) {
    guarded([&] {
        Thread* thread = start_listener(ref, script, generation);
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

void ScriptRuntime::invoke_listener_input(int ref, InstanceId script, std::uint32_t generation,
                                          const InputRecord& record) {
    guarded([&] {
        Thread* thread = start_listener(ref, script, generation);
        if (thread == nullptr) {
            return;
        }
        push_input_object(thread->co, record);
        lua_pushboolean(thread->co, record.processed ? 1 : 0);
        thread->nargs = 2;
        run_listener(*thread);
    });
}

void ScriptRuntime::make_ready_input(Thread& thread, const InputRecord& record) {
    if (!unpark(thread)) {
        return;
    }
    push_input_object(thread.co, record);
    lua_pushboolean(thread.co, record.processed ? 1 : 0);
    thread.nargs = 2;
    ready(thread);
}

const InputRecord* ScriptRuntime::delivered_input() const {
    if (game_ == nullptr) {
        return nullptr;
    }
    return game_->input().record(game_->events().payload());
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
    // the console's own VM for the command line.
    lua_State* vm = lua_mainthread(state);
    const bool console = console_state_ != nullptr && vm == console_state_;
    if (!console && vm != state_) {
        luaL_error(state, "require has no VM for this thread");
    }
    std::unordered_map<InstanceId, int>& cache = console ? console_require_cache_ : require_cache_;
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

    // A script's module gets a scheduler thread, so its signals belong to that script.
    // The console's is a plain coroutine anchored until this call returns.
    lua_State* co = nullptr;
    Thread* thread = nullptr;
    struct Anchor {
        lua_State* vm = nullptr;
        int ref = LUA_NOREF;
        ~Anchor() {
            if (vm != nullptr && ref != LUA_NOREF) {
                lua_unref(vm, ref);
            }
        }
    } anchor;
    if (console) {
        co = lua_newthread(vm);
        anchor.vm = vm;
        anchor.ref = lua_ref(vm, -1);
        lua_pop(vm, 1);
        luaL_sandboxthread(co);
        set_script_global(co, module_id);
    } else {
        Thread* caller = ScriptRuntime::thread_from(state);
        const InstanceId owner = caller != nullptr ? caller->script : 0;
        const std::uint32_t generation = caller != nullptr ? caller->generation : 0;
        thread = &new_thread(owner, generation);
        set_script_global(thread->co, module_id);
        co = thread->co;
    }
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
    lua_xmove(co, vm, 1);
    const int ref = lua_ref(vm, -1);
    lua_pop(vm, 1);
    cache[module_id] = ref;
    lua_getref(state, ref);
    return 1;
}

}  // namespace engine_core
