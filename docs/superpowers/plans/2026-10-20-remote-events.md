# Remote Events Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Scripts can fire messages between clients and the server using a `RemoteEvent` instance. Messages are reliable and ordered after the tick's tree changes, or `Unreliable`. Standalone worlds loop them back.

**Architecture:**
- `engine_core` gets a `RemoteValues` model of a fired payload. It is converted to and from Lua, and is carried to handlers by a new `LuaSlot::Kind::Values` that expands into the handler's variadic tail.
- `RemoteEvent` (engine_instances) declares `OnServerEvent(connection, ...)` and `OnClientEvent(...)`. Its Fire methods check the world role. In Standalone they loop back through the event queue; otherwise they hand the message to an `engine_core::IRemoteSink`.
- `ServerSession` and `ClientSession` implement the sink. They encode with a new `RemoteCodec` in engine_network. Reliable messages go on the reliable channel after that tick's Changes message; unreliable ones are packed into the per-tick payload.

**Tech Stack:** C++17, Luau bindings, zstd-free binary codec (ByteBuffer and JsonWire), Catch2 v3 sandbox, CMake, MSVC.

**Spec:** `docs/superpowers/specs/2026-10-20-remote-events-design.md`. It builds on `2026-10-13-server-client-design.md` and `2026-10-19-join-from-lua-design.md`.

## Global Constraints

- Lua names, exactly:
  - the class `RemoteEvent` and its saved boolean `Unreliable` (default false);
  - the methods `FireServer(...)`, `FireClient(connection, ...)`, `FireClientExt(connections, ...)` and `FireClientAll(...)`;
  - the events `OnServerEvent(connection, ...)` and `OnClientEvent(...)`.
- Exact error copy:
  - FireServer on a server: "FireServer is for clients; the server sends with FireClient, FireClientExt or FireClientAll".
  - FireClient, FireClientExt or FireClientAll on a client: "<Method> is for the server; a client sends with FireServer".
  - FireClient with no live Connection: "FireClient needs a Connection of this server".
  - An unsendable value: "<Method> cannot send a <type>". This covers a function, a thread, unknown userdata and a table that contains itself.
  - Too large: "<Method> message is over <cap>" (reliable 262144 bytes; unreliable: what fits one payload).
  - Too deep: "<Method> tables nest deeper than 32".
- Roles: Standalone loops back, Server sends to clients, Client sends to the server. All three are chosen by `world_role()` when the method is called.
- Received messages:
  - A message is accepted only for a replicated RemoteEvent, from a joined client.
  - Each client may have at most 4 MiB of accepted remote bytes in one receive pass; anything past that is dropped and counted.
  - A received message is never grounds for a kick, and never crashes.
- `engine_core`, `engine_instances` and `engine_services` never include `engine_network`.
- Handlers run on the simulation thread through the event queue.
- A game with no RemoteEvents pays nothing measurable.
- Engine distances are "units". Comments are in the engine's voice. Files are LF. Python is not on PATH.
- Build: `"C:/Program Files/CMake/bin/cmake.exe" --build build --config Release --target sandbox > build/remote.log 2>&1; grep -E " error |error C|LNK" build/remote.log`, run from the worktree `.claude/worktrees/remote-events`. Never run sandbox.exe while building.
- Known failures elsewhere: Q6.
- Tests are named `RE<n> sentence` and tagged `[remote]`.
- Commit messages read `Area: what changed, as a sentence`, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Use explicit paths only, and never add `.superpowers/`.

## Review Focus

1. **A reliable message fired in the same tick as a create.** The client handler sees the instance, not nil (RE33).
2. **A message to a client whose snapshot is still arriving.** It is delivered after the snapshot, exactly once (RE34).
3. **A hostile client:**
   - garbage bytes;
   - a remote NetId that is not replicated (one in ServerStorage);
   - a message over the cap;
   - nesting deeper than 32;
   - a flood past 4 MiB in one tick.

   Each is dropped and counted, with no crash and no kick (RE38, RE39).
4. **`FireServer(1, nil)`, and a trailing nil.** The argument count survives on every path: loopback, reliable and unreliable (RE3, RE31, RE41).
5. **An instance argument the other side lacks.** This covers a ServerStorage instance, a client-local instance and one destroyed in flight; each reads nil (RE35).

