#include "RunService.hpp"

#include "LuaApi.hpp"

namespace engine_core {

void RunService::bind(EventQueue& events) {
    if (bound_) {
        return;
    }
    events.host_signal(&pre_animation_);
    events.host_signal(&pre_simulation_);
    events.host_signal(&post_simulation_);
    events.host_signal(&heartbeat_);
    bound_ = true;
}

void RunService::release(EventQueue& events) {
    if (!bound_) {
        return;
    }
    events.release_signal(pre_animation_);
    events.release_signal(pre_simulation_);
    events.release_signal(post_simulation_);
    events.release_signal(heartbeat_);
    bound_ = false;
}

Signal* RunService::signal(Phase phase) {
    switch (phase) {
    case Phase::PreAnimation:
        return &pre_animation_;
    case Phase::PreSimulation:
        return &pre_simulation_;
    case Phase::PostSimulation:
        return &post_simulation_;
    case Phase::Heartbeat:
        return &heartbeat_;
    default:
        return nullptr;
    }
}

void RunService::fire(EventQueue& events, Phase phase, double dt) {
    Signal* target = signal(phase);
    if (target == nullptr || !target->id().valid()) {
        return;
    }
    dt_ = dt;
    events.emit(target->id(), 0, Field::Name);
}

namespace {

ANARCHY_LUA_REGISTER(register_run_service_lua) {
    const LuaField fields[] = {
        lua_signal_member("Heartbeat", static_cast<int>(Phase::Heartbeat), false),
        lua_signal_member("PreSimulation", static_cast<int>(Phase::PreSimulation), false),
        lua_signal_member("PostSimulation", static_cast<int>(Phase::PostSimulation), false),
        lua_signal_member("PreAnimation", static_cast<int>(Phase::PreAnimation), false),
        lua_signal_member("PreRender", static_cast<int>(Phase::PreRender), true),
        lua_signal_member("RenderStepped", static_cast<int>(Phase::RenderStepped), true),
    };
    register_lua_class("RunService", nullptr, fields, 6);
    register_lua_service("RunService");
}

}  // namespace

}  // namespace engine_core
