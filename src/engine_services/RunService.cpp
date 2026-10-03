#include "RunService.hpp"

#include "LuaApi.hpp"

#include <iterator>

namespace engine_core {

void RunService::bind(EventQueue& events) {
    if (bound_) {
        return;
    }
    events.host_signal(&pre_animation_);
    events.host_signal(&pre_simulation_);
    events.host_signal(&post_simulation_);
    events.host_signal(&heartbeat_);
    events.host_signal(&render_stepped_);
    events.host_signal(&started_);
    events.host_signal(&stopped_);
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
    events.release_signal(render_stepped_);
    events.release_signal(started_);
    events.release_signal(stopped_);
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
    case Phase::RenderStepped:
        return &render_stepped_;
    default:
        return nullptr;
    }
}

void RunService::fire(EventQueue& events, Phase phase, double dt) {
    Signal* target = signal(phase);
    if (target == nullptr || !target->id().valid()) {
        return;
    }
    dt_[static_cast<int>(phase)] = dt;
    events.emit(target->id(), 0, Field::Name);
}

void RunService::note_frame(double dt) {
    const double ns = dt > 0 ? dt * 1e9 : 0;
    frame_ns_.fetch_add(ns >= 1 ? static_cast<std::uint64_t>(ns) : 1, std::memory_order_relaxed);
}

void RunService::fire_render_stepped(EventQueue& events) {
    const std::uint64_t ns = frame_ns_.exchange(0, std::memory_order_relaxed);
    if (ns == 0) {
        return;
    }
    fire(events, Phase::RenderStepped, static_cast<double>(ns) * 1e-9);
}

namespace {

ANARCHY_LUA_REGISTER(register_run_service_lua) {
    const LuaField fields[] = {
        lua_signal_member("Heartbeat", static_cast<int>(Phase::Heartbeat), false),
        lua_signal_member("PreSimulation", static_cast<int>(Phase::PreSimulation), false),
        lua_signal_member("PostSimulation", static_cast<int>(Phase::PostSimulation), false),
        lua_signal_member("PreAnimation", static_cast<int>(Phase::PreAnimation), false),
        lua_signal_member("PreRender", static_cast<int>(Phase::PreRender), true),
        lua_signal_member("RenderStepped", static_cast<int>(Phase::RenderStepped), false),
        lua_host_signal("Started", HostSignal::Started),
        lua_host_signal("Stopped", HostSignal::Stopped),
    };
    register_lua_class("RunService", nullptr, fields, static_cast<int>(std::size(fields)));
    register_lua_service("RunService");
}

}  // namespace

}  // namespace engine_core
