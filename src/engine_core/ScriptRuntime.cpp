#include "ScriptRuntime.hpp"

#include "Contract.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "TestTriangle.hpp"
#include "Vector3.hpp"

#include "lualib.h"
#include "luacode.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace engine_core {
namespace {

constexpr int kAnchorNone = -1;

struct InstanceUd {
    InstanceId id = 0;
    std::uint32_t world = 0;
};

struct SignalUd {
    int kind = 0;
    InstanceId id = 0;
    std::uint32_t world = 0;
    int phase = 0;
    bool blocked = false;
    char blocked_name[24] = {};
};

const char* kInstanceMeta = "AE.Instance";
const char* kSignalMeta = "AE.Signal";
const char* kConnectionMeta = "AE.Connection";
const char* kThreadMeta = "AE.Thread";
const char* kServiceMeta = "AE.RunService";

const luaL_Reg kLibraries[] = {
    {"", luaopen_base},
    {LUA_COLIBNAME, luaopen_coroutine},
    {LUA_TABLIBNAME, luaopen_table},
    {LUA_STRLIBNAME, luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
    {LUA_BITLIBNAME, luaopen_bit32},
    {LUA_BUFFERLIBNAME, luaopen_buffer},
    {LUA_VECLIBNAME, luaopen_vector},
    {nullptr, nullptr},
};

const char* kRemoved[] = {
    "getfenv", "setfenv", "loadstring", "dofile", "loadfile", "load", "require", "collectgarbage", "module",
    "debug",   "os",      "io",         "package", nullptr,
};

const char* field_name(Field field) {
    switch (field) {
    case Field::Transform:
        return "Transform";
    case Field::Color:
        return "Color";
    case Field::Size:
        return "Size";
    case Field::LinearVelocity:
        return "LinearVelocity";
    case Field::Simulated:
        return "Simulated";
    case Field::VisualOnly:
        return "VisualOnly";
    case Field::Parent:
        return "Parent";
    case Field::Name:
        return "Name";
    case Field::Source:
        return "Source";
    case Field::Enabled:
        return "Enabled";
    case Field::Count:
        break;
    }
    return "Unknown";
}

ScriptRuntime* runtime_from(lua_State* state) {
    return static_cast<ScriptRuntime*>(lua_callbacks(state)->userdata);
}

void* test_udata(lua_State* state, int index, const char* name) {
    void* data = lua_touserdata(state, index);
    if (data == nullptr || !lua_getmetatable(state, index)) {
        return nullptr;
    }
    luaL_getmetatable(state, name);
    const bool match = lua_rawequal(state, -1, -2) != 0;
    lua_pop(state, 2);
    return match ? data : nullptr;
}

template <typename Fn>
int lua_guard(lua_State* state, Fn fn) {
    try {
        return fn();
    } catch (const ContractViolation& ex) {
        luaL_error(state, "%s", ex.what());
    }
}

bool read_color(lua_State* state, int index, ColorRgb& color) {
    if (!lua_istable(state, index)) {
        return false;
    }
    auto component = [&](const char* name, int slot, float fallback) {
        lua_getfield(state, index, name);
        if (lua_isnumber(state, -1)) {
            const float value = static_cast<float>(lua_tonumber(state, -1));
            lua_pop(state, 1);
            return value;
        }
        lua_pop(state, 1);
        lua_rawgeti(state, index, slot);
        if (lua_isnumber(state, -1)) {
            const float value = static_cast<float>(lua_tonumber(state, -1));
            lua_pop(state, 1);
            return value;
        }
        lua_pop(state, 1);
        return fallback;
    };
    color.r = component("r", 1, 0.f);
    color.g = component("g", 2, 0.f);
    color.b = component("b", 3, 0.f);
    color.a = component("a", 4, 1.f);
    return true;
}

void push_color(lua_State* state, ColorRgb color) {
    lua_newtable(state);
    lua_pushnumber(state, color.r);
    lua_setfield(state, -2, "r");
    lua_pushnumber(state, color.g);
    lua_setfield(state, -2, "g");
    lua_pushnumber(state, color.b);
    lua_setfield(state, -2, "b");
    lua_pushnumber(state, color.a);
    lua_setfield(state, -2, "a");
}

bool is_a(const DataModel& object, const char* name) {
    if (name == nullptr) {
        return false;
    }
    if (std::strcmp(object.class_name(), name) == 0) {
        return true;
    }
    if (std::strcmp(name, "DataModel") == 0) {
        return true;
    }
    if (std::strcmp(name, "GameObject") == 0) {
        return dynamic_cast<const GameObject*>(&object) != nullptr;
    }
    if (std::strcmp(name, "Script") == 0) {
        return dynamic_cast<const Script*>(&object) != nullptr;
    }
    if (std::strcmp(name, "ModuleScript") == 0) {
        return dynamic_cast<const ModuleScript*>(&object) != nullptr;
    }
    if (std::strcmp(name, "LuaSource") == 0) {
        return dynamic_cast<const LuaSource*>(&object) != nullptr;
    }
    return false;
}

constexpr std::size_t kMaxOutputBytes = 16 * 1024;
constexpr std::size_t kMaxOutputLines = 1024;

// Cut on a code-point boundary so a capped line is still valid UTF-8.
std::size_t fit_utf8(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text.size();
    }
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return end;
}

}  // namespace

struct ScriptBindings {
    static int task_wait(lua_State* state);
    static int task_spawn(lua_State* state);
    static int task_defer(lua_State* state);
    static int task_delay(lua_State* state);
    static int task_cancel(lua_State* state);
    static int instance_new(lua_State* state);
    static int require(lua_State* state);
    static int instance_index(lua_State* state);
    static int instance_newindex(lua_State* state);
    static int instance_destroy(lua_State* state);
    static int instance_children(lua_State* state);
    static int instance_find(lua_State* state);
    static int instance_isa(lua_State* state);
    static int instance_tostring(lua_State* state);
    static int instance_service(lua_State* state);
    static int signal_connect(lua_State* state);
    static int signal_wait(lua_State* state);
    static int signal_index(lua_State* state);
    static int connection_disconnect(lua_State* state);
    static int connection_gc(lua_State* state);
    static int connection_index(lua_State* state);
    static int service_index(lua_State* state);
    static int thread_index(lua_State* state);
};

