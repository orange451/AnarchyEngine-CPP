# Remote Events

Networking roadmap sub-project 3 (`2026-10-12-networking-roadmap-design.md`). It builds on the server and client of sub-project 2 (`2026-10-13-server-client-design.md`) and on joining from Lua (`2026-10-19-join-from-lua-design.md`).

## Why

A server's world now reaches its clients, but a client cannot say anything back. The server drops every message a client sends. A game can show the server's world, but a player cannot press a button that does something on the server. A game also has no way to send a client a message that is not a change to the tree: "play this effect", "you scored". Remote events give a game's scripts both directions.

## Goals

- A `RemoteEvent` instance that scripts fire and listen to: client to server, and server to one client, a list of clients, or every client.
- An `Unreliable` mode for messages sent every frame (aim direction, a held input), where a lost or late message is replaced by the next one.
- A reliable message that mentions an instance arrives after that instance exists on the client.
- Good editor IntelliSense: every method and event has one fixed signature, so the editor knows whether the first argument is a Connection or the payload.
- The same scripts work in a standalone world (single player, Studio Play without a server).

## Non-goals

- Remote functions: calls that wait for an answer. Not wanted, in either direction.
- Rate limiting and per-message validation. These are the game's scripts' job.
- Physics state, interest management, prediction (sub-projects 4 and 5).

## Anti-goals

- A hostile client that can crash the server or exhaust its memory through a remote.
- A reliable message whose instance argument reads nil only because the instance's creation had not arrived yet.
- An API where a handler's parameters depend on which side runs it, so IntelliSense cannot type it.

## The design

### The instance

- **Class:** `RemoteEvent`. It is creatable in the editor and with `Instance.new("RemoteEvent")`. It replicates like any other instance, so a RemoteEvent in ServerStorage never reaches clients and clients cannot use it.
- **Property:** `Unreliable`, a saved boolean, false by default.
  - False: messages are reliable and arrive in the order they were fired. Order is kept across every reliable remote, since they share one ordered channel.
  - True: messages may be lost or arrive out of order, and none is resent.
