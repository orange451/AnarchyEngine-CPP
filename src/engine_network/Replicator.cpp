#include "Replicator.hpp"

#include "ByteBuffer.hpp"
#include "ConnectionInstance.hpp"
#include "LuaSource.hpp"
#include "Script.hpp"

#pragma warning(push, 0)
#include <zstd.h>
#pragma warning(pop)

#include <algorithm>
#include <stdexcept>

namespace engine_network {

using engine_core::DataModel;
using engine_core::InstanceId;

Replicator::Replicator(DataModel& game, ReplicatorOptions options)
    : game_(game), options_(options), core_(game.core()), server_storage_(game.service("ServerStorage")) {
    // Every replicated instance is one a client will hold once its snapshot lands.
    std::vector<InstanceId> pending{0};
    while (!pending.empty()) {
        const InstanceId id = pending.back();
        pending.pop_back();
        for (InstanceId child = game_.first_child(id); child != 0; child = game_.next_sibling(child)) {
            if (replicated(child)) {
                known_.insert(child);
                pending.push_back(child);
            }
        }
    }
    game_.set_change_sink([this](const engine_core::TreeChange& change) { on_change(change); });
}

Replicator::~Replicator() { game_.set_change_sink({}); }

bool Replicator::replicated(InstanceId id) const {
    if (id == 0) {
        return true;
    }
    if (!game_.alive(id)) {
        return false;
    }
    for (InstanceId at = id; at != 0; at = game_.parent(at)) {
        if (at == DataModel::kNoParent || at == core_ || at == server_storage_) {
            return false;
        }
    }
    return true;
}

std::uint64_t Replicator::tick() const { return tick_; }

void Replicator::on_change(const engine_core::TreeChange& change) { journal_.push_back(change); }

std::string Replicator::client_source(const DataModel& object) {
    const auto* lua = dynamic_cast<const engine_core::LuaSource*>(&object);
    if (lua == nullptr) {
        return std::string();
    }
    const auto* script = dynamic_cast<const engine_core::Script*>(&object);
    if (script != nullptr && script->run_context() == engine_core::RunContext::Server) {
        return std::string();
    }
    return lua->source();
}

void Replicator::write_record(OpWriter& writer, InstanceId id) const {
    const DataModel& object = *game_.instance(id);
    const InstanceId parent = game_.parent(id);
    if (object.is_service()) {
        // Every Game has its services: bind them by GUID, then what differs from their defaults.
        writer.bind(id, game_.guid(id));
        engine_core::PropertyBag properties;
        object.save_properties(properties);
        for (const auto& [key, value] : properties) {
            writer.set(id, key, value);
        }
        return;
    }
    engine_core::PropertyBag properties;
    object.save_properties(properties);
    if (const auto* connection = dynamic_cast<const engine_core::ConnectionInstance*>(&object)) {
        engine_core::bag_set(properties, "Id", engine_core::JsonValue::string(connection->connection_id()));
    }
    const bool has_source = dynamic_cast<const engine_core::LuaSource*>(&object) != nullptr;
    const std::string source = has_source ? client_source(object) : std::string();
    writer.create(id, parent, object.class_name(), game_.guid(id), game_.name(id), properties,
                  has_source ? &source : nullptr);
}

void Replicator::snapshot_messages(InstanceId local_connection, std::vector<std::vector<std::uint8_t>>& out) {
    ByteWriter body;
    body.u8(kCodecVersion);
    body.varuint(tick_);
    body.u32(local_connection);
    OpWriter writer(body, strings_);
    writer.define_all();
    // Parents before children, siblings in order.
    std::vector<InstanceId> pending;
    for (InstanceId child = game_.first_child(0); child != 0; child = game_.next_sibling(child)) {
        pending.push_back(child);
    }
    std::reverse(pending.begin(), pending.end());
    while (!pending.empty()) {
        const InstanceId id = pending.back();
        pending.pop_back();
        if (!replicated(id)) {
            continue;
        }
        write_record(writer, id);
        const std::size_t mark = pending.size();
        for (InstanceId child = game_.first_child(id); child != 0; child = game_.next_sibling(child)) {
            pending.push_back(child);
        }
        std::reverse(pending.begin() + static_cast<std::ptrdiff_t>(mark), pending.end());
    }
    // Compress the body whole, then cut it into parts.
    const std::vector<std::uint8_t>& raw = body.data();
    std::vector<std::uint8_t> packed(ZSTD_compressBound(raw.size()));
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    if (cctx == nullptr) {
        throw std::runtime_error("zstd: failed to create a compression context");
    }
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1);
    const std::size_t written = ZSTD_compress2(cctx, packed.data(), packed.size(), raw.data(), raw.size());
    ZSTD_freeCCtx(cctx);
    if (ZSTD_isError(written)) {
        throw std::runtime_error("zstd: compressing a snapshot failed");
    }
    packed.resize(written);
    const std::size_t part = std::max<std::size_t>(1, options_.snapshot_part_bytes);
    const std::uint32_t count = static_cast<std::uint32_t>((packed.size() + part - 1) / part);
    for (std::uint32_t index = 0; index < count; ++index) {
        ByteWriter message;
        message.u8(static_cast<std::uint8_t>(MessageType::SnapshotPart));
        message.u32(index);
        message.u32(count);
        message.varuint(raw.size());
        const std::size_t from = static_cast<std::size_t>(index) * part;
        const std::size_t size = std::min(part, packed.size() - from);
        message.bytes(packed.data() + from, size);
        out.push_back(std::move(message.data()));
    }
}

}  // namespace engine_network