ScriptRuntime::~ScriptRuntime() { detach(); }

void ScriptRuntime::attach(DataModel& model, TaskScheduler& scheduler) {
    if (model_ != nullptr) {
        return;
    }
    model_ = &model;
    scheduler_ = &scheduler;
    scheduler.reserve(8);
    model.set_stop_hook([this] { on_stop(); });
    model.set_start_hook([this] { on_start(); });
    model.set_script_host(this);
    model.events().set_after_drain([this] { on_end_of_drain(); });
    model.events().set_script_gate(&ScriptRuntime::gate, this);
    model.events().host_signal(&pre_animation_);
    model.events().host_signal(&pre_simulation_);
    model.events().host_signal(&post_simulation_);
    model.events().host_signal(&heartbeat_signal_);
    signals_bound_ = true;
    scheduler.bind(Phase::PreAnimation, [this](double dt) { fire_phase(Phase::PreAnimation, dt); });
    scheduler.bind(Phase::PreSimulation, [this](double dt) { fire_phase(Phase::PreSimulation, dt); });
    scheduler.bind(Phase::PostSimulation, [this](double dt) { fire_phase(Phase::PostSimulation, dt); });
    scheduler.bind(Phase::Heartbeat, [this](double dt) { fire_phase(Phase::Heartbeat, dt); });
}

void ScriptRuntime::detach() {
    close_console();
    if (model_ != nullptr) {
        model_->events().disconnect_scripted();
    }
    close_vm();
    if (model_ != nullptr && signals_bound_) {
        model_->events().release_signal(pre_animation_);
        model_->events().release_signal(pre_simulation_);
        model_->events().release_signal(post_simulation_);
        model_->events().release_signal(heartbeat_signal_);
        signals_bound_ = false;
    }
    if (model_ != nullptr) {
        model_->set_stop_hook(nullptr);
        model_->set_start_hook(nullptr);
        model_->set_script_host(nullptr);
        model_->events().set_after_drain(nullptr);
        model_->events().set_script_gate(nullptr, nullptr);
        model_ = nullptr;
    }
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
    wake_sleeps();
    launch_starts();
    flush_defer();
    resume_budget();
    if (!starts_.empty()) {
        launch_starts();
        resume_budget();
    }
}

bool ScriptRuntime::global_is_nil(const char* name) {
    if (state_ == nullptr || name == nullptr) {
        return true;
    }
    lua_getglobal(state_, "_G");
    lua_getfield(state_, -1, name);
    const bool nil = lua_isnil(state_, -1);
    lua_pop(state_, 2);
    return nil;
}

bool ScriptRuntime::global_number(const char* name, double& out) {
    if (state_ == nullptr || name == nullptr) {
        return false;
    }
    lua_getglobal(state_, "_G");
    lua_getfield(state_, -1, name);
    const bool ok = lua_isnumber(state_, -1);
    if (ok) {
        out = lua_tonumber(state_, -1);
    }
    lua_pop(state_, 2);
    return ok;
}

bool ScriptRuntime::global_boolean(const char* name, bool& out) {
    if (state_ == nullptr || name == nullptr) {
        return false;
    }
    lua_getglobal(state_, "_G");
    lua_getfield(state_, -1, name);
    const bool ok = lua_isboolean(state_, -1);
    if (ok) {
        out = lua_toboolean(state_, -1) != 0;
    }
    lua_pop(state_, 2);
    return ok;
}

ScriptRuntime::Watch ScriptRuntime::watch_global(const char* name) {
    Watch watch;
    if (state_ == nullptr || name == nullptr) {
        return watch;
    }
    lua_getglobal(state_, "_G");
    lua_getfield(state_, -1, name);
    if (auto* ud = static_cast<InstanceUd*>(test_udata(state_, -1, kInstanceMeta))) {
        watch.id = ud->id;
        watch.world = ud->world;
        watch.valid = true;
    }
    lua_pop(state_, 2);
    return watch;
}

DataModel* ScriptRuntime::resolve_watch(Watch watch) const {
    if (!watch.valid) {
        return nullptr;
    }
    return resolve_id(watch.id, watch.world);
}

void ScriptRuntime::on_script_parent(Script& script, InstanceId, InstanceId next) {
    if (model_ == nullptr || !model_->simulation_running() || closing_) {
        return;
    }
    kill_script(script.id());
    if (next != DataModel::kNoParent) {
        enqueue_start(script);
    }
}

