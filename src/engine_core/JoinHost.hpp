#pragma once

#include <cstdint>
#include <string>

namespace engine_core {

// Where a script's join stands: Connections.JoinState. Idle 0, Joining 1, Joined 2.
enum class JoinState : std::uint8_t { Idle = 0, Joining = 1, Joined = 2 };

// What a program offers scripts for joining a server. engine_core calls it on
// the simulation thread; it reports back through Connections, on that thread too.
// A program that cannot join, such as AnarchyServer, installs none.
class IJoinHost {
public:
    virtual ~IJoinHost() = default;
    // Starts joining; an empty string when started, else why not (Lua raises it).
    virtual std::string join(const std::string& address) = 0;
    virtual void leave() = 0;
};

}  // namespace engine_core