- **Methods** (the user's names):
  - `remote:FireServer(...)`: client to server.
  - `remote:FireClient(connection, ...)`: server to one client.
  - `remote:FireClientExt(connections, ...)`: server to each Connection in the array.
  - `remote:FireClientAll(...)`: server to every connected client.
- **Events** (the user's names):
  - `remote.OnServerEvent(connection, ...)` fires on the server, with the sending Connection first.
  - `remote.OnClientEvent(...)` fires on a client.
- **Calling the wrong side's method** raises an error that names the right one. For example: "FireServer is for clients; the server sends with FireClient, FireClientExt or FireClientAll". *(My call.)*
- **Bad connection arguments are skipped:** a `connection` that is not a live Connection of this server, or an array entry that is not one. `FireClient` with no valid connection raises an error. `FireClientExt` skips bad entries and raises nothing. *(My call.)*

### Arguments

- **Types:** any number of arguments (the reliable size cap still applies). Each one may be:
  - nil, a boolean, a number or a string;
  - a value of any engine property type (Vector3, Color3, CFrame/Matrix, EnumItem, and the rest);
  - an Instance;
  - a table of these, nested.
- **Not sendable:** functions, threads, userdata the engine does not know, and tables that contain themselves. Firing with one of these raises an error on the sending side.
- **Number exactness:** numbers keep their exact value, as tree replication does. They are sent as 32-bit floats when that is exact.
- **Instances travel as references.** One the other side does not have reads as nil. That covers anything in ServerStorage, a client's own local instances, and an instance destroyed in flight. An instance the server receives from a client is resolved only among the instances replicated to that client.
- **Tables:** a table's array part and its string keys survive. Other key types are dropped. *(My call.)*

### Ordering (the user's decision)

A reliable message from the server is sent after that tick's tree changes, on the same ordered channel. A reliable message fired in the same tick as a create therefore finds the instance already on the client. A message fired while a client's snapshot is still arriving waits behind the snapshot, just as tree changes do.

Unreliable messages make no such promise. An instance they mention that has not arrived yet reads nil.

Client-to-server reliable messages are ordered among themselves.

### Standalone (the user's decision)

In a standalone world, remotes loop back inside the world:
- `FireServer(...)` fires `OnServerEvent(nil, ...)`.
- `FireClient`, `FireClientExt` and `FireClientAll` fire `OnClientEvent(...)` once per call, whatever connections they were given. *(Once per call is my call.)*
- Loopback is delivered through the event queue, after the firing script yields or finishes, never inside the call. *(My call: the same as a real network message.)*

### Limits (my call; the engine's protection, not rate limiting)

- **Size caps:** a reliable message may be at most 256 KB encoded. An unreliable message must fit in one packet's payload, about 1 KB. Firing past either cap raises an error on the sending side.
- **Table nesting:** at most 32 levels deep.
- **Values per message:** at most 65536, counting every argument, every table and every value inside one at any depth. A decoded value takes a few hundred bytes however small it is on the wire, so without this a 256 KB message of tiny values would unpack to about 90 MB. Firing past it raises "<Method> message has more than 65536 values"; a received one is dropped and counted.
- **Values per receive:** the server accepts at most 262144 decoded values from one client in one receive pass (`SessionConfig::max_remote_values_per_pass`). Past that, that receive's messages are dropped and counted, as past the 4 MB byte cap.
- **A client's waiting messages:** a client holds at most 4 MB of `FireServer` messages the server has not taken yet. When a server stops acking, `FireServer` past that raises "FireServer cannot send: too many messages are waiting for the server".
- **Received message checks:** the server checks every received message the same way. It drops a message that breaks a cap, names a RemoteEvent that client was never sent, or fails to decode, and counts it. It never kicks the client and never crashes.
- **Rate limiting:** none. That stays the game's.

### Transport

- **Reliable messages** use the reliable channel, interleaved with the tree messages as a new message type.
- **Unreliable messages** ride the per-tick payload. Several fired in one tick pack into that payload, and the ones that do not fit are dropped and counted.
- **Addressing:** a RemoteEvent is named by its NetId.
- **Clients' messages:** the server now reads them, where until now it dropped every one. Anything that is not a valid remote message is still counted and dropped.


### Settled details (my calls, from the builder check)

- **Which side is which.** Everything keys on the world role.
  - Standalone (single player, Studio Play with no server): loopback.
  - Server (a world hosting a `ServerSession`): `FireClient*` and `OnServerEvent`.
  - Client (`--connect`, or after a Lua join): `FireServer` and `OnClientEvent`.

  The wrong-side errors follow the role too. The firing path reaches the session through an `engine_core` interface, installed only in Server and Client worlds, as joining does.
- **A client that has connected but not finished joining.** A message for it is held and sent after its snapshot, whether it was fired before or after the snapshot was queued.

  The snapshot is taken at the end of the tick and includes the RemoteEvent, so such a message never names something the client lacks. `FireClient` to such a connection is valid.
- **What the server accepts from a client.** A message is accepted only if it names a RemoteEvent that is replicated (alive, not in ServerStorage or Core) and the client has joined. A client's instance arguments resolve against the same replicated set. There is no per-client record of what was sent.
- **The memory bound on received messages.** This is protection, not rate limiting. Each client may have at most 4 MB of received remote messages waiting for their handlers to run. Past that, its new messages are dropped and counted until the backlog drains. A client is never kicked for this.
- **What the caps measure.** The size caps measure the whole encoded message, header included. On the server, a message sent to many clients counts once per client against that client's queue bytes, as tree changes do.
- **Argument count is kept.** `FireServer(1, nil)` delivers two arguments, so `select("#", ...)` agrees on both sides.
- **Tables:**
  - A table's array part travels up to its length as Lua's `#` gives it. Elements after a hole past that length are lost.
  - String keys travel.
  - Other key types are dropped.
- **Which channel is used.** `Unreliable` is read when the message is fired, on the sending side. The receiver accepts a message on either channel, so a property changed mid-flight never drops a message.

## Constraints

- Lua names exactly as above: `RemoteEvent`, `Unreliable`, `FireServer`, `FireClient`, `FireClientExt`, `FireClientAll`, `OnServerEvent`, `OnClientEvent`.
- Handlers run on the simulation thread, inside the normal event queue. They never run from the network thread.
- Decoding never trusts input: every length is checked, and a bad message is dropped whole, never a crash. This is the same rule as the tree codec.
- `engine_core`, `engine_instances` and `engine_services` never include `engine_network`. The firing path reaches the session through an interface, as joining does.
- No measurable cost for a game that uses no remotes.
- Every method and event gets a docs entry with typed parameters, for IntelliSense.

## Left to the builder

- The wire layout of a remote message, and how values are encoded. Reusing the tree codec's value encoding is preferred.
- How fired messages are queued between a script's call and the session's send.
- Test names, tags and file layout.