void ScriptRuntime::on_script_enabled(Script& script, bool enabled) {
    if (model_ == nullptr || !model_->simulation_running() || closing_) {
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
    return static_cast<Thread*>(lua_getthreaddata(state));
}

bool ScriptRuntime::gate(InstanceId script, std::uint32_t generation, void* userdata) {
    auto* self = static_cast<ScriptRuntime*>(userdata);
    if (self == nullptr || self->model_ == nullptr || self->closing_) {
        return false;
    }
    auto* object = dynamic_cast<Script*>(self->model_->instance(script));
    if (object == nullptr || !object->enabled()) {
        return false;
    }
    return object->start_generation() == generation;
}

static void* adjust_memory(std::size_t& used, std::size_t limit, void* pointer, std::size_t old_size,
                            std::size_t new_size) {
    if (pointer == nullptr) {
        old_size = 0;
    }
    if (new_size == 0) {
        if (old_size <= used) {
            used -= old_size;
        } else {
            used = 0;
        }
        std::free(pointer);
        return nullptr;
    }
    if (old_size > used) {
        return nullptr;
    }
    const std::size_t retained = used - old_size;
    if (retained > limit || new_size > limit - retained) {
        return nullptr;
    }
    void* block = std::realloc(pointer, new_size);
    if (block == nullptr) {
        return nullptr;
    }
    used = retained + new_size;
    return block;
}

void* ScriptRuntime::allocate(void* userdata, void* pointer, std::size_t old_size, std::size_t new_size) {
    auto* self = static_cast<ScriptRuntime*>(userdata);
    if (self == nullptr) {
        if (new_size == 0) {
            std::free(pointer);
        }
        return nullptr;
    }
    return adjust_memory(self->memory_used_, kMemoryLimit, pointer, old_size, new_size);
}

void* ScriptRuntime::allocate_console(void* userdata, void* pointer, std::size_t old_size, std::size_t new_size) {
    auto* self = static_cast<ScriptRuntime*>(userdata);
    if (self == nullptr) {
        if (new_size == 0) {
            std::free(pointer);
        }
        return nullptr;
    }
    return adjust_memory(self->console_memory_used_, kMemoryLimit, pointer, old_size, new_size);
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

void ScriptRuntime::panic(lua_State* state, int) {
    const char* message = lua_tostring(state, -1);
    throw std::runtime_error(message != nullptr ? message : "Luau panic");
}

void ScriptRuntime::on_end_of_drain() {
    if (!open_ || closing_) {
        return;
    }
    assert_lua_thread();
    launch_starts();
    flush_defer();
    resume_budget();
    if (!starts_.empty()) {
        launch_starts();
        resume_budget();
    }
}

void ScriptRuntime::on_start() {
    // The previous session's lines are dropped before this session's scripts run.
    clear_output();
    open_vm();
    sim_clock_ = 0;
    if (model_ == nullptr) {
        return;
    }
    std::vector<Script*> scripts;
    model_->for_each_instance([&](DataModel& instance) {
        if (auto* script = dynamic_cast<Script*>(&instance)) {
            scripts.push_back(script);
        }
    });
    for (Script* script : scripts) {
        enqueue_start(*script);
    }
}

void ScriptRuntime::on_stop() {
    if (model_ != nullptr) {
        model_->events().disconnect_scripted();
    }
    close_vm();
}

void ScriptRuntime::assert_lua_thread() const {
    if (thread_role() == ThreadRole::Render) {
        contract_fail("Lua runs on SimulationThread");
    }
    if (model_ != nullptr && !model_->on_gameplay_thread()) {
        contract_fail("Lua runs on SimulationThread");
    }
    if (model_ != nullptr && model_->prerender_window()) {
        contract_fail("Lua runs on SimulationThread");
    }
}

void open_host_libraries(lua_State* state) {
    for (const luaL_Reg* library = kLibraries; library->func != nullptr; ++library) {
        lua_pushcfunction(state, library->func, nullptr);
        lua_pushstring(state, library->name);
        lua_call(state, 1, 0);
    }
    lua_pushcfunction(state, &ScriptRuntime::lua_print, "print");
    lua_setglobal(state, "print");
    for (const char* const* name = kRemoved; *name != nullptr; ++name) {
        lua_pushnil(state);
        lua_setglobal(state, *name);
    }

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
    lua_pushcfunction(state, &ScriptBindings::connection_gc, "gc");
    lua_setfield(state, connection_mt, "__gc");
    lua_setreadonly(state, connection_mt, 1);

    const int thread_mt = metatable(kThreadMeta);
    lua_pushcfunction(state, &ScriptBindings::thread_index, "index");
    lua_setfield(state, thread_mt, "__index");
    lua_setreadonly(state, thread_mt, 1);

    const int service_mt = metatable(kServiceMeta);
    lua_pushcfunction(state, &ScriptBindings::service_index, "index");
    lua_setfield(state, service_mt, "__index");
    lua_setreadonly(state, service_mt, 1);
    lua_pop(state, 5);

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
        callbacks->panic = &ScriptRuntime::panic;
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
    if (state_ != nullptr || model_ == nullptr) {
        return;
    }
    lua_State* state = create_state(false);
    if (state == nullptr) {
        throw std::runtime_error("could not create the Luau state");
    }
    state_ = state;
    steps_ = 0;
    last_error_.clear();
    open_ = true;
}

void ScriptRuntime::ensure_console() {
    if (console_state_ != nullptr || model_ == nullptr) {
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
    lua_Callbacks* callbacks = lua_callbacks(state);
    callbacks->interrupt = nullptr;
    callbacks->panic = nullptr;
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
    starts_.clear();
    require_cache_.clear();
    loading_.clear();
    threads_.clear();
    lua_Callbacks* callbacks = lua_callbacks(state_);
    callbacks->interrupt = nullptr;
    callbacks->panic = nullptr;
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
    if (model_ != nullptr) {
        model_->events().disconnect_script(id);
    }
    starts_.erase(std::remove_if(starts_.begin(), starts_.end(),
                                 [&](const Start& start) { return start.id == id; }),
                  starts_.end());
    for (Thread& thread : threads_) {
        if (thread.script == id) {
            thread.dead = true;
        }
    }
    drop_dead(ready_);
    drop_dead(sleep_);
    drop_dead(defer_);
    if (model_ == nullptr || closing_) {
        return;
    }
    if (auto* script = dynamic_cast<Script*>(model_->instance(id))) {
        script->bump_start_generation();
    }
}

void ScriptRuntime::enqueue_start(Script& script) {
    if (model_ == nullptr || !open_ || closing_ || !model_->simulation_running()) {
        return;
    }
    if (!script.enabled() || model_->parent(script.id()) == DataModel::kNoParent) {
        return;
    }
    const std::uint32_t generation = script.bump_start_generation();
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
    if (model_ == nullptr || state_ == nullptr) {
        return;
    }
    auto* script = dynamic_cast<Script*>(model_->instance(start.id));
    if (script == nullptr || script->start_generation() != start.generation || !script->enabled()) {
        return;
    }
    if (model_->parent(script->id()) == DataModel::kNoParent || !model_->simulation_running()) {
        return;
    }
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    std::size_t bytecode_size = 0;
    const std::string& source = script->source();
    std::unique_ptr<char, void (*)(void*)> bytecode(
        luau_compile(source.data() != nullptr ? source.data() : "", source.size(), &options, &bytecode_size), std::free);
    if (bytecode == nullptr || bytecode_size == 0) {
        last_error_ = "could not compile script";
        const std::string script_name = model_->name(script->id());
        append_output(OutputKind::Error, script_name.empty() ? last_error_ : script_name + ": " + last_error_);
        return;
    }
    Thread& thread = new_thread(script->id(), start.generation);
    const std::string chunk = "=" + model_->name(script->id());
    const int loaded = luau_load(thread.co, chunk.c_str(), bytecode.get(), bytecode_size, 0);
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

void ScriptRuntime::make_ready(Thread& thread, const char* result) {
    if (thread.dead || thread.co == nullptr) {
        return;
    }
    sleep_.remove(&thread);
    defer_.remove(&thread);
    if (result != nullptr) {
        lua_pushstring(thread.co, result);
        thread.nargs = 1;
    }
    thread.park = Thread::Park::None;
    ready(thread);
}

void ScriptRuntime::make_ready_number(Thread& thread, double result) {
    if (thread.dead || thread.co == nullptr) {
        return;
    }
    sleep_.remove(&thread);
    defer_.remove(&thread);
    lua_pushnumber(thread.co, result);
    thread.nargs = 1;
    thread.park = Thread::Park::None;
    ready(thread);
}

bool ScriptRuntime::thread_ok(const Thread& thread) const {
    if (thread.dead || model_ == nullptr) {
        return false;
    }
    // The command line is not a Script. It stays until the chunk finishes or the VM closes.
    if (thread.script == 0) {
        return true;
    }
    auto* script = dynamic_cast<Script*>(model_->instance(thread.script));
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
    thread.anchor = kAnchorNone;
    lua_State* co = lua_newthread(state_);
    if (co == nullptr) {
        thread.dead = true;
        return thread;
    }
    thread.co = co;
    thread.anchor = lua_ref(state_, -1);
    lua_pop(state_, 1);
    lua_setthreaddata(co, &thread);
    luaL_sandboxthread(co);
    set_script_global(co, script);
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

void ScriptRuntime::append_output(OutputKind kind, std::string text) {
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
    output_.push_back(OutputLine{kind, std::move(text), std::chrono::system_clock::now()});
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
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    std::size_t bytecode_size = 0;
    const char* text = source.data() != nullptr ? source.data() : "";
    std::unique_ptr<char, void (*)(void*)> bytecode(luau_compile(text, source.size(), &options, &bytecode_size),
                                                    std::free);
    if (bytecode == nullptr || bytecode_size == 0) {
        append_output(OutputKind::Error, "could not compile script");
        return;
    }
    lua_State* co = lua_newthread(state);
    if (co == nullptr) {
        append_output(OutputKind::Error, "could not create a script thread");
        return;
    }
    const int anchor = lua_ref(state, -1);
    lua_pop(state, 1);
    luaL_sandboxthread(co);
    const int loaded = luau_load(co, "=console", bytecode.get(), bytecode_size, 0);
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
    if (model_ == nullptr) {
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
    eval_chunk(console_state_, source);
}

int ScriptRuntime::lua_print(lua_State* state) {
    ScriptRuntime* runtime = runtime_from(state);
    const int count = lua_gettop(state);
    std::string line;
    for (int index = 1; index <= count; ++index) {
        std::size_t length = 0;
        const char* text = luaL_tolstring(state, index, &length);
        if (index > 1) {
            line.push_back('\t');
        }
        if (text != nullptr && length > 0) {
            line.append(text, length);
        }
        lua_pop(state, 1);
    }
    line.push_back('\n');
    if (runtime != nullptr) {
        runtime->append_output(OutputKind::Print, std::move(line));
    }
    return 0;
}

void ScriptRuntime::push_instance(lua_State* state, InstanceId id) {
    auto* ud = static_cast<InstanceUd*>(lua_newuserdata(state, sizeof(InstanceUd)));
    ud->id = id;
    ud->world = model_ != nullptr ? model_->world_generation() : 0;
    luaL_getmetatable(state, kInstanceMeta);
    lua_setmetatable(state, -2);
}

DataModel* ScriptRuntime::resolve_id(InstanceId id, std::uint32_t world) const {
    if (model_ == nullptr || world != model_->world_generation()) {
        return nullptr;
    }
    if (id == 0) {
        return model_;
    }
    return model_->instance(id);
}

void ScriptRuntime::fire_phase(Phase phase, double dt) {
    if (!open_ || closing_ || model_ == nullptr) {
        return;
    }
    Signal* signal = phase_signal(phase);
    if (signal == nullptr || !signal->id().valid()) {
        return;
    }
    phase_dt_ = dt;
    model_->events().emit(signal->id(), 0, Field::Name);
}

Signal* ScriptRuntime::phase_signal(Phase phase) {
    switch (phase) {
    case Phase::PreAnimation:
        return &pre_animation_;
    case Phase::PreSimulation:
        return &pre_simulation_;
    case Phase::PostSimulation:
        return &post_simulation_;
    case Phase::Heartbeat:
        return &heartbeat_signal_;
    default:
        return nullptr;
    }
}

void ScriptRuntime::invoke_listener(int ref, InstanceId script, std::uint32_t generation, const char* text,
                                    bool pass_number, double number) {
    if (!open_ || closing_ || state_ == nullptr) {
        return;
    }
    if (!gate(script, generation, this)) {
        return;
    }
    Thread& thread = new_thread(script, generation);
    lua_getref(state_, ref);
    lua_xmove(state_, thread.co, 1);
    if (text != nullptr) {
        lua_pushstring(thread.co, text);
        thread.nargs = 1;
    } else if (pass_number) {
        lua_pushnumber(thread.co, number);
        thread.nargs = 1;
    }
    if (lua_depth_ > 0) {
        ready(thread);
        return;
    }
    resume_one(thread);
}

int ScriptRuntime::require_module(lua_State* state, InstanceId module_id) {
    if (model_ == nullptr) {
        luaL_error(state, "require has no data model");
    }
    auto* module = dynamic_cast<ModuleScript*>(model_->instance(module_id));
    if (module == nullptr) {
        luaL_error(state, "require expects a ModuleScript");
    }
    const auto cached = require_cache_.find(module_id);
    if (cached != require_cache_.end()) {
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

    Thread* caller = ScriptRuntime::thread_from(state);
    const InstanceId owner = caller != nullptr ? caller->script : 0;
    const std::uint32_t generation = caller != nullptr ? caller->generation : 0;
    Thread& thread = new_thread(owner, generation);
    set_script_global(thread.co, module_id);

    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    std::size_t bytecode_size = 0;
    const std::string& source = module->source();
    std::unique_ptr<char, void (*)(void*)> bytecode(
        luau_compile(source.data() != nullptr ? source.data() : "", source.size(), &options, &bytecode_size), std::free);
    if (bytecode == nullptr || bytecode_size == 0) {
        thread.dead = true;
        luaL_error(state, "could not compile ModuleScript");
    }
    const std::string chunk = "=" + model_->name(module_id);
    const int loaded = luau_load(thread.co, chunk.c_str(), bytecode.get(), bytecode_size, 0);
    if (loaded != LUA_OK) {
        remember_error(thread.co);
        const std::string message = last_error_;
        thread.dead = true;
        luaL_error(state, "%s", message.c_str());
    }
    const int status = lua_resume(thread.co, state, 0);
    if (status == LUA_YIELD) {
        thread.dead = true;
        luaL_error(state, "ModuleScript yielded");
    }
    if (status != LUA_OK) {
        remember_error(thread.co);
        const std::string message = last_error_;
        thread.dead = true;
        luaL_error(state, "%s", message.c_str());
    }
    if (lua_gettop(thread.co) <= 0 || lua_isnil(thread.co, 1)) {
        require_cache_[module_id] = LUA_REFNIL;
        lua_pushnil(state);
        return 1;
    }
    lua_pushvalue(thread.co, 1);
    lua_xmove(thread.co, state_, 1);
    const int ref = lua_ref(state_, -1);
    lua_pop(state_, 1);
    require_cache_[module_id] = ref;
    lua_getref(state, ref);
    thread.dead = true;
    return 1;
}

int ScriptBindings::task_wait(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = runtime_from(state);
        ScriptRuntime::Thread* thread = ScriptRuntime::thread_from(state);
        if (runtime == nullptr || thread == nullptr || thread->co != state) {
            luaL_error(state, "task.wait yields the running script thread");
        }
        if (thread->dead) {
            luaL_error(state, "script is dead");
        }
        double dt = 0;
        if (lua_gettop(state) >= 1 && !lua_isnoneornil(state, 1)) {
            dt = luaL_checknumber(state, 1);
        }
        if (dt < 0) {
            dt = 0;
        }
        thread->park = ScriptRuntime::Thread::Park::Sleep;
        thread->due = runtime->sim_clock_ + dt;
        runtime->sleep_.push_back(thread);
        return lua_yield(state, 0);
    });
}

int ScriptBindings::task_spawn(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = runtime_from(state);
        ScriptRuntime::Thread* caller = ScriptRuntime::thread_from(state);
        if (runtime == nullptr || caller == nullptr) {
            luaL_error(state, "task.spawn runs inside a script");
        }
        luaL_checktype(state, 1, LUA_TFUNCTION);
        ScriptRuntime::Thread& child = runtime->new_thread(caller->script, caller->generation);
        const int count = lua_gettop(state);
        for (int index = 1; index <= count; ++index) {
            lua_pushvalue(state, index);
        }
        lua_xmove(state, child.co, count);
        child.nargs = count - 1;
        runtime->ready(child);
        auto* ud = static_cast<ScriptRuntime::Thread**>(lua_newuserdata(state, sizeof(ScriptRuntime::Thread*)));
        *ud = &child;
        luaL_getmetatable(state, kThreadMeta);
        lua_setmetatable(state, -2);
        return 1;
    });
}

int ScriptBindings::task_defer(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = runtime_from(state);
        ScriptRuntime::Thread* caller = ScriptRuntime::thread_from(state);
        if (runtime == nullptr || caller == nullptr) {
            luaL_error(state, "task.defer runs inside a script");
        }
        luaL_checktype(state, 1, LUA_TFUNCTION);
        ScriptRuntime::Thread& child = runtime->new_thread(caller->script, caller->generation);
        const int count = lua_gettop(state);
        for (int index = 1; index <= count; ++index) {
            lua_pushvalue(state, index);
        }
        lua_xmove(state, child.co, count);
        child.nargs = count - 1;
        child.park = ScriptRuntime::Thread::Park::Defer;
        runtime->defer_.push_back(&child);
        auto* ud = static_cast<ScriptRuntime::Thread**>(lua_newuserdata(state, sizeof(ScriptRuntime::Thread*)));
        *ud = &child;
        luaL_getmetatable(state, kThreadMeta);
        lua_setmetatable(state, -2);
        return 1;
    });
}

int ScriptBindings::task_delay(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = runtime_from(state);
        ScriptRuntime::Thread* caller = ScriptRuntime::thread_from(state);
        if (runtime == nullptr || caller == nullptr) {
            luaL_error(state, "task.delay runs inside a script");
        }
        const double dt = luaL_checknumber(state, 1);
        luaL_checktype(state, 2, LUA_TFUNCTION);
        ScriptRuntime::Thread& child = runtime->new_thread(caller->script, caller->generation);
        const int count = lua_gettop(state);
        for (int index = 2; index <= count; ++index) {
            lua_pushvalue(state, index);
        }
        lua_xmove(state, child.co, count - 1);
        child.nargs = count - 2;
        child.park = ScriptRuntime::Thread::Park::Sleep;
        child.due = runtime->sim_clock_ + (dt < 0 ? 0 : dt);
        runtime->sleep_.push_back(&child);
        auto* ud = static_cast<ScriptRuntime::Thread**>(lua_newuserdata(state, sizeof(ScriptRuntime::Thread*)));
        *ud = &child;
        luaL_getmetatable(state, kThreadMeta);
        lua_setmetatable(state, -2);
        return 1;
    });
}

