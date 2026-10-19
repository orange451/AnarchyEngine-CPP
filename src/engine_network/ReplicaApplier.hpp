#pragma once

// The client half of tree replication: it reassembles a join snapshot and
// applies what the server sends through the ordinary setters, so a client's
// tree is built the way any script or tool would build it.

#include "DataModel.hpp"
#include "TreeCodec.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace engine_network {

enum class ApplyResult { Ok, SnapshotApplied, Bad };

class ReplicaApplier {
public:
    explicit ReplicaApplier(engine_core::DataModel& game);

    // One message from the server: a snapshot part (applied when the last one
    // arrives) or a tick's changes (only after the snapshot). Bad: the bytes
    // were malformed or out of order; the caller ends the connection.
    ApplyResult receive_message(const std::uint8_t* data, std::size_t size);
    engine_core::InstanceId local(std::uint32_t netid) const;   // DataModel::kNoParent when not known
    bool joined() const;               // the snapshot has been applied
    std::size_t skipped() const;       // operations refused or naming an instance this client no longer has
    std::uint64_t server_tick() const;
    // How many netids this client maps: what it holds of the server's tree, the root included.
    std::size_t mapped() const;
    // Runs once a whole snapshot has arrived and unpacked, just before its ops apply:
    // a join that swaps the world swaps it here, once the download itself can no longer
    // fail. A malformed op can still fail the apply after it, leaving a half-built world.
    std::function<void()> before_snapshot;

private:
    ApplyResult snapshot_part(ByteReader& in);
    ApplyResult changes(ByteReader& in);
    // False only for bad data; a refused operation counts as skipped.
    bool apply(const Op& op);
    bool apply_create(const Op& op);
    // Pairs netid and id in both maps, dropping either one's old pairing.
    void map(std::uint32_t netid, engine_core::InstanceId id);
    void unmap(std::uint32_t netid);

    engine_core::DataModel& game_;
    StringTable strings_;
    std::unordered_map<std::uint32_t, engine_core::InstanceId> local_;   // netid -> this world's id
    std::unordered_map<engine_core::InstanceId, std::uint32_t> netid_;   // the reverse: this world's id -> netid
    std::vector<std::uint8_t> parts_;   // compressed snapshot bytes so far
    std::uint32_t parts_seen_ = 0;
    std::uint32_t parts_count_ = 0;
    std::uint64_t raw_size_ = 0;
    bool joined_ = false;
    std::size_t skipped_ = 0;
    std::uint64_t server_tick_ = 0;
};

}  // namespace engine_network
