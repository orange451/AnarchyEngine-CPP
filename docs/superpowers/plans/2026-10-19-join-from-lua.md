# Joining a Server from Lua Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A script on a standalone menu world calls `Connections:Join("host:port")`. Instances marked `Persistent` survive while the server's world replaces everything else. Scripts hear `Joined` and `Left(reason)`.

**Architecture:**
- `Persistent` is a saved boolean on the `DataModel` base. It is never replicated.
- `engine_core` defines `IJoinHost` (join, leave, state). `DataModel` holds a pointer to one. The `Connections` service carries `JoinState` and the `Joined`/`Left` host signals.
- `engine_network::ClientJoinHost` implements `IJoinHost`. It resolves the name on a worker thread, owns a `ClientSession` in a new swap mode, and pumps it through `Engine::set_network`.
- The swap waits for the whole snapshot. A hook on `ReplicaApplier` runs `swap_for_client` and flips the role to Client right before the snapshot's ops apply.
- `DataModel::set_world_role` re-checks every running script's RunContext.
- AnarchyPlayer always installs a `ClientJoinHost`. Studio installs one during Play.

**Tech Stack:** C++17, Luau bindings (`ScriptBindings`, `LuaApi`), Catch2 v3 sandbox, CMake, MSVC.

**Spec:** `docs/superpowers/specs/2026-10-19-join-from-lua-design.md`. It builds on `docs/superpowers/specs/2026-10-13-server-client-design.md`.

## Global Constraints

- Lua names are exactly `Persistent`, `Connections:Join`, `Connections:Leave`, `Connections.Joined`, `Connections.Left`, and `Connections.JoinState` (enum `Idle`, `Joining`, `Joined`). They are Anarchy's own, never Roblox's.
- Exact copy:
  - Join outside a client host: "Only a client can join a server".
  - Join when the world is not standalone and playing: "Join needs a standalone world that is playing".
  - Join while joining or joined: "Already joining or joined; call Leave first".
  - An unresolvable name: "Could not find the server: <resolver error>".
  - Leave: "You left the game".
  - Other `Left` reasons are `ClientSession`'s existing ones, unchanged.
- `engine_core`, `engine_instances` and `engine_services` never include `engine_network`. `AnarchyServer` installs no join host.
- Signals fire on the simulation thread through the event queue (`emit_args`), never from the resolver or network thread.
- `--connect` behavior is unchanged:
  - the project never plays;
  - the world starts empty;
  - the "Connecting…" overlay shows.
- Comments are in the engine's voice. Engine distances are "units". Files are LF. Python is not on PATH.
- Build: `"C:/Program Files/CMake/bin/cmake.exe" --build build --config Release --target sandbox > build/join.log 2>&1; grep -E " error |error C|LNK" build/join.log` (from the worktree `.claude/worktrees/join-from-lua`). Never run sandbox.exe while building.
- Known failures elsewhere: Q6 (fails on main); S38 and A34 (load-sensitive).
- Test names are `JN<n> sentence` with tag `[join]`, plus `[persistent]` where they cover Persistent.
- Commit messages are `Area: what changed, as a sentence`, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Use explicit paths only, and never add `.superpowers/`.

## Review Focus

1. **A resolve that finishes after `Leave()`, a second `Join`, or the host being destroyed.** The stale result is dropped, nothing touches a dead object, and the world is unchanged (JN24, JN25).
2. **A failure while the snapshot is still downloading.** This covers `Leave()`, a refusal, and a lost connection. The menu world is left exactly as it was, Persistent or not (JN22, JN23).
3. **A Persistent script on the default RunContext, `Server`.** It stops at the join, and a `Client` one keeps its state (JN12).
4. **Studio Stop while joined or joining.** The connection ends first, `Left` does not fire, and the captured place comes back (JN26, plus the hand check).
5. **Join twice in one frame, and Join from a server or a client world.** Each refusal errors in the calling script with the exact message (JN17).

---

### Task 1: `Persistent`

**Files:**
- Modify:
  - `src/engine_core/DataModel.hpp/.cpp`: the flag, `persistent()` and `set_persistent()`, and the Lua field;
  - `src/engine_network/Replicator.cpp`: never send `Persistent`;
  - `src/engine_core/LuaApi.cpp`: the doc line.
- Test: `sandbox/join_persistent_tests.cpp` (new; add it to the sandbox list).

**Interfaces:**
- Produces:
  ```cpp
  // DataModel
  bool persistent(InstanceId id) const;
  void set_persistent(InstanceId id, bool persistent);
  ```
