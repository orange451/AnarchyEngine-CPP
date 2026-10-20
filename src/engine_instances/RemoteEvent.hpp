#pragma once

#include "DataModel.hpp"

namespace engine_core {

// A channel scripts send messages through: a client to the server with
// FireServer, the server to clients with FireClient, FireClientExt, and
// FireClientAll. OnServerEvent and OnClientEvent fire with what was sent. A
// standalone world loops each message back to itself
// (docs/superpowers/specs/2026-10-20-remote-events-design.md).
//
// Unreliable, saved, false by default: when true, messages may be lost or
// arrive out of order, and none is resent. It is read when a message is fired.
//
// The methods are engine_core's (RemoteEventBindings.cpp), which has Luau.
// class_name is defined in RemoteEvent.cpp so that file, and its Lua
// registration, stays linked.
class RemoteEvent final : public DataModel {
public:
    RemoteEvent(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override;

    bool unreliable() const { return unreliable_; }
    // SimulationThread.
    void set_unreliable(bool unreliable);

protected:
    void on_reuse() override {
        unreliable_ = false;
        DataModel::on_reuse();
    }

private:
    bool unreliable_ = false;
};

}  // namespace engine_core