---

### Task 1: RemoteValues, the RemoteEvent class, and standalone loopback

**Files:**
- Create:
  - `src/engine_core/RemoteValues.hpp/.cpp`: the payload model and its Lua conversion;
  - `src/engine_core/RemoteSink.hpp`: the session interface;
  - `src/engine_instances/RemoteEvent.hpp/.cpp`.
- Modify:
  - `src/engine_core/LuaApi.hpp`: `LuaSlot::Kind::Values`, plus a `shared_ptr<const RemoteValues>` member;
  - the slot-push code in `src/engine_core/ScriptBindings.cpp` (near the `TouchObject` case): a Values slot pushes its values as the handler's trailing arguments;
  - `src/engine_core/DataModel.hpp/.cpp`: `set_remote_sink` and `remote_sink`;
  - `src/engine_core/LuaApi.cpp`: the docs;
  - CMake source lists.
- Test: `sandbox/remote_loopback_tests.cpp` (new; add it to the sandbox list).

**Interfaces:**
- Produces:
  ```cpp
  // RemoteValues.hpp
  struct RemoteValue;
  struct RemoteTable {
      std::vector<RemoteValue> array;                            // 1..#t
      std::vector<std::pair<std::string, RemoteValue>> fields;   // string keys, in any order
  };
  // nil, bool, number and string travel in slot (Nil, Bool, Number, String),
  // as do engine values (Vec3, Vec2, Color, Matrix4, Enum, NumberCurve, ColorCurve)
  // and instances (Instance, by id in this world). A table is in table.
  struct RemoteValue {
      LuaSlot slot;
      std::shared_ptr<const RemoteTable> table;   // non-null: this value is a table
  };
  // A fired payload: every argument, nils included (count = select("#", ...)).
  struct RemoteValues { std::vector<RemoteValue> values; };
  constexpr std::size_t kRemoteMaxDepth = 32;
  constexpr std::size_t kRemoteMaxReliableBytes = 262144;
  // Reads stack[first..top] into out. False with error worded per Global Constraints.
  bool remote_values_from_lua(lua_State* state, int first, const char* method, RemoteValues& out, std::string& error);
  // Pushes every value; returns how many. An Instance id no longer alive pushes nil.
  int push_remote_values(lua_State* state, const DataModel& world, const RemoteValues& values);
  // RemoteSink.hpp: what a session offers RemoteEvent. Called on the simulation thread.
  class IRemoteSink {
  public:
      virtual ~IRemoteSink() = default;
      // Client: to the server. False with error when it cannot be sent (too large).
      virtual bool fire_server(InstanceId remote, std::shared_ptr<const RemoteValues> values, bool unreliable,
                               std::string& error) = 0;
      // Server: to the listed Connections; all=true is every connected client.
      virtual bool fire_clients(InstanceId remote, const std::vector<InstanceId>& connections, bool all,
                                std::shared_ptr<const RemoteValues> values, bool unreliable, std::string& error) = 0;
  };
  // DataModel
  void set_remote_sink(IRemoteSink* sink);   // null by default
  IRemoteSink* remote_sink() const;
  // RemoteEvent (engine_instances)
  bool unreliable() const;  void set_unreliable(bool);   // saved "Unreliable", default false
  ```
- Event declarations, for IntelliSense:
  - `OnServerEvent` has params `{"connection","Connection"}, {"...","any"}`.
  - `OnClientEvent` has params `{"...","any"}`.
  - The last param may be a single `Values` slot. Adjust `check_event_args` so a Values slot satisfies a `"..."` param.
- Method behaviour:
  - Each method checks the role first, raising the exact errors above.
  - It then converts the arguments with `remote_values_from_lua` (raising its error).
  - Standalone:
    - `FireServer` fires `OnServerEvent(nil, values)`.
    - Each `FireClient`, `FireClientExt` or `FireClientAll` call fires `OnClientEvent(values)` exactly once. FireClient's connection is ignored, so nil is accepted.
    - Both go through `DataModel::fire_event`, never inline.
  - Server or Client: the method calls the sink, and raises "Only a networked world can send this" if no sink is installed.
  - `FireClient` checks that the connection is a live `ConnectionInstance` under Connections before calling the sink. `FireClientExt` drops entries that fail that check.
