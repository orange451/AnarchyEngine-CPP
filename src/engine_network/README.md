# engine_network

`engine_network` replicates a server's instance tree to its clients over `anarchy_network`. Spec: `docs/superpowers/specs/2026-10-13-server-client-design.md`.

Bottom up: `ByteBuffer` (`ByteWriter` and `ByteReader`: little-endian values, varints, strings, never reading past the end), `JsonWire` (a property value as a save would write it, in binary), `TreeCodec` (the op stream: `StringTable`, `OpWriter`, `read_ops`). `Replicator` (what clients may see, and the join snapshot: the filtered tree as one op stream, zstd-compressed, cut into parts) and `ReplicaApplier` (reassembles a snapshot and applies ops through the ordinary setters, counting refused ones as skipped). `Replicator::end_tick` turns each tick's journal into one Changes message. `Sessions` (`ServerSession`, `ClientSession`) wire an `anarchy_network` Endpoint to the tree: the server gives each client a Connection, its snapshot at the end of the tick it connects, and every later tick's changes behind it, dropping a client whose queue grows too long; the client starts play empty, builds the world from the snapshot, and says why a connection ended in words for a player.

Nothing in the engine packages includes this library: `Engine` reaches a session only through `engine_core::INetworkPump`.

Tests: `sandbox/replication_*_tests.cpp`, tag `[replication]`.