int ScriptBindings::task_cancel(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = runtime_from(state);
        auto* ud = static_cast<ScriptRuntime::Thread**>(test_udata(state, 1, kThreadMeta));
        if (runtime == nullptr || ud == nullptr || *ud == nullptr) {
            luaL_error(state, "task.cancel expects a thread");
        }
        ScriptRuntime::Thread* thread = *ud;
        thread->dead = true;
        runtime->ready_.remove(thread);
        runtime->sleep_.remove(thread);
        runtime->defer_.remove(thread);
        if (ScriptRuntime::thread_from(state) == thread) {
            luaL_error(state, "cancelled");
        }
        return 0;
    });
}

int ScriptBindings::instance_new(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->model_ == nullptr) {
            luaL_error(state, "Instance.new has no data model");
        }
        const char* name = luaL_checkstring(state, 1);
        DataModel* created = nullptr;
        if (std::strcmp(name, "GameObject") == 0) {
            created = &runtime->model_->create<GameObject>();
        } else if (std::strcmp(name, "Script") == 0) {
            created = &runtime->model_->create<Script>();
        } else if (std::strcmp(name, "ModuleScript") == 0) {
            created = &runtime->model_->create<ModuleScript>();
        } else {
            luaL_error(state, "unknown class %s", name);
        }
        runtime->push_instance(state, created->id());
        return 1;
    });
}