- Size is checked in the sink (Task 3), since only the codec knows the encoded size. Depth is checked here.

- [ ] **Step 1: Write the failing tests**

All of these run in a Standalone playing world with ScriptRig.

- `RE1 RemoteEvent is creatable and Unreliable is saved`:
  - `Instance.new("RemoteEvent").Unreliable == false`;
  - when it is set true, a project save and load keeps it true.
- `RE2 in a standalone world FireServer reaches OnServerEvent with a nil connection`:
  - A script connects `OnServerEvent(function(c, a, b) _G.got = {c == nil, a, b} end)` and fires `FireServer(5, "x")`.
  - After a frame, `got` reads `{true, 5, "x"}`.
- `RE3 the argument count survives, trailing nils included`:
  - `FireServer(1, nil)` delivers `select("#", ...) == 2` after the connection;
  - `FireServer()` delivers 0.
- `RE4 FireClientAll and FireClient loop back to OnClientEvent once per call`:
  - `FireClientAll(1)`, `FireClient(nil, 2)`, `FireClientExt({}, 3)`;
  - `OnClientEvent` saw 1, 2, 3, in order.
- `RE5 tables, engine values and instances arrive intact`:
  - fire `{1, 2, {nested = Vector3.new(1, 2, 3)}, name = "n", [true] = 1}`, plus `Color3.new(1, 0, 0)`, plus `workspace`;
  - the array is `{1, 2, ...}` and nested.nested is the Vector3;
  - the name field is kept and the boolean key is dropped;
  - the Color3 is equal and the instance is workspace.
- `RE6 unsendable values raise on the sender`:
  - `FireServer(function() end)` raises "FireServer cannot send a function";
  - a self-containing table raises "FireServer cannot send a table that contains itself";
  - 33 levels of nesting raise "FireServer tables nest deeper than 32".
- `RE7 delivery waits for the event queue`: the handler has not run when `FireServer` returns, and it has after the script yields.

- [ ] **Step 2: Run `[remote]` and see the tests fail**

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run `[remote]`, `[lua]`, `[join]`, `[replication]`.** The API docs test passes.

- [ ] **Step 5: Commit** `Instances: RemoteEvent fires typed events, looping back in a standalone world`

---

### Task 2: RemoteCodec

**Files:**
- Create: `src/engine_network/RemoteCodec.hpp/.cpp`, added to the target.
- Modify: `src/engine_network/TreeCodec.hpp` (`MessageType::Remote = 3`) and the README.
- Test: `sandbox/remote_codec_tests.cpp` (new).

**Interfaces:**
- Consumes: `RemoteValues` (Task 1), `ByteWriter`/`ByteReader`, `JsonWire`, and `slot_to_json`/`slot_from_json` (PropertyReflection.hpp) for engine values.
- Produces:
  ```cpp
  // A remote message: [MessageType::Remote][kCodecVersion][varuint netid][values].
  // Instance values are NetIds on the wire; the two callbacks map them.
  using IdToNet = std::function<std::uint32_t(engine_core::InstanceId)>;     // 0: not on the other side → nil
  using NetToId = std::function<engine_core::InstanceId(std::uint32_t)>;      // 0: unknown → nil
  void encode_remote(std::uint32_t remote_netid, const engine_core::RemoteValues& values, const IdToNet& to_net,
                     ByteWriter& out);
  // False on any malformed input, a depth over 32, or more than max_bytes; never throws, never crashes.
  bool decode_remote(ByteReader& in, std::uint32_t& remote_netid, engine_core::RemoteValues& out, const NetToId& to_id,
                     std::size_t max_bytes);
  ```
- Value encoding:
  - one tag byte, then: nil; false or true; a number (f32 if exact, else f64, as JsonWire does); a string (varuint length plus bytes); an instance (varuint netid); an engine value (the slot kind byte plus the `slot_to_json` value in JsonWire binary);
  - a table: varuint array count and the values, then varuint field count and (string, value) pairs.
- An Enum carries its enum type name so `slot_from_json` can rebuild it.

