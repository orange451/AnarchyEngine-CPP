#include "Runner.hpp"

#include "LuaEngine.hpp"
#include "Engine.hpp"
#include "SceneFeed.hpp"
#include "ScriptRuntime.hpp"

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

Runner::Runner() : feed_(std::make_unique<SceneFeed>()) {}

Runner::~Runner() { silencePrint(lua_.get()); }

void Runner::prepare() {
    if (lua_ != nullptr) {
        throw std::logic_error("runner already started");
    }
    auto lua = std::make_unique<engine_core::LuaEngine>();
    lua->start();
    auto simulation = std::make_unique<engine_core::Engine>();
    // Neither thread has a fixed rate. Each waits for the Scene View's paint, so
    // the place steps as often as the window draws: 120 times a second on a
    // 120 Hz display. An empty step cannot run ahead of the picture.
    simulation->set_simulation_pace_hz(0.0);
    simulation->set_render_pace_hz(0.0);
    simulation->set_simulation_client_sync(true);
    simulation->set_render_client_sync(true);
    simulation->set_renderer(feed_.get());
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