int ScriptBindings::require(lua_State* state) {
    return lua_guard(state, [&] {
        ScriptRuntime* runtime = runtime_from(state);
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        if (runtime == nullptr || ud == nullptr) {
            luaL_error(state, "require expects a ModuleScript");
        }
        DataModel* object = runtime->resolve_id(ud->id, ud->world);
        if (object == nullptr) {
            luaL_error(state, "instance is gone");
        }
        return runtime->require_module(state, object->id());
    });
}

int ScriptBindings::instance_tostring(lua_State* state) {
    auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    const char* fallback = "Instance";
    if (runtime == nullptr || runtime->model_ == nullptr || ud == nullptr) {
        lua_pushstring(state, fallback);
        return 1;
    }
    DataModel* object = runtime->resolve_id(ud->id, ud->world);
    if (object == nullptr) {
        lua_pushstring(state, fallback);
        return 1;
    }
    const std::string name = runtime->model_->name(object->id());
    if (!name.empty()) {
        lua_pushlstring(state, name.data(), name.size());
        return 1;
    }
    const char* class_name = object->class_name();
    lua_pushstring(state, class_name != nullptr ? class_name : fallback);
    return 1;
}

void push_registered(lua_State* state, ScriptRuntime* runtime, const LuaSlot& slot, InstanceId id, std::uint32_t world) {
    switch (slot.kind) {
    case LuaSlot::Kind::Nil:
        lua_pushnil(state);
        return;
    case LuaSlot::Kind::Bool:
        lua_pushboolean(state, slot.flag ? 1 : 0);
        return;
    case LuaSlot::Kind::Number:
        lua_pushnumber(state, slot.number);
        return;
    case LuaSlot::Kind::String:
        lua_pushlstring(state, slot.text.data(), slot.text.size());
        return;
    case LuaSlot::Kind::Instance:
        runtime->push_instance(state, slot.id);
        return;
    case LuaSlot::Kind::Vec3:
        lua_pushvector(state, slot.vec.x, slot.vec.y, slot.vec.z);
        return;
    case LuaSlot::Kind::Color:
        push_color(state, slot.color);
        return;
    case LuaSlot::Kind::Transform: {
        lua_newtable(state);
        for (int index = 0; index < 16; ++index) {
            lua_pushnumber(state, slot.transform.m[index]);
            lua_rawseti(state, -2, index + 1);
        }
        return;
    }
    case LuaSlot::Kind::Signal: {
        auto* signal = static_cast<SignalUd*>(lua_newuserdata(state, sizeof(SignalUd)));
        *signal = SignalUd{};
        signal->kind = 0;
        signal->id = id;
        signal->world = world;
        luaL_getmetatable(state, kSignalMeta);
        lua_setmetatable(state, -2);
        return;
    }
    }
    lua_pushnil(state);
}

