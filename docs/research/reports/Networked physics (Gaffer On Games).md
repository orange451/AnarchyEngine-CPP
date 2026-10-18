# State synchronization, not lockstep: networked physics after Gaffer On Games

Glenn Fiedler's networked-physics series networks one simulation three ways: a player-controlled cube in ODE, then hundreds of cubes, and finally a "katamari" ball of cubes held by constraints ([category page](https://gafferongames.com/categories/networked-physics/)). **Deterministic lockstep** sends only inputs, which is tiny but needs bit-exact determinism and waits on the slowest player. **Snapshot interpolation** sends the whole visible state and needs no determinism, but costs bandwidth and about three send intervals of delay. **State synchronization** sends inputs plus a prioritized subset of each body's state, within whatever bandwidth budget is set, and lets both sides simulate. A final article applies state synchronization to a cooperative VR game, with players taking authority over the bodies they touch. For AnarchyEngine (a dedicated server, many players, many bodies, Box3D not guaranteed bit-exact across platforms), state synchronization with server authority and limited client prediction is the right model, and it is what the networking roadmap adopts (`docs/superpowers/specs/2026-10-12-networking-roadmap-design.md`). This report summarizes what the series teaches and how each lesson maps onto the engine.

## The three models, and why lockstep is out

| | Deterministic lockstep | Snapshot interpolation | State synchronization |
|---|---|---|---|
| What is sent | Inputs only | Every visible body's state | Inputs plus a prioritized subset of body state |
| Receiver simulates? | Yes, identically | No, it only interpolates | Yes, extrapolating between updates |
| Needs determinism? | Bit-exact | No | No |
| Bandwidth | Tiny, independent of body count | Large unless compressed | Fits any budget |
| Weak points | Waits on the slowest peer; cross-platform floating-point determinism is very hard | Delay of about 3× the send interval | Approximate: pops and drift need handling |

