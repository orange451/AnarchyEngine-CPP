# Server and Client

2026-10-13 · sub-project 2 of the networking roadmap (`2026-10-12-networking-roadmap-design.md`). A headless server runs a place; players connect to it and see the server's world, kept in step as scripts change it.

## Goal

A game author writes a place, marks each script Server, Client, or Any, and runs it on `AnarchyServer`. Players start `AnarchyPlayer --connect host:port`, receive the server's world, and watch it change as the server's scripts change it. Client scripts run on each player's machine against that world. Nothing about names, characters, or accounts is the engine's: each connection is a `Connection` with a UUID, and the rest is the game's.

The same place still runs single-player in Studio and the player exactly as today.

## Decisions

| Question | Decision |
| --- | --- |
| Where it lives | A new library `src/engine_network/` (`engine_network`), the only engine code that knows about the network. It links `engine_core` and `anarchy_network`. Two executables: `src/server/` (`AnarchyServer`) and the existing player gains a client mode. |
| How replication works | A change journal: `DataModel` gets one optional sink called from its existing funnels, and the server turns each tick's changes into one binary message, encoded once and sent to every joined client on the reliable channel. Join is a binary snapshot of the filtered tree, compressed with zstd. |
| Script placement | `Script.RunContext`, an `Enum.RunContext`: `Server` (default), `Client`, `Any`. Where a script may run (Workspace, Scripts, Gui) is unchanged. |
| World roles | Every world has a role: `Standalone` (Studio Play, the player without `--connect`), `Server`, or `Client`. Standalone runs scripts of every RunContext, so existing places behave as today. Server runs `Server` and `Any`; Client runs `Client` and `Any`. |
| Server-only content | `ServerStorage`, a service inside `Storage`. Nothing in it replicates or runs scripts. Server-context scripts elsewhere replicate as shells: the instance and its other properties, with `Source` empty. |
| Client content | `ClientStorage`, a service inside `Storage` beside `ServerStorage`. It replicates like the rest of the place and runs no scripts; it is the conventional home for what clients use (GUIs to clone, effects, client modules). Replication gives it no rule of its own. |
| Who is connected | A hidden game service, `Connections`, holding a `Connection` per connected client. `Connection.Id` is a random UUID the server makes; `Connections.LocalConnection` is the client's own. No players, names, or characters. |
| Client writes | Allowed locally, never sent. The server's next write to that property overwrites the client's value. Instances a client creates stay on the client. Clients change shared state only through remotes (sub-project 3) or inputs (sub-project 4). |
| Physics on clients | Not simulated in this sub-project. The server's physics writes are quiet, so they do not replicate; bodies on a client stay where a script last put them until sub-project 4 replicates motion. Script-set transforms replicate. |
| Values | Exact, at the precision the engine stores them. Tree replication never quantizes. |
| Identity in the handshake | `gameId` and `version` from `project.json`, and `hash_resources` over the project's resources folder. A zero game id or a zero hash refuses to start the server and to connect. |
| Not here | Remotes (3), physics motion and inputs (4), Studio multi-client testing (6), authentication (7), a lobby or server browser, interest management for the tree (every client gets the whole filtered tree). |

## Architecture

### 1. `engine_core` and `engine_instances` changes

**Change sink.** `DataModel::State` gains `std::function<void(const TreeChange&)> change_sink`, empty by default. It is called from the existing funnels and only when set:

```
struct TreeChange {
    enum class Kind : std::uint8_t { Parented, Destroyed, Renamed, Property };
    Kind kind;
    InstanceId id;
    InstanceId previous_parent = 0;   // Kind::Parented: where it was (DataModel::kNoParent if nowhere)
    Field field = Field::Name;        // Kind::Property: the changed field
    std::uint64_t payload = 0;        // Kind::Property, Field::Reflected: lua_property_id
};
```

`create<T>()` makes an instance unparented, and every caller then sets its name and properties and parents it, so there is no "created" event: Parented fires from `set_parent` (including the first parenting out of `kNoParent`), Destroyed from `destroy`, Renamed from `set_name`, and Property from `emit_change` for every other field (script, user, and load writes, both built-in fields such as `Transform` and `Source` and `Field::Reflected` registry properties). The Replicator decides what is new: an instance that becomes replicated by a Parented change and is not yet known to clients is a create of its subtree. Quiet writers (physics `store_simulated`, `store_pose`, `write_simulated_transform`, `store_visual_transform`, the pose blend) do not call it. The sink reads nothing else; consumers read current values from the tree when they encode.

Stop (`restore_place_unlocked`) and undo revival change the tree without these funnels. The server never stops play while it serves, and a client stops play only when its connection ends, so neither path runs during a session; Studio's Play does not replicate.