- [ ] **Step 1: Write the failing tests**
  - `RE10 every value kind round-trips`: nil, both booleans, 0.1, 1e300, an empty string, a 1000-character string, Vector3, Vector2, Color3, Matrix4, an EnumItem, a NumberCurve, an instance (through identity maps), and nested tables. The argument count includes trailing nils.
  - `RE11 an instance the other side lacks decodes as nil`: `to_net` returns 0, so the instance decodes as Nil.
  - `RE12 hostile bytes fail without crashing`. Each case returns false:
    - truncation at every byte of a valid message;
    - 10 000 random buffers (seeded);
    - a declared length past the end;
    - depth 33;
    - an unknown tag;
    - a message larger than `max_bytes`.

- [ ] **Step 2: Run `[remote]` and see the tests fail**

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run `[remote]`, `[replication]`**

- [ ] **Step 5: Commit** `Network: RemoteCodec encodes a fired payload, instances by NetId, and refuses bad bytes`

---

### Task 3: Reliable remotes through the sessions

**Files:**
- Modify: `src/engine_network/Sessions.hpp`, `ServerSession.cpp`, `ClientSession.cpp`, `ReplicaApplier.hpp/.cpp` (a reverse map lookup for client-to-server ids, if missing), README.
- Test: `sandbox/remote_session_tests.cpp` (new). It uses the session test pattern from `replication_session_tests.cpp`: LinkHub and a virtual clock.

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces:
  - `ServerSession` and `ClientSession` both implement `engine_core::IRemoteSink`. Each calls `game.set_remote_sink(this)` in its constructor and clears it in its destructor.
  - `std::size_t ServerSession::dropped_remotes() const;` counts remote messages from clients that were refused.
- Server send:
  - `fire_clients` encodes once per call, with `to_net` = the raw InstanceId when `Replicator::replicated(id)`, else 0.
  - It checks the encoded size against 262144. If over, it raises (returns false with the exact error).
  - It then queues `(peers, message)` in an outbox.
  - In `send()`, after `end_tick` pushes the tick's Changes, each outbox message is pushed to each target peer that has joined. A peer not yet joined has it held and pushed after its snapshot is queued. Messages are pushed through the same per-peer queue, so ordering and backpressure are the existing ones.
- Client send:
  - `fire_server` encodes with `to_net` = the applier's reverse map (local id to netid, 0 if none) and `remote` = the remote's netid.
  - The message is queued and sent in `send()` with `Endpoint::send_message`. If the window is full, it is held for the next send.
- Server receive (`on_message`):
  1. Decode with `max_bytes` 262144. `to_id` maps a netid to an InstanceId when it is alive and `replicated`.
  2. Accept only if the peer has joined, the remote id is a live `RemoteEvent` that is replicated, and that peer's bytes this receive pass, plus this message, are at most 4194304.
  3. Otherwise drop the message and count it in `dropped_remotes`.
  4. On accept, fire `OnServerEvent(connection_of(peer), values)`.
  5. Non-remote client messages keep counting in `dropped_messages`.
- Client receive: a `MessageType::Remote` message decodes with the applier's `local(netid)` map, then fires `OnClientEvent`. A message whose remote is unknown is dropped. A decode failure is "Bad data from the server", like any other message from the server.

- [ ] **Step 1: Write the failing tests** (a server and one or two joined clients, both playing; their scripts record into globals)
  - `RE30 a client's FireServer reaches OnServerEvent with its Connection`: the connection argument equals `connection_of(peer)`, and the values are intact.
  - `RE31 FireClient, FireClientExt and FireClientAll reach the right clients`. With clients A and B:
    - `FireClient(A)` reaches only A;
    - `FireClientExt({A, B})` reaches both;
    - `FireClientAll` reaches both;
    - the argument count survives.
  - `RE32 the wrong side raises`: `FireServer` on the server, and `FireClient` on a client, each raise the exact text.
  - `RE33 a reliable message fired in the same tick as a create finds the instance`: the server creates Workspace/Ball and calls `FireClientAll(ball)` in one script run; the client handler receives the Ball, not nil.
  - `RE34 a message to a joining client arrives after its snapshot, once`:
    - the server has a 400-folder world and the client a low budget, so the snapshot spans many ticks;
    - the server calls `FireClientAll(1)` while the client is still Joining;
    - the client's handler runs once, after its world holds the folders.
  - `RE35 instances the other side lacks read nil`: a ServerStorage Folder sent to a client reads nil, and a client-local Folder sent to the server reads nil.
  - `RE36 an oversized reliable message raises on the sender`: a 300 KB string raises "FireServer message is over 262144".
  - `RE37 messages on one remote and across remotes keep their order`: 100 `FireServer(i)` calls across two remotes arrive in firing order.
  - `RE38 hostile client messages are dropped and counted`. A raw client endpoint sends:
    - garbage;
    - a Remote message naming a ServerStorage RemoteEvent;
    - depth 33;
    - an oversize message.

    No handler runs, `dropped_remotes()` grows by 4, the client stays connected and no crash occurs.
  - `RE39 a flood past 4 MiB in one pass is cut off`:
    - a raw client sends 5 MiB of valid messages that land in one receive;
    - the handler runs for at most 4 MiB of them, the rest are counted, and the client stays connected.