void push_method(lua_State* state, const LuaField& field) {
    if (field.call == nullptr) {
        lua_pushnil(state);
        return;
    }
    lua_pushcfunction(state, reinterpret_cast<lua_CFunction>(field.call), field.name);
}

int ScriptBindings::instance_index(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        const char* key = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->model_ == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        DataModel* object = runtime->resolve_id(ud->id, ud->world);
        if (object == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        const LuaField* field = lua_class_find(object->class_name(), key != nullptr ? key : "");
        if (field == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        if (field->method) {
            push_method(state, *field);
            return 1;
        }
        LuaSlot slot;
        if (field->read == nullptr || !field->read(*runtime->model_, *object, slot)) {
            lua_pushnil(state);
            return 1;
        }
        push_registered(state, runtime, slot, object->id(), ud->world);
        return 1;
    });
}

int ScriptBindings::instance_newindex(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        const char* key = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->model_ == nullptr) {
            luaL_error(state, "instance is gone");
        }
        DataModel* object = runtime->resolve_id(ud->id, ud->world);
        if (object == nullptr) {
            luaL_error(state, "instance is gone");
        }
        const LuaField* field = lua_class_find(object->class_name(), key != nullptr ? key : "");
        if (field == nullptr || !field->writable || field->write == nullptr || field->type_name == nullptr) {
            luaL_error(state, "cannot set %s", key);
        }
        LuaSlot slot;
        const std::string_view type = field->type_name;
        if (type == "string") {
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, 3, &length);
            slot.kind = LuaSlot::Kind::String;
            slot.text.assign(text != nullptr ? text : "", length);
        } else if (type == "boolean") {
            slot.kind = LuaSlot::Kind::Bool;
            slot.flag = lua_toboolean(state, 3) != 0;
        } else if (type == "Instance") {
            if (lua_isnil(state, 3)) {
                slot.kind = LuaSlot::Kind::Nil;
            } else {
                auto* parent = static_cast<InstanceUd*>(luaL_checkudata(state, 3, kInstanceMeta));
                if (runtime->resolve_id(parent->id, parent->world) == nullptr) {
                    luaL_error(state, "instance is gone");
                }
                slot.kind = LuaSlot::Kind::Instance;
                slot.id = parent->id;
            }
        } else if (type == "Vector3") {
            const float* components = lua_tovector(state, 3);
            if (components == nullptr) {
                luaL_error(state, "%s expects a Vector3", field->name);
            }
            slot.kind = LuaSlot::Kind::Vec3;
            slot.vec = Vec3{components[0], components[1], components[2]};
        } else if (type == "Color") {
            if (!read_color(state, 3, slot.color)) {
                luaL_error(state, "%s expects a table", field->name);
            }
            slot.kind = LuaSlot::Kind::Color;
        } else if (type == "Transform") {
            if (!lua_istable(state, 3)) {
                luaL_error(state, "%s expects a table of 16 numbers", field->name);
            }
            slot.kind = LuaSlot::Kind::Transform;
            slot.transform = transform_identity();
            for (int index = 0; index < 16; ++index) {
                lua_rawgeti(state, 3, index + 1);
                if (!lua_isnumber(state, -1)) {
                    lua_pop(state, 1);
                    luaL_error(state, "%s expects a table of 16 numbers", field->name);
                }
                slot.transform.m[index] = static_cast<float>(lua_tonumber(state, -1));
                lua_pop(state, 1);
            }
        } else {
            luaL_error(state, "cannot set %s", key);
        }
        if (!field->write(*runtime->model_, *object, slot)) {
            luaL_error(state, "property is not available");
        }
        return 0;
    });
}

