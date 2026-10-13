# Networking Roadmap

2026-10-12 · the overview that the networking specs hang from. Each sub-project below gets its own spec, plan, and implementation, in order.

## Goal

A place made in Anarchy runs as a multiplayer game on a dedicated server with many players: the server's instance tree replicates to every client automatically, scripts are marked to run on the server or the client and talk through remotes, the server simulates physics, each client predicts its own character and what it is pushing, and everything else interpolates smoothly. A single-player place becomes a multiplayer one by adding players, not by rewriting it.

Two rules decide every trade-off:

1. **It just works.** An author writes a place and marks which scripts run where. Replication, interpolation, prediction, bandwidth, and interest management are the engine's business, and none of them are properties the author must set to get a good result.
2. **Hyper-performant at scale.** Authors will put many physics bodies in a place. A server with 10,000 bodies and 50 players must spend its time on the bodies that move and the players that can see them, and nothing else.

## Where the design comes from

Glenn Fiedler's networked-physics series (gafferongames.com/categories/networked-physics/), with the server-authoritative model its final article argues for. The lessons taken:

- UDP, never TCP. Redundancy instead of retransmission for inputs; a jitter buffer; a sequence number on every packet; acks carried in every packet.
- Physics is state synchronization, not snapshots or lockstep: the server sends each body's position, orientation, and velocities, chosen by a priority accumulator under a per-client bandwidth budget, quantized, and delta-encoded against the last state that client acknowledged. An at-rest flag means a resting body costs nothing.
- The client smooths only what it draws, never the simulation; Hermite interpolation for position, slerp for orientation.
- Prediction is limited to what the player controls and touches, with rollback of only that.

## Performance targets

These hold for every sub-project that touches them and are asserted by benchmarks in the plans:

| Measure | Target |
| --- | --- |
| Players per server | 50 |
| Bodies in a place | 10,000, of which 1,000 awake |
| Server replication cost | under 2 ms per 60 Hz tick on one core, 50 clients |
| Client receive, decode, interpolate | under 0.5 ms per frame |
| Bandwidth per client | under 256 kbps while the world is busy |
| Network conditions it plays under | 150 ms RTT, 20 ms jitter, 2% loss: no visible snapping of the local character |

## Architecture in one paragraph

Two replication paths behind one authored tree. **Tree replication** carries instance creation, parenting, destruction, and ordinary property writes over a reliable channel; these are rare and small. **The physics path** carries body motion in a packed struct-of-arrays table keyed by a dense body index, over an unreliable channel, with interest management (a spatial hash of awake bodies, per-client radius), a priority accumulator with a partial sort to the packet budget, quantization, delta encoding against acknowledged baselines, and client-side interpolation; bodies asleep in Box3D are sent once and then not at all. The client's Box3D world holds only the bodies in its interest; those it does not predict are kinematic at interpolated poses, so the predicted character collides with them, and a rollback re-steps only that small world.

## Sub-projects

| # | Name | Spec | Depends on |
| --- | --- | --- | --- |
| 1 | Transport | `2026-10-12-network-transport-design.md` | — |
| 2 | Server and client | to be written | 1 |
| 3 | Remotes | to be written | 2 |
| 4 | Physics state sync | to be written | 2 |
| 5 | Prediction and rollback | to be written, after a spike | 4 |
| 6 | Studio multi-client testing | to be written | 2, 4 |
| 7 | Authenticated, encrypted connections | to be written | 1 |

Sub-projects 1 to 4 together make a playable networked game whose bodies react to a push one round trip late. 5 removes that for what the player touches.

### 1. Transport

A library under `src/network/` with no dependency on the engine: UDP sockets, packets with sequence numbers and piggybacked acks, an unreliable channel whose delivery the caller learns per packet, a reliable ordered message channel with fragmentation, flow control, a connect handshake that checks the game's identity, a network conditioner for testing, and bit-level serialization shared with the physics path. See its spec.

### 2. Server and client (decided so far)

- A headless server executable runs a project's place with no window or renderer, on the engine loop the single-engine-loop spec defines, and listens on a port. The player connects to it; Studio can be both, for testing (sub-project 6).
- On join the client receives the whole tree (a zstd-compressed reliable block), then a stream of changes: created, reparented, destroyed, property written. Writes physics makes are not on this path.
- Scripts are marked to run on the server or on the client. Names are Anarchy's own, not Roblox's; this spec decides them.
- A `Players` service with a `Player` per connection, its character, and join and leave signals.
- What a client may write: nothing in the replicated tree by default. Client scripts own client-only instances (GUIs, local effects); every change to shared state goes through a remote (3) or the physics path's inputs (4).

### 3. Remotes (decided so far)

- Server-to-client and client-to-server events (fire and forget, reliable, ordered per remote) and functions (a call that returns). Arguments are the engine's property value types plus tables of them; instances cross as references the other side resolves or reads as nil.
- Named for Anarchy, not Roblox.

### 4. Physics state sync (decided so far)

- Per body: position quantized to 1/4096 unit over the world's bounds, orientation as smallest-three with 15-bit components, velocities to 11 bits per component over a bounded range, an at-rest bit. The server quantizes what it sends, not its own simulation.
- Priority: the player's own body and what it touches highest; recently woken or collided bodies boosted; a body that just went to sleep stays high priority until its sleep update is acknowledged, then is not sent until it wakes.
- Interest: a spatial hash of awake bodies; each client sees bodies within its radius; a body entering interest gets a full update; one leaving is left where it was.
- Delta: each body encoded against its state in the newest packet that client acknowledged, with a per-client, per-body baseline over the interest set only.
- Send rate 30 Hz by default while the server ticks at 60; the client holds 3 to 4 samples per body and draws about 100 ms in the past with Hermite position and slerp orientation, through the single-engine-loop spec's quiet setters.
- Inputs client-to-server: redundant, every unacknowledged input in every packet.

### 5. Prediction and rollback (decided so far)

- The client predicts its own PlayerController and its interaction island: the bodies it touches or touched in the last second.
- A correction rewinds the island to the server's state at that tick and replays the inputs since, stepping the client's small world with non-island bodies pinned at their recorded poses per tick. The error between the predicted and corrected pose goes into a visual offset that decays, fast for large errors and slow for small, as Fiedler describes.
- Before the spec: a spike measuring what re-stepping a few-hundred-body Box3D world 10 to 40 times costs, to decide between this and a separate small prediction world.

### 6. Studio multi-client testing (decided so far)

Play can start a server and N clients from the open project, in one process or several, with the conditioner's latency, jitter, and loss set from the UI, and each connection's stats shown.

### 7. Authenticated, encrypted connections (decided so far)

A connect token issued by something the author runs, and encryption of every packet after the handshake, through the encode/decode hook and connect packet the transport leaves for it. Until then the game-identity check in the transport guards against accidents and casual mismatches, not against a deliberate impostor.
