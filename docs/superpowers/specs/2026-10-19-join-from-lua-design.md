# Joining a Server from Lua

## Why

A game made with Anarchy launches its client standalone, on a world the developer built: a main menu, a server browser, whatever they want. Today the only way into a server is the `AnarchyPlayer --connect host:port` command line, which leaves no room for that menu. A script on the menu needs to ask the engine to join a server. While the server's world loads, the menu's loading screen and the script driving it must stay alive.

## Goals

- A script in a standalone, playing world can join a server by address. It hears when the server's world has arrived, and it hears when the connection ends and why.
- A new `Persistent` property on every instance marks what survives a world swap. Joining is the first swap; loading a scene (future work) will be the next.
- Joining never freezes the game: looking up a host name happens off the simulation thread.

## Non-goals

- Going back to the menu after leaving. The engine leaves the world as it is and tells the game's scripts; scenes will later let a game load its menu scene.
- A server browser, server lists, or discovery.
- Joining from a server, or joining while already joined (both are refused).
- Studio multi-client testing (roadmap sub-project 6).

## Anti-goals

- A join that blocks the frame loop, or freezes the window on a slow DNS answer.
- A join that destroys the loading screen before the server's world is there, leaving a blank or half-built frame with nothing to tell the player what is happening.
- A failed join that damages the menu world. A refused or unreachable server must leave the menu exactly as it was.

## The design

### `Persistent`

- A new property on every instance (the `DataModel` base, beside `Archivable`): `Persistent`, boolean, false by default. It is saved and loaded like any property, shown in Properties, and scriptable.
- At a world swap, an instance survives when it or an ancestor is `Persistent`. Everything else that a swap clears is destroyed. Services always survive.
- A Persistent instance under an ancestor that does not survive is moved, at the swap, to be a direct child of the service it was under (the topmost ancestor under `game`), keeping its name and children. *(My call.)*
- Survivors get fresh GUIDs at the swap. The menu is often loaded from the same project the server serves, so a surviving loading screen could otherwise share a GUID with an instance the server is about to create. After the swap the survivors are the client's own, local instances. *(My call.)*
- `Persistent` is local: it is not replicated. A server's instances replicate without it, and a client's Persistent instances are never sent anywhere. *(My guess, approved in conversation.)*
- A surviving Script keeps running through the swap with its state. It is still subject to RunContext: after a join the world is a Client, so a Persistent script must be `Client` or `Any` to keep running. The default `Server` would stop it. This rule is documented on `Persistent` and `Connections:Join`.

The swap is `clear_for_client` extended. Today it destroys every non-service descendant outside Core and resets service properties to their defaults. With this change it skips survivors, moves and re-GUIDs them as above, and still resets services to defaults.

### The Lua API, on `Connections`

`Connections` (the existing hidden service, `game:GetService("Connections")`) gains:

- `Connections:Join(address: string)`: starts joining `"host:port"`.
  - It errors at once when the world is not standalone and playing, or a join is already under way.
  - The address is resolved off the simulation thread.
  - When the server accepts, the swap runs, the world becomes a Client, and the snapshot builds the server's world.
  - When the server refuses, or the address cannot be found or reached, nothing in the world changes and `Left` fires with the reason.
- `Connections:Leave()`: ends the connection or the join in progress. `Left` fires with "You left the game". Does nothing when not joining or joined.
- `Connections.Joined`: an event that fires once the server's world has arrived (the snapshot is applied). `LocalConnection` is set by then.
- `Connections.Left(reason: string)`: an event that fires when a join fails or a connection ends. Its reasons are the client reasons that exist today ("The server is full", "This server runs a different game", "The server closed", "Removed by the server", "Your connection could not keep up", "Could not reach the server", "Lost connection to the server", "You left the game", ...), plus "Could not find the server: …" for a name that does not resolve.
- `Connections.JoinState`: read-only enum `Idle`, `Joining`, `Joined`.

