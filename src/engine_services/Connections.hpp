#pragma once

#include "ChangeHistoryService.hpp"
#include "JoinHost.hpp"
#include "SceneService.hpp"

#include <string>

namespace engine_core {

// Who is connected to a networked game: one Connection per client, made and
// destroyed by the engine. A child of game outside the place, like Core: never
// saved, untouched by Play and Stop. The explorer shows it only while it holds
// a Connection. LocalConnection is a client's own Connection, nil on a server
// and in single-player.
//
// It is also where a script joins a server: Join and Leave go to the
// program's IJoinHost (DataModel::join_host), and the host reports back here,
// on the simulation thread, with set_join_state, note_joined, and note_left.
class Connections : public Service {
public:
    using Service::Service;
    const char* class_name() const override;
    bool hidden_in_explorer() const override;

    InstanceId local_connection() const;
    // The engine's: replication sets it when a client's world arrives.
    void set_local_connection(InstanceId id);

    JoinState join_state() const;
    // The host's. Changed fires for JoinState when the state really changes.
    void set_join_state(JoinState state);
    // Sets Joined, then fires on_joined.
    void note_joined();
    // Sets Idle, then fires on_left with why the join ended.
    void note_left(const std::string& reason);

    // ScriptRuntime turns these into the Joined and Left signals scripts hear.
    HistorySignal<> on_joined;
    HistorySignal<const std::string&> on_left;

private:
    InstanceId local_connection_ = 0;
    JoinState join_state_ = JoinState::Idle;
};

}  // namespace engine_core
