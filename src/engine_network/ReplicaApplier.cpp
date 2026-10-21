#include "ReplicaApplier.hpp"

#include "ConnectionInstance.hpp"
#include "Connections.hpp"
#include "LuaApi.hpp"
#include "LuaSource.hpp"

#pragma warning(push, 0)
#include <zstd.h>
#pragma warning(pop)

namespace engine_network {

using engine_core::DataModel;
using engine_core::InstanceId;

namespace {
// A snapshot bigger than this is not one any server here would send.
constexpr std::uint64_t kMaxSnapshotBytes = 256ull << 20;
}  // namespace

ReplicaApplier::ReplicaApplier(DataModel& game) : game_(game) { map(0, 0); }

void ReplicaApplier::map(std::uint32_t netid, InstanceId id) {
    // Either side's old pairing goes, so the two maps always mirror each other.
    unmap(netid);
    const auto held = netid_.find(id);
    if (held != netid_.end()) {
        local_.erase(held->second);
        netid_.erase(held);
    }
    local_[netid] = id;
    netid_[id] = netid;
}

void ReplicaApplier::unmap(std::uint32_t netid) {
    const auto found = local_.find(netid);
    if (found == local_.end()) {
        return;
    }
    const auto back = netid_.find(found->second);
    if (back != netid_.end() && back->second == netid) {
        netid_.erase(back);
    }
    local_.erase(found);
}

std::size_t ReplicaApplier::mapped() const { return local_.size(); }

InstanceId ReplicaApplier::local(std::uint32_t netid) const {
    const auto found = local_.find(netid);
    return found != local_.end() ? found->second : DataModel::kNoParent;
}

std::uint32_t ReplicaApplier::netid_of(InstanceId id) const {
    const auto found = netid_.find(id);
    return found != netid_.end() ? found->second : 0;
}

bool ReplicaApplier::joined() const { return joined_; }
std::size_t ReplicaApplier::skipped() const { return skipped_; }
std::uint64_t ReplicaApplier::server_tick() const { return server_tick_; }

ApplyResult ReplicaApplier::receive_message(const std::uint8_t* data, std::size_t size) {
    ByteReader in(data, size);
    switch (static_cast<MessageType>(in.u8())) {
        case MessageType::SnapshotPart: return snapshot_part(in);
        case MessageType::Changes: return changes(in);
        default: return ApplyResult::Bad;
    }
}

ApplyResult ReplicaApplier::snapshot_part(ByteReader& in) {
    // Every part names its codec version, so the first one is refused before anything is buffered.
    const std::uint8_t version = in.u8();
    const std::uint32_t index = in.u32();
    const std::uint32_t count = in.u32();
    const std::uint64_t raw_size = in.varuint();
    if (in.failed() || version != kCodecVersion || joined_ || count == 0 || index != parts_seen_ ||
        raw_size > kMaxSnapshotBytes) {
        return ApplyResult::Bad;
    }
    if (index == 0) {
        parts_count_ = count;
        raw_size_ = raw_size;
    } else if (count != parts_count_ || raw_size != raw_size_) {
        return ApplyResult::Bad;
    }
    // The rest of the message is this part's compressed bytes; never more than a frame of this size could be.
    const std::uint8_t* rest = nullptr;
    const std::size_t left = in.take_rest(rest);
    if (parts_.size() + left > ZSTD_compressBound(static_cast<std::size_t>(raw_size_))) {
        return ApplyResult::Bad;
    }
    parts_.insert(parts_.end(), rest, rest + left);
    ++parts_seen_;
    if (parts_seen_ < parts_count_) {
        return ApplyResult::Ok;
    }
    // The frame says how big it unpacks; believe it only when it agrees with the header, before allocating.
    const unsigned long long framed = ZSTD_getFrameContentSize(parts_.data(), parts_.size());
    if (framed != raw_size_) {
        return ApplyResult::Bad;
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(raw_size_));
    const std::size_t written = ZSTD_decompress(raw.data(), raw.size(), parts_.data(), parts_.size());
    parts_.clear();
    parts_.shrink_to_fit();
    if (ZSTD_isError(written) || written != raw.size()) {
        return ApplyResult::Bad;
    }
    ByteReader body(raw.data(), raw.size());
    server_tick_ = body.varuint();
    const std::uint32_t local_connection = body.u32();
    if (body.failed()) {
        return ApplyResult::Bad;
    }
    std::vector<Op> ops;
    if (!read_ops(body, strings_, ops)) {
        return ApplyResult::Bad;
    }
    if (before_snapshot) {
        before_snapshot();
    }
    for (const Op& op : ops) {
        if (!apply(op)) {
            return ApplyResult::Bad;
        }
    }
    joined_ = true;
    if (local_connection != 0) {
        const InstanceId mine = local(local_connection);
        auto* connections = dynamic_cast<engine_core::Connections*>(game_.instance(game_.connections()));
        const bool real = mine != DataModel::kNoParent && game_.alive(mine) &&
                          dynamic_cast<engine_core::ConnectionInstance*>(game_.instance(mine)) != nullptr &&
                          game_.parent(mine) == game_.connections();
        if (connections != nullptr && real) {
            connections->set_local_connection(mine);
        }
    }
    return ApplyResult::SnapshotApplied;
}

ApplyResult ReplicaApplier::changes(ByteReader& in) {
    const std::uint8_t version = in.u8();
    const std::uint64_t tick = in.varuint();
    if (in.failed() || !joined_ || version != kCodecVersion) {
        return ApplyResult::Bad;
    }
    std::vector<Op> ops;
    if (!read_ops(in, strings_, ops)) {
        return ApplyResult::Bad;
    }
    server_tick_ = tick;
    for (const Op& op : ops) {
        if (!apply(op)) {
            return ApplyResult::Bad;
        }
    }
    return ApplyResult::Ok;
}

bool ReplicaApplier::apply(const Op& op) {
    const auto alive_local = [&](std::uint32_t netid) -> InstanceId {
        const InstanceId id = local(netid);
        return id != DataModel::kNoParent && (id == 0 || game_.alive(id)) ? id : DataModel::kNoParent;
    };
    switch (op.kind) {
        case Op::Kind::Bind: {
            // Only a service, or Connections, is bound: never something a client could make.
            const std::optional<InstanceId> found = engine_core::valid_guid(op.text) ? game_.find_guid(op.text) : std::nullopt;
            const DataModel* object = found ? game_.instance(*found) : nullptr;
            const auto bound = local_.find(op.netid);
            const bool taken = bound != local_.end() && (bound->second == 0 || game_.alive(bound->second));
            if (object == nullptr || !object->is_service() || op.netid == 0 || taken || *found == game_.core() ||
                *found == game_.service("ServerStorage")) {
                return false;
            }
            map(op.netid, *found);
            return true;
        }
        case Op::Kind::Create:
            return apply_create(op);
        case Op::Kind::Destroy: {
            // A refused destroy keeps its mapping; one already gone, or done here, frees the netid.
            const InstanceId id = alive_local(op.netid);
            if (id == DataModel::kNoParent) {
                unmap(op.netid);
                ++skipped_;
                return true;
            }
            if (id == 0 || game_.destroy_error(id)) {
                ++skipped_;
                return true;
            }
            // The server sends no Destroy for what goes with id, so every netid
            // in the subtree is freed here, before destroy_tree forgets the ids.
            std::vector<InstanceId> pending{id};
            while (!pending.empty()) {
                const InstanceId next = pending.back();
                pending.pop_back();
                const auto held = netid_.find(next);
                if (held != netid_.end()) {
                    unmap(held->second);
                }
                for (InstanceId child = game_.first_child(next); child != 0; child = game_.next_sibling(child)) {
                    pending.push_back(child);
                }
            }
            game_.destroy_tree(id);
            return true;
        }
        case Op::Kind::Parent: {
            const InstanceId id = alive_local(op.netid);
            const InstanceId parent = alive_local(op.parent);
            if (id == DataModel::kNoParent || id == 0 || parent == DataModel::kNoParent || game_.parent_error(id, parent)) {
                ++skipped_;
                return true;
            }
            game_.set_parent(id, parent);
            return true;
        }
        case Op::Kind::Rename: {
            const InstanceId id = alive_local(op.netid);
            if (id == DataModel::kNoParent || id == 0 || game_.rename_error(id, op.text)) {
                ++skipped_;
                return true;
            }
            game_.set_name(id, op.text);
            return true;
        }
        case Op::Kind::Set: {
            const InstanceId id = alive_local(op.netid);
            DataModel* object = id == DataModel::kNoParent ? nullptr : (id == 0 ? &game_ : game_.instance(id));
            std::string error;
            if (object == nullptr || op.properties.size() != 1 ||
                !object->load_property(op.properties[0].first, op.properties[0].second, error) || !error.empty()) {
                ++skipped_;
            }
            return true;
        }
        case Op::Kind::Source: {
            const InstanceId id = alive_local(op.netid);
            auto* lua = id == DataModel::kNoParent || id == 0 ? nullptr : dynamic_cast<engine_core::SourceText*>(game_.instance(id));
            if (lua == nullptr) {
                ++skipped_;
                return true;
            }
            lua->set_source(op.stage, op.text);
            return true;
        }
        default:
            return false;
    }
}

bool ReplicaApplier::apply_create(const Op& op) {
    // A netid whose instance died with an ancestor is free again; a live one is a second create.
    const auto old = local_.find(op.netid);
    if (old != local_.end() && old->second != 0 && !game_.alive(old->second)) {
        unmap(op.netid);
    }
    if (local_.count(op.netid) != 0 || op.netid == 0 || !engine_core::valid_guid(op.text)) {
        return false;   // a second create of one instance, or a GUID no server makes: bad data
    }
    const auto parent_it = local_.find(op.parent);
    const InstanceId parent = parent_it != local_.end() ? parent_it->second : DataModel::kNoParent;
    if (parent == DataModel::kNoParent || (parent != 0 && !game_.alive(parent)) ||
        !engine_core::lua_creatable_known(op.class_name.c_str()) || game_.room_left() == 0 || game_.find_guid(op.text)) {
        ++skipped_;
        return true;
    }
    DataModel* made = engine_core::lua_create_instance(game_, op.class_name.c_str());
    if (made == nullptr) {
        ++skipped_;
        return true;
    }
    const InstanceId id = made->id();
    game_.set_guid(id, op.text);
    if (!game_.rename_error(id, op.name)) {
        game_.set_name(id, op.name);
    }
    for (const auto& [key, value] : op.properties) {
        if (key == "Id") {
            if (auto* connection = dynamic_cast<engine_core::ConnectionInstance*>(made); connection != nullptr && value.is_string()) {
                connection->set_connection_id(value.as_string());
                continue;
            }
        }
        std::string error;
        if (!made->load_property(key, value, error) || !error.empty()) {
            ++skipped_;
        }
    }
    if (op.has_source) {
        if (auto* lua = dynamic_cast<engine_core::SourceText*>(made)) {
            lua->set_source(op.source);
            for (std::size_t i = 0; i < op.extra_sources.size(); ++i) {
                lua->set_source(static_cast<int>(i) + 1, op.extra_sources[i]);
            }
        }
    }
    // Parent last, so a Script starts with its Source and RunContext in place.
    if (game_.parent_error(id, parent)) {
        game_.destroy_tree(id);
        ++skipped_;
        return true;
    }
    game_.set_parent(id, parent);
    map(op.netid, id);
    return true;
}

}  // namespace engine_network
