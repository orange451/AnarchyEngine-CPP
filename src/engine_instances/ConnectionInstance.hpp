#pragma once

#include "DataModel.hpp"

#include <string>

namespace engine_core {

// One client connected to a server. The server makes it under Connections
// when the client joins and destroys it when the client leaves; scripts read
// it and never make one. Id is a random UUID the server chose. Names,
// characters, and accounts are the game's to keep, keyed by Id.
// Scripts see the class as Connection; C++ calls it ConnectionInstance because
// engine_core::Connection is already an event subscription (Events.hpp).
// class_name is defined in ConnectionInstance.cpp so that file, and its Lua registration, stays linked.
class ConnectionInstance : public DataModel {
public:
    ConnectionInstance(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    const char* class_name() const override;

    const std::string& connection_id() const { return connection_id_; }
    // The engine's: replication sets it once, before parenting.
    void set_connection_id(std::string uuid);

protected:
    void on_release() override;
    void on_reuse() override;

private:
    std::string connection_id_;
};

// A random version-4 style UUID, lowercase hex with dashes.
std::string make_connection_uuid();

}  // namespace engine_core