- [ ] **Step 2: Run `[remote]` and see the tests fail**

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run `[remote]` several times, then `[session]`, `[replication]`, `[join]`, `[network]`**

- [ ] **Step 5: Commit** `Network: RemoteEvents travel reliably between clients and the server, after the tick's tree changes`

---

### Task 4: Unreliable remotes

**Files:**
- Modify: `src/engine_network/ServerSession.cpp`, `ClientSession.cpp`, `Sessions.hpp` and the README.
- Test: `sandbox/remote_session_tests.cpp`.

**Interfaces:**
- Consumes: Task 3's sinks and the codec.
- A remote whose `Unreliable` was true when fired takes the payload path:
  - Each tick, a peer's pending unreliable messages are packed into one payload: `[varuint count]` then, for each message, `[varuint length][message bytes]`.
  - The packing stops at `Endpoint::payload_budget(peer, now)`, and the rest are dropped and counted (`dropped_unreliable()` on each session).
  - A single message larger than an empty payload's budget makes the Fire method raise "<Method> message is over <budget>". The budget is checked against `kMaxPayloadBytes - 1` at fire time.
- Receive is through `on_payload`, with the same decode and validation as reliable messages (Task 3), and the same 4 MiB per pass counted together.
- Server to client: unreliable messages to a peer that has not joined are dropped. Until the snapshot arrives they cannot name anything. *(Spec: unreliable makes no promise.)*
- The receiver accepts either path whatever its own copy of `Unreliable` says.

- [ ] **Step 1: Write the failing tests**
  - `RE40 an unreliable FireServer arrives through the payload`: with no conditioner loss, 10 fires across 10 ticks all arrive, and `dropped_unreliable` is 0.
  - `RE41 unreliable keeps the argument count and values`: `(1, nil)`, a table, and an instance that exists on both sides.
  - `RE42 under 50% loss unreliable messages are lost, never resent, and never crash`: fire 100 messages, then check the received count is between 20 and 80 and that no reliable resend count grows from them.
  - `RE43 an unreliable message too big for a payload raises`: a 2000-byte string raises.
  - `RE44 many unreliable messages in one tick pack until the budget, the rest dropped and counted`.

- [ ] **Step 2: Run `[remote]` and see the tests fail**

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run `[remote]`, `[session]`, `[network]`**

- [ ] **Step 5: Commit** `Network: Unreliable RemoteEvents ride the per-tick payload and are never resent`

---

### Task 5: Whole-suite check, docs, and a hand test

- [ ] **Step 1: Run the full sandbox and build every Release target.** Only Q6 may fail.
- [ ] **Step 2: Docs.** Update `src/engine_network/README.md` (remote messages), the networking roadmap (sub-project 3 done) and `docs/superpowers/networking-parked.md` (what remains). Commit `Docs: remote events`.
- [ ] **Step 3: Hand test, with the user** (the controller does this step):
  1. Make a project with a RemoteEvent in Workspace, a Client script that fires it on a key press, and a Server script that answers with `FireClient(connection, "pong")`.
  2. Run it in AnarchyServer and join from Studio Play using the menu script from the join hand test. Pressing the key prints "pong" on the client.
  3. Play the same project standalone in Studio. The loopback answers too.
