# Networking: parked work

Left over from sub-project 1 (transport, `src/network/`), for the sub-projects that build on it. Each line says what and which sub-project should pick it up.

## Before sub-project 2 ships a headless server

- ~~`UdpSocket::open` makes only a dual-stack IPv6 socket. A host with IPv6 disabled (Linux booted with `ipv6.disable=1`) cannot open one at all; add an IPv4 fallback.~~ Done in 2c: `open` falls back to `open_ipv4`.
- ~~Refuse to start (or to connect) with an all-zero `game_id` or a `resources_hash` of 0. `hash_resources` returns 0 on any read error, and two unset ids would match.~~ Done in 2c: `project_identity` refuses both, in the server and the player.
- If the `gameId` migration write fails (a read-only old project), each load makes a fresh id, so two processes opening that project refuse each other. Decide whether that needs a warning.

## Sub-project 2: wiring callbacks and the engine loop

- ~~`Endpoint::read_connection` is not exception-safe: a throwing callback leaves `reading_` set and the slot never frees. Add a scope guard, or forbid throwing callbacks.~~ Done in 2c: the pending drop is made and the exception propagates.
- A client callback that disconnects and then reconnects in the same callback gets `false` from `connect()` until the packet finishes. Document it.
- ~~No `Endpoint::close(now)`: destroying an Endpoint tells no peer, so clients wait 5 s. Add one that disconnects every peer.~~ Done in 2c: `close` tells each peer `Closed`.
- ~~`Endpoint::peers()` allocates a vector per call; a per-tick caller loses the no-allocation property N71 checks. Offer an allocation-free iteration.~~ Done in 2c: `for_each_peer`.
- ~~Join speed: one fragment per packet and one packet per tick caps a join block near 60 KB/s (a 2 MB tree takes about 35 s). Decide whether a peer may send more than one packet per tick while a block is in flight.~~ Done in 2c: several packets a tick while reliable data is due, under a per-client budget (1 MB/s by default) and an in-flight cap.
- ~~The reliable section is not limited by flow control (only the payload budget is); block transfers run at one fragment per tick regardless of the budget.~~ Done in 2c: the byte credit is refilled at the flow-control rate.
- Save As still writes `project.json` from a fresh document, dropping keys the code does not know (pre-existing; the load migration now keeps them).

## Sub-project 4: physics state sync

- `payload_budget` uses the Endpoint-wide `send_interval`. Inputs at 30 Hz on a 60 Hz server need the interval passed per call.
- No `on_lost(sequence, tag)`: a sleep update that never arrives has to be detected by the caller's own timeout ring. Consider reporting loss.
- `PacketRing::loss` trusts tiny samples: a lost accept can read as ~100% loss at startup and halve flow control for about 5 s. Require a minimum count.
- `ReliableChannel::has_due`, `bytes_due`, and `write` walk every fragment of a queued block each call; a per-block cursor of the first unacked fragment would make that constant.

## Sub-project 6: Studio multi-client testing

- The conditioner's seed is always 1 and not exposed: every client drops the same packets in lockstep. Put it in `EndpointConfig` or seed from the Endpoint's random source.
- Expose each peer's address for the stats panel.

## Sub-project 7: authentication and encryption

- A `ConnectDeny` carries no client salt, so anyone who can spoof the server's address can deny a connecting client.
- A malicious peer can still hold about 24 MB of reassembly per connection (the 8 MB budget plus one exempt 16 MB block).

## Small things

- `Address::parse("[::ffff:1.2.3.4]:p")` stays IPv6, but replies arrive as IPv4 and fail the client's address check. Normalize mapped addresses in `parse`.
- A version longer than 255 bytes is truncated on the wire but compared in full, so two identical long versions deny each other. Cap it in `Project`.
- Two socket tests are numbered N25 and N26, the same numbers as two conditioner tests (the full names differ). Renumber them.
- Untested: the socket-error classification (`fatal()`), keep-alive and retransmit inside N71's allocation window, the reliable channel across the 16-bit id wrap, and a backward clock jump (it suspends timeouts until the clock catches up).

## Left over from 2c (AnarchyServer and AnarchyPlayer --connect)

- A client still in its first round trip (endpoint state Connecting) when the server closes times out after 5 s ("Could not reach the server") instead of hearing "The server closed": it has no session to accept the Disconnect. Closing it needs a deny-style reply to a ConnectRequest from a closing server.
- Geometry a script builds with a MeshEditor is not a saved property, so it does not replicate: a Server-context script that builds a mesh shows nothing on clients (the Bounce sample). Either replicate editor geometry or document RunContext Any for such scripts.
- No server-wide send budget: 64 clients at 1 MB/s each is 64 MB/s of upload, and 50 backlogged clients cost the server about 1.4 ms a tick on the test socket (RP62). Consider a total budget shared across clients.
- `resolve` blocks; the player calls it once after its window opens, so a slow DNS answer shows a blank window until it returns. Resolve on another thread.
- Exporting on a Mac appends the pack to a signed `AnarchyServer`; Apple Silicon may refuse it until the server is signed again after packing. Untested.
- A 16 MB block still walks every sent-but-unacked fragment from the first open one on each packet; a cursor of the next unsent fragment would make that constant.
- The player hashes the whole resources folder on the UI thread before its first frame, so a big project shows a blank window meanwhile. Compute the hash at export and store it in the pack.
- Temporary unpack folders (`AnarchyEngine/games/`, `AnarchyEngine/servers/`) are never cleaned up, and two servers started together from one exe race on the same folder.
- POSIX SIGHUP is not handled: closing the terminal ends the server without telling its clients.
- Between `close` and `engine.stop` a closing server can still accept a ConnectRequest, and that client may not hear "The server closed".
- `-NS` arguments to the player take the next argument as their value when it has no dash, so `-NSFoo game` (with no value) skips the folder. macOS always passes a value.
