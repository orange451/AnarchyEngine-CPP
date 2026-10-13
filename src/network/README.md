# network

`anarchy_network` moves bytes between a server and its clients over UDP and knows nothing about instances or physics. Spec: `docs/superpowers/specs/2026-10-12-network-transport-design.md`; roadmap: `docs/superpowers/specs/2026-10-12-networking-roadmap-design.md`.

Bottom up: `Bits` (bit packing, quantization), `Sequence` (wrap-aware 16-bit sequence math), `Packet` (the header and the encode/decode hook), `PacketRing` (sent and received rings, acks, RTT, loss), `Socket` (`ISocket`, `UdpSocket`, `LinkHub` for tests), `Conditioner` (latency, jitter, loss, duplication, cap), `ReliableChannel`, `FlowControl`, `Connection` (one peer), `GameIdentity` (game id, version, resources hash), `Endpoint` (server or client).

No threads: every pump call takes `double now` in seconds. No heap use on the packet path; reliable blocks and the conditioner's queue are the exceptions.

Tests: `sandbox/network_*_tests.cpp`, tag `[network]`.
