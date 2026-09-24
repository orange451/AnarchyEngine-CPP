#include "Runner.hpp"

#include "../engine_core/LuaEngine.hpp"
#include "../engine_core/Engine.hpp"

#include <stdexcept>
#include <utility>

namespace runner {

Runner::Runner() = default;

Runner::~Runner() = default;

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