int ScriptBindings::instance_destroy(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->resolve_id(ud->id, ud->world) == nullptr) {
            luaL_error(state, "instance is gone");
        }
        if (ud->id == 0) {
            luaL_error(state, "cannot destroy the root");
        }
        runtime->model_->destroy(ud->id);
        return 0;
    });
}

int ScriptBindings::instance_children(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->resolve_id(ud->id, ud->world) == nullptr) {
            lua_newtable(state);
            return 1;
        }
        const std::vector<InstanceId> children = runtime->model_->get_children(ud->id);
        lua_newtable(state);
        int index = 1;
        for (InstanceId child : children) {
            runtime->push_instance(state, child);
            lua_rawseti(state, -2, index);
            ++index;
        }
        return 1;
    });
}

int ScriptBindings::instance_find(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        const char* name = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || runtime->resolve_id(ud->id, ud->world) == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        const InstanceId child = runtime->model_->find_first_child(ud->id, name != nullptr ? name : "");
        if (child == 0) {
            lua_pushnil(state);
        } else {
            runtime->push_instance(state, child);
        }
        return 1;
    });
}

int ScriptBindings::instance_isa(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        const char* name = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        DataModel* object = runtime != nullptr ? runtime->resolve_id(ud->id, ud->world) : nullptr;
        if (object == nullptr) {
            lua_pushboolean(state, 0);
            return 1;
        }
        lua_pushboolean(state, is_a(*object, name) ? 1 : 0);
        return 1;
    });
}

int ScriptBindings::instance_service(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<InstanceUd*>(luaL_checkudata(state, 1, kInstanceMeta));
        const char* name = luaL_checkstring(state, 2);
        ScriptRuntime* runtime = runtime_from(state);
        if (runtime == nullptr || ud->id != 0 || runtime->resolve_id(0, ud->world) == nullptr) {
            luaL_error(state, "GetService is on the root DataModel");
        }
        if (name == nullptr || std::strcmp(name, "RunService") != 0) {
            luaL_error(state, "unknown service");
        }
        lua_newuserdata(state, 1);
        luaL_getmetatable(state, kServiceMeta);
        lua_setmetatable(state, -2);
        return 1;
    });
}

int ScriptBindings::signal_connect(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<SignalUd*>(luaL_checkudata(state, 1, kSignalMeta));
        luaL_checktype(state, 2, LUA_TFUNCTION);
        ScriptRuntime* runtime = runtime_from(state);
        ScriptRuntime::Thread* caller = ScriptRuntime::thread_from(state);
        if (runtime == nullptr || runtime->model_ == nullptr || caller == nullptr) {
            luaL_error(state, "Connect runs inside a script");
        }
        if (ud->blocked) {
            luaL_error(state, "%s is not available to scripts", ud->blocked_name);
        }
        if (!runtime->gate(caller->script, caller->generation, runtime)) {
            luaL_error(state, "script is dead");
        }
        lua_pushvalue(state, 2);
        const int ref = lua_ref(state, -1);
        lua_pop(state, 1);
        Signal* signal = nullptr;
        if (ud->kind == 0) {
            if (runtime->resolve_id(ud->id, ud->world) == nullptr) {
                luaL_error(state, "instance is gone");
            }
            signal = &runtime->model_->changed(ud->id);
        } else {
            signal = runtime->phase_signal(static_cast<Phase>(ud->phase));
            if (signal == nullptr) {
                luaL_error(state, "signal is not available");
            }
        }
        const InstanceId script = caller->script;
        const std::uint32_t generation = caller->generation;
        Connection connection = signal->connect_scripted(
            [runtime, ref, script, generation, kind = ud->kind](InstanceId, Field field) {
                if (kind == 0) {
                    runtime->invoke_listener(ref, script, generation, field_name(field), false, 0);
                } else {
                    runtime->invoke_listener(ref, script, generation, nullptr, true, runtime->phase_dt_);
                }
            },
            script, generation, false);
        auto* box = static_cast<Connection*>(lua_newuserdata(state, sizeof(Connection)));
        new (box) Connection(connection);
        luaL_getmetatable(state, kConnectionMeta);
        lua_setmetatable(state, -2);
        return 1;
    });
}