Lockstep's promise is striking: in Fiedler's framing a million bodies can cost the same bandwidth as one, because only inputs travel ([Deterministic Lockstep](https://gafferongames.com/post/deterministic_lockstep/)). The cost is determinism, and even on one machine and one binary ODE's solver shuffled constraints with an internal random number generator, so the fix was to reseed it from the frame number every step. Different compilers, operating systems, CPUs, and even debug versus release builds can still diverge. Lockstep also scales poorly beyond two to four players, because everyone advances at the pace of the slowest connection ([Snapshot Interpolation](https://gafferongames.com/post/snapshot_interpolation/)). A dedicated server with fifty players rules it out.

## Transport lessons that hold for every model

- **UDP, never TCP, for real-time data.** Over TCP at 100 ms and 1% loss, lockstep already hitches every few seconds, because each lost packet stalls everything behind it for about two round trips ([Deterministic Lockstep](https://gafferongames.com/post/deterministic_lockstep/)).
- **Redundancy beats retransmission.** Putting every input the other side has not acknowledged into each packet, bit-packed (one bit for an unchanged input, about seven for a changed one), kept lockstep smooth at 2 s of latency and 25% loss in Fiedler's tests.
- **A jitter (playout) buffer turns clumped arrivals into an even stream.** Packets arrive two, zero, one, two, zero per frame; holding them a few frames and releasing them by sequence number removes the hitches. Fiedler used 4–5 frames at 60 Hz for state synchronization and 100 ms in the VR project ([State Synchronization](https://gafferongames.com/post/state_synchronization/); [Networked Physics in VR](https://gafferongames.com/post/networked_physics_in_virtual_reality/)).
- **A sequence number on every packet** drops stale and duplicate packets and, at a fixed tick rate, doubles as the frame number.
- **Cap catch-up steps** (four per frame in the series) so a burst of late packets cannot start a spiral of death.
- **Test over a bad network, not a LAN.** A LAN hides jitter; the series tests at around 250 ms with 5% loss.

AnarchyEngine's transport (sub-project 1, `src/network/`) follows these: piggybacked acks with a 32-bit bitfield, an unreliable channel with per-packet delivery reports, a reliable ordered channel for messages and blocks, and a built-in conditioner so tests run under latency, jitter, loss, and duplication.

## Snapshot interpolation: what it teaches even when it is not the model

- **Render slightly in the past** and interpolate between two buffered snapshots. The buffer should survive two consecutive lost packets: roughly three send intervals plus jitter headroom. That is about 350 ms at 10 Hz, 150 ms at 30 Hz, and 85 ms at 60 Hz ([Snapshot Interpolation](https://gafferongames.com/post/snapshot_interpolation/)).
- **Hermite interpolation for position** (which needs each body's velocity) removes the visible jitter of straight-line interpolation and stops orbiting bodies cutting through the middle. **Slerp for orientation** was enough; angular velocity was not needed.
- **Extrapolation fails for rigid bodies.** It knows nothing of the floor, springs, or collisions, so bodies sink through the ground and snap back, and constrained bodies fly off along their tangents. It is acceptable only for mostly straight-line movers (planes, boats) over 50–250 ms.
- **The fix for delay is a higher send rate**, and that forces compression.

## Compression: from 17 Mbps toward a 256 kbps budget

Fiedler's compression article takes about 900 cubes at 60 Hz, roughly 17 Mbps uncompressed, toward a 256 kbps target ([Snapshot Compression](https://gafferongames.com/post/snapshot_compression/)):

1. **Smallest-three orientation:** two bits naming the largest component, then the other three at 9 bits each over ±0.707. 128 bits become 29.
2. **Bound and quantize everything.** Positions fit ±256 m at about 2 mm in 50 bits instead of 96.
3. **Send nothing you do not need.** At 60 Hz linear interpolation is good enough, so velocity can be dropped entirely.
4. **An at-rest bit** means resting bodies send no velocity.
5. **Delta-encode against the newest state the receiver acknowledged.** This is the largest structural gain. An unchanged body costs one bit, and a stationary scene drops to about 15 kbps, mostly packet headers.
6. **Send only changed bodies,** identified by relative index offsets (about 5.5 bits on average) or by a bitmask when many changed.
7. **Fit delta bit-widths to captured data,** not guesses. An arithmetic coder beat hand-tuned bit-packing by about 25%.

## State synchronization: the model the engine adopts

- **Send position, orientation, linear velocity, and angular velocity.** Without velocities, the receiver extrapolates from the wrong motion and pops when the next update lands ([State Synchronization](https://gafferongames.com/post/state_synchronization/)).
- **Priority accumulator.** Every tick, add each body's priority to its running total, sort by total, and fill the packet until the bandwidth budget is spent. Bodies that were sent reset to zero; the rest carry their totals forward, so nothing starves and the budget can change at runtime for congestion control. In the VR project, raising the priority of hard collisions and recently thrown bodies helped. Lowering the priority of resting bodies hurt: they drifted and caused divergence the next time something hit them ([Networked Physics in VR](https://gafferongames.com/post/networked_physics_in_virtual_reality/)).
- **Snap the simulation to each update; never smooth inside the simulation.** Smoothing belongs to what is drawn.
- **Quantize the whole simulation on both sides every step,** as though it had crossed the network, so both ends extrapolate from identical values. This needs more precision than snapshots do (about 4096 steps per metre and 15-bit orientation components). Quantization pushes bodies into one another, so cap the depenetration velocity (about 1 m/s in PhysX) and soften constraints.
- **Smooth only the rendered result.** Keep a visual error offset per body and decay it each frame: slowly for small errors (about 0.95 per frame up to 25 cm), quickly for large ones (about 0.85 per frame beyond 1 m). A slow drag after a big correction is disorienting.
- **Watch bodies that just came to rest.** If their final update is lost they stay wrong indefinitely; keep their priority high until that update is acknowledged.
- **Plan for bursts of loss.** About a quarter second of nothing will eventually cause divergence and pops even with quantization on both sides.

## Authority, prediction, and rollback

The VR article is the series' answer to latency for bodies a player touches ([Networked Physics in VR](https://gafferongames.com/post/networked_physics_in_virtual_reality/)):

- **Lockstep was ruled out:** PhysX is not deterministic, and hiding 250 ms at 90 Hz would mean about 25 physics steps per rendered frame.
- **Client prediction with rollback was set aside** for that project because, without dedicated servers, it buys little security and needs expensive resimulation.
- **Distributed authority was chosen instead.** Whoever touches a cube becomes its authority, so they see no latency for it, and the host arbitrates in a star topology.
- **Two sequence numbers per cube settle conflicts.** An authority sequence changes on interaction and on coming to rest; an ownership sequence changes on grab. A grab outranks an interaction, and authority can be taken during collisions while ownership cannot. These numbers order late packets from lagging clients.
- **Held objects ride in the avatar's state** as an ID plus a transform relative to the hand, not as physics.
- **Fixed-point ballistic prediction compressed well:** predicted motion matched PhysX within quantization about 90% of the time, so a single "perfect prediction" bit saved a lot. Rotation prediction failed.
- **Result:** under 256 kbps per player and about 1 Mbps at the host for four players. The approach suits cooperative games only, because clients are trusted.

AnarchyEngine needs a dedicated server that resists cheating, so it takes a different path from the VR project: the server keeps authority over every body, and clients predict only what they control. The roadmap's sub-project 5 records the design:

- **Predict little.** The client predicts its own PlayerController plus its "interaction island", the bodies it touches or touched in the last second. Everything else is interpolated about 100 ms in the past and costs nothing to correct.
- **Roll back only the island.** A correction rewinds the island to the server's state at that tick and replays the inputs since, stepping the client's small world with the bodies it does not predict pinned at their recorded poses for each tick.
- **Hide corrections visually.** The error between predicted and corrected pose becomes a decaying visual offset, fast for large errors and slow for small ones, as above.
- **Measure before committing.** At 240 Hz physics, correcting 150 ms of latency means re-stepping about 36 substeps in one frame; a spike will measure what re-stepping a few-hundred-body Box3D world 10–40 times costs, to choose between resimulating the client's interest world and a separate small prediction world.

## What this means for AnarchyEngine

| Lesson | Where it lands |
|---|---|
| UDP, redundancy, piggybacked acks, jitter buffer, conditioner | Sub-project 1, transport (done) |
| Reliable ordered tree changes beside unreliable motion | Sub-project 2, tree replication (in progress) |
| State sync: velocities, priority accumulator, at-rest handling, quantization, delta encoding against acks, Hermite and slerp interpolation | Sub-project 4, physics state sync |
| Server authority with client prediction of the local character and interaction island; rollback of that island only; decaying visual error offsets | Sub-project 5, prediction and rollback |
| Per-connection encryption and connect tokens (netcode.io's approach) | Sub-project 7 |

## Sources

- [Networked Physics category](https://gafferongames.com/categories/networked-physics/)
- [Introduction to Networked Physics](https://gafferongames.com/post/introduction_to_networked_physics/)
- [Deterministic Lockstep](https://gafferongames.com/post/deterministic_lockstep/)
- [Snapshot Interpolation](https://gafferongames.com/post/snapshot_interpolation/)
- [Snapshot Compression](https://gafferongames.com/post/snapshot_compression/)
- [State Synchronization](https://gafferongames.com/post/state_synchronization/)
- [Networked Physics in Virtual Reality](https://gafferongames.com/post/networked_physics_in_virtual_reality/)