- The Lua field is `lua_saved_property("Persistent", "boolean", read, write, "false")` in the DataModel base field list, beside `Archivable`, and not hidden.
- Writing true on a service fails with "<name> is a service, and a service always survives".
- A change emits `Persistent` like `Archivable` does.
- The Replicator leaves the `"Persistent"` row out of a Create's properties and never sends a `Set` for it.

- [ ] **Step 1: Write the failing tests**

- `JN1 Persistent defaults to false, is scriptable, and round-trips a project save`:
  - `Instance.new("Folder").Persistent` is false;
  - after setting it true, a `Project` save and load reads it true;
  - a Folder left false writes no `Persistent` key.
- `JN2 a service refuses Persistent`: `pcall(function() workspace.Persistent = true end)` fails with the exact message.
- `JN3 Persistent never replicates`:
  - a server Folder with Persistent true reaches the client with Persistent false;
  - setting it on the server during play sends no op. Use `Replicator::end_tick` and check that it returns false.

- [ ] **Step 2: Run `[persistent]` and see the tests fail (build error or assertion)**

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run `[persistent]`, `[replication]` and `[project]` (only Q6 may fail)**

- [ ] **Step 5: Commit** `Instances: Persistent marks an instance that survives a world swap, saved but never replicated`

---

### Task 2: The swap, and scripts at a role flip

**Files:**
- Modify:
  - `src/engine_network/ClientWorld.hpp/.cpp`: add `swap_for_client`;
  - `src/engine_core/DataModel.cpp` (`set_world_role`);
  - `src/engine_core/ScriptHost.hpp` and `src/engine_core/ScriptRuntime.hpp/.cpp`: the re-check.
- Test: `sandbox/join_swap_tests.cpp` (new).

**Interfaces:**
- Consumes: `persistent()` from Task 1.
- Produces:
  ```cpp
  // ClientWorld.hpp. clear_for_client, keeping what survives: every instance
  // that is Persistent or under a Persistent ancestor. A survivor whose parent
  // does not survive moves to the service it was under (the topmost ancestor
  // under game). Every survivor gets a fresh GUID. Services reset to defaults.
  void swap_for_client(engine_core::DataModel& game);
  // ScriptHost: the world role changed during play.
  virtual void on_world_role_changed() = 0;
  ```
- `DataModel::set_world_role` calls `script_host->on_world_role_changed()` when the role actually changes while the simulation is running.
- `ScriptRuntime::on_world_role_changed` walks every Script in Workspace, Scripts and Gui:
  - it kills the ones `runs_here` now refuses;
  - it enqueues the enabled ones that run here and were not started;
  - running ones that still run keep their state.
- Fresh GUIDs use the existing GUID maker that `Instance.new` uses.

- [ ] **Step 1: Write the failing tests**

- `JN10 swap_for_client keeps Persistent trees and nothing else`. In a Standalone playing world, build:
  - Workspace/Menu (Folder, Persistent) containing Logo (Folder);
  - Workspace/Map (Folder);
  - Gui/Loading (Persistent).

  After `swap_for_client`:
  - Menu, Logo and Loading are alive, under the same parents;
  - Map is gone;
  - every service is still alive.
- `JN11 a Persistent instance under a cleared folder moves to its service, with a fresh GUID`:
  - Workspace/Level (Folder, not Persistent) containing Hud (Persistent) containing Bar (Folder);
  - after the swap, Hud's parent is Workspace and Bar is under Hud;
  - `guid(Hud)` and `guid(Bar)` differ from before;
  - Level is gone.
- `JN12 at the flip to Client a Persistent Server script stops and a Client script keeps its state`:
  - two Persistent Scripts in Workspace, one `RunContext = Server` and one `Client`;
  - each sets `_G.count += 1` per Heartbeat in its own global (`count_server`, `count_client`);
  - run 5 frames;
  - `set_world_role(Client)`, then run 5 more frames;
  - `count_server` stopped growing and `count_client` reached 10. Use `ScriptRuntime::global_string`, or a number read added if needed.
- `JN13 at the flip back to Standalone a Server script that never ran starts`:
  - a Client world with a Server-context Script whose Source sets `_G.started = true`;
  - `set_world_role(Standalone)`, then run 1 frame;
  - `started` is true.

- [ ] **Step 2: Run `[join]` and see the tests fail**

