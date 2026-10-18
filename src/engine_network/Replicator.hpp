#pragma once

// The server half of tree replication: it watches the server's world, decides
// what clients may see, and builds the messages that carry it
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).
// A snapshot is one op stream: u8 kCodecVersion, varuint server tick, u32 the
// joining client's Connection netid, then ops (define_all first). Its body is
// zstd-compressed and cut into parts, each: u8 MessageType::SnapshotPart,
// u32 index, u32 count, varuint raw body size, then that part's bytes.

#include "DataModel.hpp"
#include "TreeChange.hpp"
#include "TreeCodec.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace engine_network {

struct ReplicatorOptions {
    std::size_t snapshot_part_bytes = 1u << 20;   // a snapshot message carries at most this much compressed data
};

class Replicator {
public:
    // Sets the game's change sink.
    explicit Replicator(engine_core::DataModel& game, ReplicatorOptions options = {});
    // Clears it.
    ~Replicator();
    Replicator(const Replicator&) = delete;
    Replicator& operator=(const Replicator&) = delete;

    // Clients see it: alive, the root, or under the root and neither Core nor ServerStorage nor under them.
    bool replicated(engine_core::InstanceId id) const;
    // A join snapshot for the client whose Connection is local_connection, as messages ready to send in order.
    void snapshot_messages(engine_core::InstanceId local_connection, std::vector<std::vector<std::uint8_t>>& out);
    // Everything that changed since the last call, as one Changes message for
    // every joined client. False, with message left empty, when nothing did.
    bool end_tick(std::vector<std::uint8_t>& message);
    std::uint64_t tick() const;

private:
    void on_change(const engine_core::TreeChange& change);
    // Source a client may hold: empty for a Server script, otherwise its source.
    static std::string client_source(const engine_core::DataModel& object);
    void write_record(OpWriter& writer, engine_core::InstanceId id) const;
    // Steps from the root to id; a child of the root is 1.
    int depth(engine_core::InstanceId id) const;
    // Creates id and its replicated subtree, skipping children clients already
    // hold (they moved here this tick and get their own Parent op).
    void write_subtree(OpWriter& writer, engine_core::InstanceId id,
                       std::unordered_set<engine_core::InstanceId>& created);
    // Clients no longer hold id or anything under it.
    void forget_subtree(engine_core::InstanceId id);

    engine_core::DataModel& game_;
    ReplicatorOptions options_;
    engine_core::InstanceId core_ = 0;
    engine_core::InstanceId server_storage_ = 0;
    StringTable strings_;
    std::uint32_t broadcast_strings_ = 0;   // strings every joined client has been told
    std::uint64_t tick_ = 0;
    std::unordered_set<engine_core::InstanceId> known_;   // what clients hold, the root aside
    std::vector<engine_core::TreeChange> journal_;        // this tick's changes
};

}  // namespace engine_network
