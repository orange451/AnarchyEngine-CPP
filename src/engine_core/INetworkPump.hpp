#pragma once

namespace engine_core {

// A networked game's connection to the engine loop: a server or a client
// session. The engine calls receive at the start of each step, before
// anything else runs, and send at its end, after audio, both on the engine
// thread under the frame's write lock. now is seconds on a steady clock.
class INetworkPump {
public:
    virtual ~INetworkPump() = default;
    virtual void receive(double now) = 0;
    virtual void send(double now) = 0;
};

}  // namespace engine_core
