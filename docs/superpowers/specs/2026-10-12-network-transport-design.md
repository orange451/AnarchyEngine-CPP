# Network Transport

2026-10-12 · sub-project 1 of the networking roadmap (`2026-10-12-networking-roadmap-design.md`). A library that moves bytes between a server and its clients over UDP and knows nothing about instances or physics.

## Goal

Every later networking sub-project sends through this and never touches a socket. It gives them: packets that each know whether they arrived, a reliable ordered channel for the tree and remotes, an unreliable channel for state and inputs, a bandwidth budget per connection, a handshake that refuses another game's client, and a network conditioner so every test runs under latency and loss.

## Why our own, not a library

The physics path encodes each body against the last state a client *acknowledged*, keeps a body that just fell asleep at high priority *until its sleep update is acknowledged*, and sends every *unacknowledged* input in every packet. All of that needs per-packet delivery knowledge for unreliable packets. ENet hides it. yojimbo (Fiedler's netcode.io plus reliable.io) exposes it but brings libsodium and a fiddly MSVC build for encryption we are deferring. Fiedler's own articles (*Reliability and Flow Control*, *Sending Large Blocks of Data*, *Client Server Connection*) describe the design below in full; it is about 1,500 lines and no new dependency. The hash uses XXH64, which zstd already bundles.

## Decisions

| Question | Decision |
| --- | --- |
| Where | `src/network/`, built as `anarchy_network`, depends only on the standard library, the platform socket API, and zstd's XXH64. `engine_core` does not depend on it; sub-project 2 adds the dependency the other way. |
| Transport | UDP only, IPv4 and IPv6. Datagrams of at most 1,200 bytes, so nothing fragments at the IP layer. |
| Threads | None of its own. The engine thread calls `receive()` at the start of its tick and `send()` at the end, under the engine's write lock, so callbacks may touch the tree. |
| Allocation | Fixed rings sized at connect time. The heap is used only to reassemble reliable blocks. |
| Acks | Piggybacked: every packet carries the peer's highest received sequence and a 32-bit bitfield of the 32 before it. No ack packets of their own; a keep-alive 10 times a second when there is nothing else to say. |
| Channels | `Unreliable`: one payload per packet, delivery reported per sequence. `Reliable`: ordered messages of any size, exactly once, sent until acked, fragmented over about 1,000 bytes. |
| Flow control | A bytes-per-second budget per connection: halved when loss over 2 s exceeds 10% or RTT exceeds 250 ms, recovering 10% a second. The transport reports how many payload bytes this tick's packet may carry; callers decide what. |
| Handshake | Client salt, server salt, connection id = their XOR carried in every later packet. Game identity checked before acceptance: `GameId`, `version`, `ResourcesHash`. |
| Game identity | `project.json` gains `gameId` (random 128-bit, made at project creation, added on load to older projects, kept by Save As) and `version` (`"1.0.0"` by default; the user will set it later). The resources hash is XXH64 over the resources folder's files in sorted relative-path order. |
| Security | None yet. One `encode`/`decode` point per packet, the identity today, and the connect packet are where sub-project 7 adds a token and encryption. The identity check guards against accidents and casual mismatches, not a deliberate impostor. |
| Conditioner | Between Endpoint and Socket, always present, free when every setting is 0: latency, jitter, loss, duplication, bandwidth cap. An in-memory `Link` replaces the socket in tests. |
| Errors | Bad packets are dropped and counted, never thrown. `Endpoint::open` returns a result; a socket failing mid-session disconnects every peer with reason `Socket`. |
| Not here | Instances, properties, physics, remotes, Studio UI, NAT traversal, relays, a lobby. |

## Architecture

### 1. The wire

```
header  (13 bytes)
  protocol id     u16   this engine's wire version; mismatched packets are dropped silently
  type            u8    ConnectRequest, ConnectAccept, ConnectDeny, Disconnect, KeepAlive, Payload
  connection id   u16   low 16 bits of client salt XOR server salt; 0 during the handshake
  sequence        u16   this packet's number, wrapping; compared wrap-aware
  ack             u16   highest sequence received from the peer
  ack bits        u32   bit n set: sequence (ack − n − 1) was also received
payload
  reliable section    count u8, then messages (id u16, length u16, bytes) or fragments
  unreliable section  the rest of the packet
```

Every packet is one datagram. The sender keeps a ring of its last 256 sent packets: send time, size, the reliable message ids inside, and an opaque 32-bit user tag. When the peer's ack state covers a sequence, the Unreliable channel calls `on_delivered(sequence, tag)` once. A packet not acked within RTT × 2 (at least 100 ms) counts as lost for the loss estimate; an ack arriving later still fires `on_delivered`, since a baseline that arrived late is still a baseline the peer holds.

Received sequences are tracked in a 256-entry ring; a sequence already seen, or older than the ring, is dropped and counted as a replay.

### 2. Reliable channel

Messages are numbered 16-bit and delivered in order, exactly once. Each packet includes as many unacked messages as fit, oldest first; a message is resent no sooner than RTT × 1.25 after its last send. A message over 1,000 bytes is a block: fragments of 1,000 bytes, each `(message id, fragment index u16, fragment count u16, bytes)`, one per packet, acked individually through the packet ring and reassembled on the other side; the block is delivered when complete. Callers compress a block with zstd when it is worth it; the transport does not. The receive window holds 256 messages; `send` refuses a message when the send window would overflow and returns `false`, which the caller sees as back-pressure.

### 3. Unreliable channel

`begin_payload()` gives a `BitWriter` over the space left in this tick's packet after the reliable section; `end_payload(tag)` returns the sequence the packet will carry. On receipt, `on_payload(sequence, BitReader)` is called once per packet in arrival order; the channel never reorders or holds packets, since a jitter buffer belongs to the caller who knows the tick rate.

### 4. Flow control and stats

Per connection: smoothed RTT (10% weight per sample, from the ack of a packet to its send time), loss over the last 2 s from the packet ring, bytes in and out per second, and the budget in bytes per second (`Good` mode: the configured rate; `Bad` mode: half; with the recovery schedule above). `payload_budget()` returns how many bytes this tick may carry at the send interval the caller runs. `Stats` holds these and the drop counters (bad protocol, bad connection id, truncated, replay, unknown peer) for Studio to show in sub-project 6.

### 5. Connections and the handshake

```
client → ConnectRequest   client salt u64, GameId 16 bytes, version string, ResourcesHash u64
server → ConnectAccept    server salt u64            (or)
server → ConnectDeny      reason u8: Full, WrongProtocol, WrongGame, WrongVersion, WrongContent;
                          for WrongVersion, the server's version string
```

The client sends `ConnectRequest` 10 times a second until an answer or 5 s. The server checks protocol id, then GameId, then version, then ResourcesHash, in that order, and denies with the first mismatch; it accepts up to its configured client count. From `ConnectAccept` on, every packet carries the connection id, so a stale or spoofed packet from another session is dropped. `Disconnect` is sent 10 times and the peer forgotten; a peer silent for 5 s times out. A connection is `Connecting`, `Connected`, or `Disconnected` with a reason (`Requested`, `Timeout`, `Denied(reason)`, `Socket`).

`GameIdentity` is a value `{ game_id, version, resources_hash }` the Endpoint is given at open. The engine side computes it: `Project` reads `gameId` and `version` from `project.json` (writing a new `gameId` and `"1.0.0"` into a project that lacks them on the next save), and `hash_resources(path)` walks the resources folder, sorts relative paths with `/` separators, and feeds each path then its bytes into XXH64. A packaged game carries the identity in its package; the server computes it at startup, once.

### 6. Pumping

`Endpoint::receive()` drains the socket, routes each datagram to its connection by address and connection id (or to the handshake), updates acks, RTT, and loss, delivers reliable messages in order, and calls `on_payload`. `Endpoint::send()` builds one packet per connected peer when there are reliable messages due, a payload was written, or a keep-alive is due, and hands it to the conditioner. Both are called once per engine tick by sub-project 2.

### 7. The conditioner and the link

`Conditioner` wraps a `Socket` and holds a time-ordered ring of delayed datagrams; `send` queues each datagram at `now + latency ± jitter`, drops it with probability `loss`, enqueues it twice with probability `duplicate`, and refuses it when the bandwidth cap is spent this second; `receive` releases what is due. `Link` is a `Socket` that delivers to another `Link` in the same process, so two Endpoints run a whole session in a unit test with no port.

### 8. Bit serialization

`BitWriter` and `BitReader` over a byte buffer: `bits(value, n)`, `bool`, `bytes`, `align`; `integer(value, min, max)` in the fewest bits; `float_bounded(value, min, max, resolution)` and `quaternion_smallest_three(q, bits_per_component)` as Fiedler describes, with the inverse on read. Reads past the end set a `failed` flag the caller checks once rather than throwing. The physics path reuses these in sub-project 4.

## Testing

Catch2 tests in `sandbox/network_tests.cpp`, all through `Link` and the conditioner unless named otherwise:

- `BitWriter`/`BitReader` round-trips for every helper, including the quantization edges and reads past the end.
- Sequence comparison and the ack bitfield across the 65535 wrap.
- Reliable delivery in order, exactly once, under 30% loss and reordering; blocks of 100 KB reassembled under loss; send-window back-pressure.
- Unreliable `on_delivered` fires once per acked sequence and never for a lost one; a late ack still fires it.
- Handshake: accept, deny when full, each identity mismatch with its reason, timeout, stale connection id rejected, disconnect both ways.
- Flow control halves under 10% loss and recovers at 10% a second.
- `hash_resources` is the same for the same files in a different directory order and differs when one byte changes.
- A soak test moving 10,000 reliable messages through a 200 ms / 5% / 30 ms-jitter link.
- One real-socket test over loopback, skipped when no socket can be opened.
- A benchmark: a server pumping 64 connections at 60 Hz with full 1,200-byte packets spends under 1 ms per tick, measured warmed up in Release.