- [ ] **Step 3: Implement `swap_for_client`, `on_world_role_changed`, and the call in `set_world_role`**

- [ ] **Step 4: Run `[join]`, `[replication]`, `[session]`, `[script]`, `[analysis]`**

  Expected: all pass. The 2c tests that flip roles (RP45c and RP48) are unchanged.

- [ ] **Step 5: Commit** `Network: a swap keeps Persistent trees, and a role change during play stops or starts scripts by RunContext`

---

### Task 3: The Lua API on `Connections`

**Files:**
- Create: `src/engine_core/JoinHost.hpp`.
- Modify:
  - `src/engine_core/DataModel.hpp/.cpp`: the host pointer;
  - `src/engine_services/Connections.hpp/.cpp`: the state, the events and the Lua fields;
  - `src/engine_core/LuaApi.hpp`: `HostSignal::Joined = 7` and `Left = 8`;
  - `src/engine_core/ScriptRuntime.hpp/.cpp`: the signals, emitted the way the history signals are;
  - `src/engine_core/ScriptBindings.hpp/.cpp`: `Join` and `Leave`;
  - `src/engine_core/LuaApi.cpp`: the docs.
- Test: `sandbox/join_api_tests.cpp` (new).

**Interfaces:**
- Produces:
  ```cpp
  // JoinHost.hpp
  enum class JoinState : std::uint8_t { Idle, Joining, Joined };
  // What a program offers scripts for joining a server. engine_core calls it on
  // the simulation thread; it reports back through Connections, on that thread too.
  class IJoinHost {
  public:
      virtual ~IJoinHost() = default;
      // Starts joining; an empty string when started, else why not (Lua raises it).
      virtual std::string join(const std::string& address) = 0;
      virtual void leave() = 0;
  };
  // DataModel
  void set_join_host(IJoinHost* host);   // null: none (the default)
  IJoinHost* join_host() const;
  // Connections
  JoinState join_state() const;
  void set_join_state(JoinState state);           // the host's
  void note_joined();                             // sets Joined, fires Joined
  void note_left(const std::string& reason);      // sets Idle, fires Left(reason)
  ```
- `Connections` keeps two small callback lists, like `ChangeHistoryService`'s `HistorySignal`. `ScriptRuntime::attach` connects to them and `emit_args` onto `HostSignal::Joined` (no args) and `HostSignal::Left` (`text_slot(reason)`).
- `Connections:Join(address)` checks, in order:
  1. no host: "Only a client can join a server";
  2. the world role is not Standalone, or play is not running: "Join needs a standalone world that is playing";
  3. `join_state() != Idle`: "Already joining or joined; call Leave first".

  It then calls `host->join(address)` and raises any non-empty result.
- `Connections:Leave()` calls `host->leave()` when there is a host and the state is not Idle. Otherwise it does nothing.
- `JoinState` is a read-only enum property, using a new `EnumType` named `JoinState` with items `Idle`, `Joining` and `Joined`.

- [ ] **Step 1: Write the failing tests**

All of these use a `FakeJoinHost` in the test file, which records calls and returns a scripted result.

- `JN15 Join hands the address to the host and JoinState reads Joining`:
  - `Connections:Join("example:7777")`;
  - the fake saw `"example:7777"`;
  - the fake calls `set_join_state(Joining)`;
  - Lua reads `Connections.JoinState == Enum.JoinState.Joining`.
- `JN16 Joined and Left reach scripts on the simulation thread`:
  - a script connects both events;
  - the test calls `note_joined()`, then `note_left("The server closed")`, then drains;
  - the script saw Joined, then `"The server closed"`;
  - JoinState is Idle.
- `JN17 Join refuses with the exact messages`. With pcall, check each message:
  - no host;
  - a Server world;
  - a Client world;
  - a stopped world;
  - state Joining.
- `JN18 Leave calls the host only while joining or joined`.

- [ ] **Step 2: Run `[join]` and see the tests fail**

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run `[join]`, `[script]`, `[analysis]`, `[replication]`**

  The API docs test that every Lua member has a doc line must pass.

- [ ] **Step 5: Commit** `Connections: Join, Leave, JoinState, Joined and Left let a script join a server through its program`

---

### Task 4: `ClientJoinHost` and the swap mode in `ClientSession`

**Files:**
- Create: `src/engine_network/ClientJoinHost.hpp/.cpp` (add them to the `engine_network` target).
- Modify:
  - `src/engine_network/Sessions.hpp` and `ClientSession.cpp`: the mode and a state callback;
  - `src/engine_network/ReplicaApplier.hpp/.cpp`: `before_snapshot`;
  - `src/engine_network/README.md`.