**RunContext.** `Script` gains `RunContext` (`Enum.RunContext`: `Server`, `Client`, `Any`; default `Server`; saved like other properties). `DataModel` gains `world_role()` / `set_world_role(WorldRole)` (`Standalone` default). `ScriptRuntime::runs_here` additionally requires `context_runs(script.run_context(), world_role())`. Changing `RunContext` during play starts or stops the script as toggling `Enabled` does.

**`RunService:IsServer()` and `IsClient()`.** Server role: true, false. Client role: false, true. Standalone: true, true.

**`ClientStorage` and `ServerStorage`.** Two services inside `Storage`, in that order, each with a fixed GUID (`clientstorage`, `serverstorage`), nested as the asset categories are in `Assets`: services but not scene services, so they cannot be moved, renamed, or destroyed, and no script in them runs (their top ancestor is `Storage`). Both save with the place, show in the Explorer with Storage's icon, and are reached as `game.Storage.ServerStorage` or `game:GetService("ServerStorage")` (likewise ClientStorage). A project saved when `ServerStorage` was a child of game loads it into `Storage`.

**`Connections` and `Connection`.** `Connections` is a hidden game service (like `Assets`): always under `game`, not movable or destroyable, not shown by the Explorer while it has no children. It holds only `Connection` instances. `Connection` has `Id` (string, read-only to scripts, a UUID `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` in lowercase hex). `Connections.LocalConnection` is a read-only `Connection?`: nil on the server and in Standalone, the client's own after joining. Only the engine creates or destroys Connections; a script's `Instance.new("Connection")` is refused. Neither saves with the place.

**Audio off.** `Engine` gains a way to construct without an audio device (`AudioWorld(false)` exists); the server uses it.

### 2. `engine_network`

**`TreeCodec`.** Encodes and decodes, with `network::BitWriter`/`BitReader`:

- **Values**, one encoder per `LuaSlot` kind: Nil, Bool, Number (64-bit), String (length-prefixed UTF-8), Instance (NetId, 0 for nil), Vec3, Color, Matrix4, Enum, Vec2, NumberCurve, ColorCurve, at the engine's stored precision.
- **Names** through a per-connection string table: the first use of a class or property name sends `Define {id, text}`, later uses send the id. Each side keeps the table for the life of the connection.
- **Operations**: `Create {netid, class, parent netid, name, properties[]}`, `Destroy {netid}`, `Parent {netid, parent netid}`, `Rename {netid, name}`, `Set {netid, property, value}`.
- **Messages**: `SnapshotPart {header on the first part, last flag, ops}` and `Changes {server tick, ops}`. The snapshot header carries the codec version, the server tick, and the joining client's Connection NetId.

A NetId is the server's `InstanceId` raw value: unique while alive, generation-tagged, so a reused slot never reads as the instance that held it.

Decoding never trusts input: a truncated message, an unknown string id, an unknown class or property name, an out-of-range enum, or a value of the wrong kind for its property fails the whole message.

**`Replicator` (server).** Holds the journal for the current tick, appended by the change sink. At tick end it coalesces:

- writes to one property keep the last value;
- an instance created this tick becomes one `Create` with its end-of-tick properties and later writes to it this tick fold in;
- created and destroyed in the same tick sends nothing;
- a move into `ServerStorage` (or under any filtered ancestor) is a `Destroy` for clients, a move out is a `Create` of the whole subtree;
- parents are created before children, and a `Destroy` follows the writes it cancels.

Filtering: `ServerStorage` and everything under it, `Core`, and the `Source` of `Server`-context scripts (a shell keeps every other property; a change of `RunContext` to or from Server re-sends `Source` accordingly). The result is encoded once per tick and the same bytes go to every joined client.

On join it encodes the filtered tree as `Create` operations in parent-before-child order, compresses with zstd, and splits it into `SnapshotPart` messages of at most 1 MB, each a reliable message. Changes from the following tick go to that client too and arrive after the last part, in order.

**`ReplicaApplier` (client).** Buffers snapshot parts until the last, decompresses, and builds the place into its empty `DataModel` with the same privileged path it uses for changes. It keeps `NetId → InstanceId`. Each `Changes` message is applied at the start of the client's tick, before scripts. The privileged path may write any property (including ones read-only to scripts), fires the normal signals (`Changed`, property signals, `ChildAdded`, `ChildRemoved`, `Destroying`), and records no undo. An operation on an unknown NetId is skipped and counted; an instance reference to an unknown NetId reads nil and resolves when that instance is created.

