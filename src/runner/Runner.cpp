#include "Runner.hpp"

#include "../engine_core/LuaEngine.hpp"
#include "../engine_core/Engine.hpp"
#include "../engine_core/ScriptRuntime.hpp"

#include <stdexcept>
#include <utility>

namespace runner {
namespace {

// The handler points at the simulation's script log. Drop it before that log is destroyed.
void silencePrint(engine_core::LuaEngine* lua) {
    if (lua != nullptr) {
        lua->setPrintHandler(nullptr);
    }
}

}  // namespace

Runner::Runner() = default;

Runner::~Runner() { silencePrint(lua_.get()); }

void Runner::prepare() {
    if (lua_ != nullptr) {
        throw std::logic_error("runner already started");
    }
    auto lua = std::make_unique<engine_core::LuaEngine>();
    lua->start();
    auto simulation = std::make_unique<engine_core::Engine>();
    // Heartbeat stays at 60 Hz. The render thread has no 60 Hz sleep. It waits
    // for each Scene View paint so an empty step cannot run ahead of the picture.
    simulation->set_simulation_pace_hz(60.0);
    simulation->set_render_pace_hz(0.0);
    simulation->set_render_client_sync(true);
    // The IDE console reads the script log. Sandbox print shares that log with play scripts.
    engine_core::ScriptRuntime* scripts = &simulation->scripts();
    lua->setPrintHandler([scripts](std::string_view text) {
        scripts->append_output(engine_core::ScriptRuntime::OutputKind::Print, std::string(text));
    });
    lua_ = std::move(lua);
    simulation_ = std::move(simulation);
}

void Runner::start() {
    if (lua_ == nullptr) {
        prepare();
    }
    if (threadsStarted_) {
        throw std::logic_error("runner already started");
    }
    try {
        simulation_->start();
    } catch (...) {
        lua_->stop();
        lua_.reset();
        simulation_.reset();
        throw;
    }
    threadsStarted_ = true;
}

void Runner::stop() {
    silencePrint(lua_.get());
    simulation_.reset();
    lua_.reset();
    threadsStarted_ = false;
}

engine_core::LuaEngine& Runner::lua() {
    if (lua_ == nullptr) {
        throw std::logic_error("runner is not started");
    }
    return *lua_;
}

engine_core::Engine& Runner::simulation() {
    if (simulation_ == nullptr) {
        throw std::logic_error("runner is not started");
    }
    return *simulation_;
}

}  // namespace runner