- Test: `sandbox/join_host_tests.cpp` (new). It uses `server::ServerHost` on loopback UDP, as `server_tests.cpp` does.

**Interfaces:**
- Consumes: `swap_for_client` (Task 2), `IJoinHost` and the `Connections` notes (Task 3), `ClientSession`, `resolve`, `project_identity`.
- Produces:
  ```cpp
  // Sessions.hpp
  enum class ClientMode { FromEmpty, Swap };
  // In SessionConfig:
  ClientMode client_mode = ClientMode::FromEmpty;   // FromEmpty is --connect, as in 2c
  // ClientSession: called on the simulation thread whenever state() or reason() changes.
  std::function<void(ClientState, const std::string& reason)> on_state;
  // ReplicaApplier: runs once a whole snapshot has arrived and unpacked, just before its ops apply.
  std::function<void()> before_snapshot;
  // ClientJoinHost.hpp
  class ClientJoinHost : public engine_core::IJoinHost {
  public:
      ClientJoinHost(engine_core::Engine& engine, network::GameIdentity identity);
      ~ClientJoinHost() override;               // joins the resolver thread; ends any connection
      std::string join(const std::string& address) override;   // Swap mode
      void leave() override;
      // --connect: clears the world (clear_for_client), FromEmpty mode, connects to an already resolved address.
      bool connect_from_empty(const network::Address& address, std::string& error);
      // Studio Stop: ends any connection without firing Left; the session is unhooked from the engine.
      void end_silently();
      // The session, for the player's overlay; null while idle or resolving.
      engine_network::ClientSession* session();
  };
  ```

Swap mode in `ClientSession`:
- On accept, it does not set the role or start play. Play is already running, and the world stays the menu.
- `applier.before_snapshot` runs `swap_for_client(game)` and then `set_world_role(Client)`. The previous role is Standalone, and it is remembered so a later disconnect puts it back.
- On disconnect it never stops play. The role goes back to Standalone only if the swap happened, with the role flip re-checking scripts (Task 2).

FromEmpty mode is exactly 2c's behavior.

`ClientJoinHost::join`:
1. Bump a generation counter, `set_join_state(Joining)`, and start a resolver thread that runs `resolve(address)`.
2. The thread posts the result with `engine.on_simulation`. It is dropped if the generation has moved on or the host is gone, meaning the destructor joined the thread and set a flag first.
3. On the simulation thread:
   - on error, `note_left("Could not find the server: " + error)`;
   - otherwise, make the socket (`UdpSocket::open(0)`) and a `ClientSession` with `Swap`, `kDefaultSendBudget` and the identity, then `engine.set_network(session)`, then `connect(address, Engine::network_now())`.
4. `on_state` maps the session's states:
   - Joined: `note_joined()`;
   - Disconnected: `note_left(reason)`, then clear the network on the simulation thread with `set_network(nullptr)`, and destroy the session at the next `on_simulation`, never inside its own callback.

`leave()`:
- while resolving: bump the generation and `note_left("You left the game")`;
- while connected: `session->disconnect(now)`, which reports "You left the game" through `on_state`.

`end_silently()` is like leave, but drops `note_left`.

- [ ] **Step 1: Write the failing tests**

Each test runs a client `Engine` (never started, stepped with `run_frame` on the test thread) with a `ClientJoinHost` installed (`set_join_host`), and a `ServerHost` on port 0 serving a temp project. The project has a Server Script that makes Workspace/FromServer. The client world plays standalone with Workspace/Map, Gui/Loading (Persistent), and a Persistent `Client` Script that records the events in globals. Use real-time limits as SV4 does.

- `JN20 Join builds the server's world and keeps the Persistent ones`. `Connections:Join("127.0.0.1:<port>")` from Lua, then step until the Joined event (5 s limit). Then:
  - FromServer exists and Map is gone;
  - Loading exists;
  - the recording script saw Joined;
  - JoinState is Joined;
  - `world_role() == Client`;
  - LocalConnection is set.
- `JN21 Join by name resolves without blocking the frame`. Join `"localhost:<port>"`; the `run_frame` calls keep returning (each under 100 ms) until Joined.
- `JN22 a refused join leaves the menu untouched`. The server is a different game (another gameId). Then:
  - Left fires with "This server runs a different game";
  - Map and Loading are still there with their GUIDs unchanged;
  - the role is Standalone;
  - JoinState is Idle.