**`NetworkSession`.** Owns the `network::Endpoint` and, by role, the Replicator or the ReplicaApplier. `Engine` gains an optional `INetworkPump*`: `receive(now)` runs at the start of `run_step`, before PreAnimation; `send(now)` at its end, after audio. Both under the frame's write lock. On the server:

- a peer connects: create a `Connection` with a fresh UUID under `Connections`; the snapshot goes out at the end of that tick;
- a peer disconnects or times out: destroy its `Connection`;
- a reliable message from a client: dropped and counted (sub-project 3 gives it meaning);
- a client whose reliable send window is full (it has fallen about 4 s behind): disconnected.

On the client:

- the snapshot completes: set the world role to Client, set `Connections.LocalConnection`, start play;
- a malformed message, an unknown class or property, or a codec-version mismatch: disconnect and report the reason;
- the connection ends for any reason: stop play and report it.

### 3. Executables

**`AnarchyServer <project folder> [--port 7777] [--max-connections 64]`.** Loads the project, computes the identity (refusing to start if the game id or resources hash is zero), opens a UDP socket, builds an `Engine` with no renderer, no client sync, no audio device, `set_pace_hz(60)`, and world role Server, starts play, and runs. Script output goes to stdout with a timestamp. Ctrl+C (or SIGTERM) disconnects every client and exits. A port in use, a missing project, or a bad identity prints the reason and exits non-zero.

**`AnarchyPlayer --connect host:port [project folder]`.** Takes the identity from the packed game (or the folder), creates an empty place, connects, and waits for the snapshot; the window shows "Connecting…" until then. Failures and disconnects show their reason in the window: wrong game, needs an update (with the server's version), server full, timed out, server closed, bad data from the server. Without `--connect` the player runs Standalone as today.

## Error handling summary

| Situation | Behavior |
| --- | --- |
| Snapshot bigger than one block | Split into ≤1 MB parts; the reliable channel's 8 MB reassembly budget and one-block-at-a-time rule are never approached |
| Client falls behind | Disconnected when its reliable send window is full |
| Malformed or mismatched data from the server | Client disconnects with "bad data from server" |
| Data from a client | Dropped and counted |
| Operation on an instance the client destroyed locally | Skipped and counted |
| Reference to a filtered or not-yet-created instance | Reads nil; resolves on creation |
| Server shutdown | Every client disconnected with "server closed" |
| Zero game id or zero resources hash | Server refuses to start; client refuses to connect |

## Testing

Catch2 in the sandbox, tags `[replication]`, `[server]`, plus existing tags for touched code:

- **Codec**: round-trips for every value kind and operation; the string table across messages; truncated, garbage, unknown-name, and wrong-kind input fail the message without crashing.
- **Two worlds in one process** over `network::LinkHub` (a server `DataModel` and one or more client `DataModel`s, stepped by hand): script changes on the server appear on the client; `ServerStorage` contents and Server-script source never arrive; a Server script arrives as a shell; coalescing (last write wins, create-then-destroy sends nothing, moves into and out of `ServerStorage`); references that arrive late or point at filtered instances; Connections created and destroyed with UUIDs; `LocalConnection` set on the client and nil on the server; `RunContext` gating in all three roles, including a RunContext change during play; client writes stay local and are overwritten by the server's next write; client `Instance.new("Connection")` refused. A helper asserts a client's tree equals the server's filtered tree at quiet points.
- **Failure paths**: a client that stops acking is disconnected; a corrupted message disconnects the client with its reason; a client reliable message is dropped and counted.
- **Benchmarks (Release)**: a 10,000-instance place's snapshot encode, decode, and compressed size; 1,000 property changes per tick encoded and sent to 50 clients in under 1 ms of server time; a client applies a tick's changes in under 0.5 ms; no heap allocation on the server's steady-state change path beyond the journal's reused buffers.
- **Executables**: a smoke test starts `AnarchyServer` on a small project, connects a headless client over loopback UDP, sees a server script's change arrive, sends Ctrl+C, and checks the client sees "server closed". `AnarchyPlayer --connect` is checked by hand at the end.

## Performance targets

| Measure | Target |
| --- | --- |
| Server replication cost, 1,000 changes per tick, 50 clients | under 1 ms per tick |
| Client apply cost per tick | under 0.5 ms |
| Join snapshot, 10,000 instances | encoded in under 50 ms and under 1 MB compressed. It arrives at the transport's block rate, one 1,000-byte fragment per tick (about 60 KB/s at 60 Hz), so a 600 KB snapshot takes about 10 s; raising that rate is parked in `docs/superpowers/networking-parked.md` and is not this sub-project's |
| Standalone cost of this sub-project | none measurable: the change sink is empty and RunContext is one comparison per script start |