int ScriptBindings::signal_wait(lua_State* state) {
    return lua_guard(state, [&] {
        auto* ud = static_cast<SignalUd*>(luaL_checkudata(state, 1, kSignalMeta));
        ScriptRuntime* runtime = runtime_from(state);
        ScriptRuntime::Thread* thread = ScriptRuntime::thread_from(state);
        if (runtime == nullptr || runtime->model_ == nullptr || thread == nullptr) {
            luaL_error(state, "Wait yields the running script thread");
        }
        if (ud->blocked) {
            luaL_error(state, "%s is not available to scripts", ud->blocked_name);
        }
        // kind 0 is an instance Changed signal. Other kinds are simulation phases
        // on RunService (Heartbeat and the other sim steps). Render phases are
        // already rejected above.
        Signal* signal = nullptr;
        const bool phase = ud->kind != 0;
        if (phase) {
            signal = runtime->phase_signal(static_cast<Phase>(ud->phase));
            if (signal == nullptr) {
                luaL_error(state, "signal is not available");
            }
        } else {
            if (runtime->resolve_id(ud->id, ud->world) == nullptr) {
                luaL_error(state, "instance is gone");
            }
            signal = &runtime->model_->changed(ud->id);
        }
        thread->park = ScriptRuntime::Thread::Park::Signal;
        signal->connect_scripted(
            [runtime, thread, phase](InstanceId, Field field) {
                if (runtime->closing_ || thread->dead) {
                    thread->dead = true;
                    return;
                }
                if (phase) {
                    runtime->make_ready_number(*thread, runtime->phase_dt_);
                } else {
                    runtime->make_ready(*thread, field_name(field));
                }
            },
            thread->script, thread->generation, true);
        return lua_yield(state, 0);
    });
}

int ScriptBindings::connection_disconnect(lua_State* state) {
    return lua_guard(state, [&] {
        auto* connection = static_cast<Connection*>(luaL_checkudata(state, 1, kConnectionMeta));
        connection->disconnect();
        return 0;
    });
}

int ScriptBindings::connection_gc(lua_State* state) {
    auto* connection = static_cast<Connection*>(lua_touserdata(state, 1));
    if (connection != nullptr) {
        connection->~Connection();
    }
    return 0;
}

int ScriptBindings::connection_index(lua_State* state) {
    const char* key = luaL_checkstring(state, 2);
    const LuaField* field = lua_class_find("Connection", key != nullptr ? key : "");
    if (field == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    if (field->method) {
        push_method(state, *field);
        return 1;
    }
    if (field->tag == 1) {
        auto* connection = static_cast<Connection*>(luaL_checkudata(state, 1, kConnectionMeta));
        lua_pushboolean(state, connection->connected() ? 1 : 0);
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

int ScriptBindings::signal_index(lua_State* state) {
    const char* key = luaL_checkstring(state, 2);
    const LuaField* field = lua_class_find("Signal", key != nullptr ? key : "");
    if (field == nullptr || !field->method) {
        lua_pushnil(state);
        return 1;
    }
    push_method(state, *field);
    return 1;
}

int ScriptBindings::service_index(lua_State* state) {
    const char* key = luaL_checkstring(state, 2);
    const LuaField* field = lua_class_find("RunService", key != nullptr ? key : "");
    if (field == nullptr) {
        luaL_error(state, "unknown RunService member");
    }
    auto* ud = static_cast<SignalUd*>(lua_newuserdata(state, sizeof(SignalUd)));
    *ud = SignalUd{};
    ud->kind = 1;
    ud->phase = field->tag;
    ud->blocked = field->blocked;
    if (field->blocked && field->name != nullptr) {
        std::strncpy(ud->blocked_name, field->name, sizeof(ud->blocked_name) - 1);
    }
    luaL_getmetatable(state, kSignalMeta);
    lua_setmetatable(state, -2);
    return 1;
}

int ScriptBindings::thread_index(lua_State* state) {
    lua_pushnil(state);
    return 1;
}

ANARCHY_LUA_REGISTER(register_script_methods) {
    const LuaField methods[] = {
        lua_method("Destroy", "nil", reinterpret_cast<void*>(&ScriptBindings::instance_destroy)),
        lua_method("GetChildren", "Instance", reinterpret_cast<void*>(&ScriptBindings::instance_children), false, false, true),
        lua_method("FindFirstChild", "Instance", reinterpret_cast<void*>(&ScriptBindings::instance_find), false, true, false),
        lua_method("IsA", "boolean", reinterpret_cast<void*>(&ScriptBindings::instance_isa)),
        lua_method("GetService", "", reinterpret_cast<void*>(&ScriptBindings::instance_service), true, false, false),
    };
    register_lua_class("DataModel", nullptr, methods, 5);

    const LuaField signal[] = {
        lua_method("Connect", "Connection", reinterpret_cast<void*>(&ScriptBindings::signal_connect)),
        lua_method("Wait", "nil", reinterpret_cast<void*>(&ScriptBindings::signal_wait)),
    };
    register_lua_class("Signal", nullptr, signal, 2);

    LuaField connected = lua_property("Connected", "boolean", false, nullptr, nullptr);
    connected.tag = 1;
    const LuaField connection[] = {
        lua_method("Disconnect", "nil", reinterpret_cast<void*>(&ScriptBindings::connection_disconnect)),
        connected,
    };
    register_lua_class("Connection", nullptr, connection, 2);

    const LuaField service[] = {
        lua_signal_member("Heartbeat", static_cast<int>(Phase::Heartbeat), false),
        lua_signal_member("PreSimulation", static_cast<int>(Phase::PreSimulation), false),
        lua_signal_member("PostSimulation", static_cast<int>(Phase::PostSimulation), false),
        lua_signal_member("PreAnimation", static_cast<int>(Phase::PreAnimation), false),
        lua_signal_member("PreRender", static_cast<int>(Phase::PreRender), true),
        lua_signal_member("RenderStepped", static_cast<int>(Phase::RenderStepped), true),
    };
    register_lua_class("RunService", nullptr, service, 6);
}

}  // namespace engine_core