- `JN23 Leave while the snapshot is still arriving leaves the menu untouched`:
  - the server has 400 filler Folders and `max_send = 2048`, as in SV4;
  - call Leave when JoinState is Joining and the session is Joining;
  - Left fires with "You left the game", and Map is still there.
- `JN24 a resolve that finishes after Leave is dropped`. Join `"no-such-host.invalid:1"`, then call Leave at once. Left fires once, with "You left the game", and never with a "Could not find" message.
- `JN25 destroying the host while it resolves is safe`. Join a name, then destroy the `ClientJoinHost` right away (after `set_join_host(nullptr)`). Step 60 frames: no crash, and no event.
- `JN26 end_silently while joined ends the connection without Left`. After Joined, `end_silently()`, then step 10 frames:
  - the recording script saw no Left;
  - the server's peer count reaches 0, within 2 s.
- `JN27 after the server closes, the world keeps running locally`:
  - after Joined, `ServerHost::stop()`;
  - Left fires with "The server closed";
  - the role is Standalone;
  - FromServer is still there;
  - a second `Connections:Join` is allowed (no error).
- `JN28 --connect through connect_from_empty behaves as in 2c`:
  - a fresh client world that is not playing;
  - `connect_from_empty(loopback)`;
  - the world is cleared, play starts on accept, and FromServer arrives;
  - Connections' JoinState reads Joined.

- [ ] **Step 2: Run `[join]` and see the tests fail**

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run `[join]` several times, then `[session]`, `[replication]`, `[server]`, `[network]`**

- [ ] **Step 5: Commit** `Network: ClientJoinHost joins a server for scripts, swapping the world once the snapshot is in`

---

### Task 5: The player and Studio install the host

**Files:**
- Modify:
  - `src/player/main.cpp`;
  - `src/ide/IdeLayout.hpp/.cpp` (`start_test`, `stop_test`);
  - `CMakeLists.txt` (the `studio` library links `engine_network` if it doesn't already);
  - `src/engine_services/README.md` or `docs` as the existing Connections doc lives.
- Test: none automated beyond the builds. The hand check is in Task 6.

**Interfaces:**
- Consumes: `ClientJoinHost` (Task 4) and `project_identity`.

Player:
- It always makes a `ClientJoinHost` with the loaded project's identity, unless the identity fails, and installs it with `set_join_host` before the engine starts.
- `--connect` resolves as today, then calls `connect_from_empty` instead of building its own session.
- The overlay reads `host->session()->status()`.
- Shutdown order:
  1. `engine.stop()`;
  2. `set_join_host(nullptr)`;
  3. `set_network(nullptr)`;
  4. destroy the host.

Studio:
- `start_test` makes a `ClientJoinHost` from `project_->` identity. With no project, or an identity error, it installs none, and Join then raises "Only a client can join a server". It installs the host inside the same `on_simulation` that starts play.
- `stop_test` runs `end_silently()` and `set_join_host(nullptr)` inside the `on_simulation` that stops play, before `stop_simulation`, then destroys the host after.

- [ ] **Step 1: Build `AnarchyPlayer`, `AnarchyStudio`, `AnarchyServer` and `sandbox`**

  Expected: no errors.

- [ ] **Step 2: Run `[player]`, `[server]`, `[join]` and the explorer and engine tests the studio touches (`engine-tests`)**

- [ ] **Step 3: Commit** `Player and Studio: scripts can join a server; --connect shares the same host`

---

### Task 6: Whole-suite check, docs, and a hand test

- [ ] **Step 1: Run the full sandbox and build every Release target.** Only Q6 may fail.
- [ ] **Step 2: Update the docs and commit** `Docs: joining from Lua and Persistent`:
  - in `docs/superpowers/networking-parked.md`, add what remains: going back to a menu waits for scenes;
  - in `src/engine_network/README.md`, describe the swap mode.
- [ ] **Step 3: Hand test, with the user** (the controller does this step):
  1. In Studio, build a menu place: a Persistent ScreenGui "Loading", and a Persistent `Client` Script that calls `Connections:Join("localhost:7777")` on a key or a button and prints `Joined` and `Left`.
  2. Run `AnarchyServer` on a project, then Play in Studio, then join. The server's world appears and "Loading" stays.
  3. Ctrl+C the server. `Left` prints "The server closed", and the world keeps running.
  4. Stop Play. The authored place comes back.