The existing `LocalConnection` is unchanged.


### When the swap runs

The swap waits until the whole snapshot has arrived and unpacked. The world is cleared and the server's world built in one step, right before the snapshot is applied. *(My call.)*

- While the snapshot downloads, the menu world keeps playing untouched, so its loading screen animates. `JoinState` reads `Joining`.
- Any failure before that point leaves the menu exactly as it was: a refused connect, an unreachable server, a timeout, `Leave()`, or a corrupt snapshot part. `Left` fires with the reason.
- Today `ClientSession` sets the Client role on accept. For a Lua join, the role flips at the swap instead, and play is already running.
- After the swap, failures are ordinary disconnects: see "After leaving".

### Scripts at the role flip

Today `set_world_role` does not stop or start any script; RunContext is only checked when a script starts. The swap must re-check every surviving Script after the flip to Client:
- Scripts that no longer run here (`Server`) stop.
- `Client` and `Any` scripts keep running with their state.

*(Needed for the Persistent rule above; in scope.)*

### `--connect` and Studio Stop

- `--connect` stays as it is today. The project's own world never plays, the world starts empty, and the "Connecting…" overlay shows. Only its plumbing moves onto the same host implementation that `Join` uses. *(My call: `--connect` is a developer tool and its behavior was just tested.)*
- In Studio, stopping Play ends any connection silently. `Left` does not fire, because every script is being stopped anyway. The connection ends before the captured place is restored. *(My call.)*

### After leaving

The engine clears nothing; the world keeps running locally (the user's decision). Concretely:

- What the server built stays. The world role goes back to Standalone, so physics steps again and loose bodies may fall or roll.
- At this flip, scripts are re-checked as in "Scripts at the role flip":
  - Scripts that now run here start. These are the replicated `Server`-context scripts, which arrived with empty Source and do nothing.
  - Running `Client` and `Any` scripts keep their state.
- `Left` fires. The game's Persistent scripts decide what to show.
- Another `Join` is allowed from there. Its swap clears the leftover world like any other.

### Who makes the connection

`engine_core` must not include `engine_network`. So `engine_core` defines a small host interface: join this address, leave, and report state, joined and left. The Lua binding reaches it through the `Game`. The program that hosts the world installs an implementation that owns a `ClientSession`, its UDP socket, and the game's identity (`project_identity` of the loaded project).

- **AnarchyPlayer** installs one always. `--connect` becomes a thin use of the same path: it calls the host's join at start.
- **Studio** installs one during Play, so a developer can test a menu's join against a local `AnarchyServer`. When Play stops, any connection ends and the world returns to the captured place as usual. *(My call: Studio support is cheap and lets developers test without exporting.)*
- **AnarchyServer** installs none. `Join` there errors with "Only a client can join a server".
- A host with no implementation, such as a bare `Engine` in tests, errors the same way unless a test installs one.

The session is pumped by the engine as today (`Engine::set_network`). The host sets it when a join starts and clears it on the simulation thread after the connection ends.

## Constraints

- The Lua names are Anarchy's own (`Join`, `Leave`, `Joined`, `Left`, `JoinState`, `Persistent`), not Roblox's.
- Comments in the engine's voice. Engine distances are "units".
- No allocation or blocking on the frame loop for a join beyond what a `ClientSession` already does. Name resolution runs on a worker thread, and its result is handed back to the simulation thread.
- Events fire on the simulation thread, inside the normal script resume, never from the network thread or the resolver thread.
- Existing behavior unchanged: `--connect`, plan 2c's tests, saving and loading of projects without `Persistent`, and standalone play.

## Left to the builder

- The exact host-interface names and where the `Game` holds the pointer.
- How the resolver thread hands its result back. Something it already posts through, such as `Engine::on_simulation`, is fine.
- Test names and the split of tests between `[replication]`, `[session]`, `[project]` and a new tag.
