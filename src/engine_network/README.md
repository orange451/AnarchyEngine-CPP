# engine_network

`engine_network` replicates a server's instance tree to its clients over `anarchy_network`. Spec: `docs/superpowers/specs/2026-10-13-server-client-design.md`.

Bottom up: `ByteBuffer` (`ByteWriter` and `ByteReader`: little-endian values, varints, strings, never reading past the end), `JsonWire` (a property value as a save would write it, in binary), `TreeCodec` (the op stream: `StringTable`, `OpWriter`, `read_ops`). Later tasks add `Replicator`, `ReplicaApplier` and the sessions.

Nothing in the engine packages includes this library: `Engine` reaches a session only through `engine_core::INetworkPump`.

Tests: `sandbox/replication_*_tests.cpp`, tag `[replication]`.
