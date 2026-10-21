#include "Replicator.hpp"

#include "ByteBuffer.hpp"
#include "ConnectionInstance.hpp"
#include "LuaApi.hpp"
#include "LuaSource.hpp"
#include "Script.hpp"

#pragma warning(push, 0)
#include <zstd.h>
#pragma warning(pop)

#include <algorithm>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace engine_network {

using engine_core::DataModel;
using engine_core::InstanceId;

namespace {

// Local to each world: what survives that world's own swap.
constexpr const char* kLocalOnly = "Persistent";

// The property a change names, or "" for one never sent (LinearVelocity, Persistent).
std::string property_of(const engine_core::TreeChange& change) {
    using engine_core::Field;
    switch (change.field) {
        case Field::Transform: return "Transform";
        case Field::Source: return "Source";
        case Field::VertexSource: return "VertexSource";
        case Field::Enabled: return "Enabled";
        case Field::Simulated: return "Simulated";
        case Field::VisualOnly: return "VisualOnly";
        case Field::Reflected: {
            std::string name = engine_core::lua_property_name(static_cast<std::uint32_t>(change.payload));
            return name == kLocalOnly ? std::string() : name;
        }
        default: return std::string();
    }
}

}  // namespace

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

// A Destroyed change keeps, in previous_parent, the top of the detached subtree
// the instance was in as it went (the ancestor left nowhere), or kNoParent when
// it was in the tree or was that top itself. end_tick needs it once the instance is gone.
void Replicator::on_change(const engine_core::TreeChange& change) {
    journal_.push_back(change);
    if (change.kind != engine_core::TreeChange::Kind::Destroyed) {
        return;
    }
    InstanceId top = DataModel::kNoParent;
    for (InstanceId at = game_.parent(change.id); at != 0 && at != DataModel::kNoParent; at = game_.parent(at)) {
        top = game_.parent(at) == DataModel::kNoParent ? at : DataModel::kNoParent;
    }
    journal_.back().previous_parent = top;
}

std::string Replicator::client_source(const DataModel& object) {
    const auto* lua = dynamic_cast<const engine_core::SourceText*>(&object);
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
        engine_core::bag_erase(properties, kLocalOnly);
        for (const auto& [key, value] : properties) {
            writer.set(id, key, value);
        }
        return;
    }
    engine_core::PropertyBag properties;
    object.save_properties(properties);
    engine_core::bag_erase(properties, kLocalOnly);
    if (const auto* connection = dynamic_cast<const engine_core::ConnectionInstance*>(&object)) {
        engine_core::bag_set(properties, "Id", engine_core::JsonValue::string(connection->connection_id()));
    }
    const auto* text = dynamic_cast<const engine_core::SourceText*>(&object);
    const bool has_source = text != nullptr;
    const std::string source = has_source ? client_source(object) : std::string();
    std::vector<std::string> extras;
    for (int stage = 1; has_source && stage < text->stage_count(); ++stage) {
        extras.push_back(text->source(stage));
    }
    writer.create(id, parent, object.class_name(), game_.guid(id), game_.name(id), properties,
                  has_source ? &source : nullptr, has_source ? &extras : nullptr);
}

void Replicator::snapshot_messages(InstanceId local_connection, std::vector<std::vector<std::uint8_t>>& out) {
    ByteWriter body;
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
        message.u8(kCodecVersion);
        message.u32(index);
        message.u32(count);
        message.varuint(raw.size());
        const std::size_t from = static_cast<std::size_t>(index) * part;
        const std::size_t size = std::min(part, packed.size() - from);
        message.bytes(packed.data() + from, size);
        out.push_back(std::move(message.data()));
    }
}

int Replicator::depth(InstanceId id) const {
    int steps = 0;
    for (InstanceId at = id; at != 0 && at != DataModel::kNoParent; at = game_.parent(at)) {
        ++steps;
    }
    return steps;
}

void Replicator::write_subtree(OpWriter& writer, InstanceId id, std::unordered_set<InstanceId>& created) {
    std::vector<InstanceId> pending{id};
    while (!pending.empty()) {
        const InstanceId next = pending.back();
        pending.pop_back();
        write_record(writer, next);
        known_.insert(next);
        created.insert(next);
        const std::size_t mark = pending.size();
        for (InstanceId child = game_.first_child(next); child != 0; child = game_.next_sibling(child)) {
            if (replicated(child) && known_.count(child) == 0) {
                pending.push_back(child);
            }
        }
        std::reverse(pending.begin() + static_cast<std::ptrdiff_t>(mark), pending.end());
    }
}

void Replicator::forget_subtree(InstanceId id, const std::unordered_set<InstanceId>& moved) {
    std::vector<InstanceId> pending{id};
    while (!pending.empty()) {
        const InstanceId next = pending.back();
        pending.pop_back();
        known_.erase(next);
        if (!game_.alive(next)) {
            continue;
        }
        for (InstanceId child = game_.first_child(next); child != 0; child = game_.next_sibling(child)) {
            // One that moved here this tick is somewhere else on the client, and has its own entry.
            if (moved.count(child) == 0) {
                pending.push_back(child);
            }
        }
    }
}

bool Replicator::end_tick(std::vector<std::uint8_t>& message) {
    message.clear();
    ++tick_;
    std::vector<engine_core::TreeChange> journal;
    journal.swap(journal_);

    // The tick's journal, coalesced: each instance once per kind, each property once.
    std::vector<InstanceId> destroyed;
    std::unordered_set<InstanceId> destroyed_set;
    std::vector<InstanceId> moved;
    std::unordered_set<InstanceId> moved_set;
    // Where each moved instance was when the tick began: where clients hold it, if they know it.
    std::unordered_map<InstanceId, InstanceId> start_parent;
    // Left nowhere by a parent's destroy this tick and not moved since.
    std::unordered_set<InstanceId> orphaned;
    std::vector<InstanceId> renamed;
    std::unordered_set<InstanceId> renamed_set;
    std::vector<std::pair<InstanceId, std::string>> written;
    std::set<std::pair<InstanceId, std::string>> written_set;
    std::unordered_set<InstanceId> context_changed;
    // Destroyed inside an orphan the client takes with its parent this tick, so it needs no Destroy of its own.
    std::unordered_set<InstanceId> taken_under;
    const auto note_write = [&](InstanceId id, std::string name) {
        if (written_set.emplace(id, name).second) {
            written.emplace_back(id, std::move(name));
        }
    };
    for (const engine_core::TreeChange& change : journal) {
        switch (change.kind) {
            case engine_core::TreeChange::Kind::Destroyed: {
                // The orphan above it was noted when its parent went, earlier in this journal.
                const InstanceId top = change.previous_parent;
                const auto start = start_parent.find(top);
                if (orphaned.count(top) != 0 && start != start_parent.end() && destroyed_set.count(start->second) != 0) {
                    taken_under.insert(change.id);
                }
                if (destroyed_set.insert(change.id).second) {
                    destroyed.push_back(change.id);
                }
                break;
            }
            case engine_core::TreeChange::Kind::Parented:
                if (moved_set.insert(change.id).second) {
                    moved.push_back(change.id);
                    start_parent.emplace(change.id, change.previous_parent);
                }
                if (destroyed_set.count(change.previous_parent) != 0) {
                    orphaned.insert(change.id);
                } else {
                    orphaned.erase(change.id);
                }
                break;
            case engine_core::TreeChange::Kind::Renamed:
                if (renamed_set.insert(change.id).second) {
                    renamed.push_back(change.id);
                }
                break;
            case engine_core::TreeChange::Kind::Property: {
                std::string name = property_of(change);
                if (name.empty()) {
                    break;
                }
                // A new RunContext changes what source a client may hold.
                if (name == "RunContext") {
                    context_changed.insert(change.id);
                    note_write(change.id, "Source");
                }
                note_write(change.id, std::move(name));
                break;
            }
        }
    }
    // An orphan whose parent at the tick's start was destroyed is still under that
    // parent on the client, so the client's destroy_tree of it takes this one: no op.
    // Any other orphan the client knows leaves through a Destroy of its own.
    const auto taken_with_parent = [&](InstanceId id) {
        if (orphaned.count(id) == 0) {
            return false;
        }
        const auto at = start_parent.find(id);
        return at != start_parent.end() && destroyed_set.count(at->second) != 0;
    };

    ByteWriter out;
    out.u8(static_cast<std::uint8_t>(MessageType::Changes));
    out.u8(kCodecVersion);
    out.varuint(tick_);
    OpWriter writer(out, strings_);
    // Strings a later snapshot added that the clients already joined have not heard.
    writer.define_range(broadcast_strings_, static_cast<std::uint32_t>(strings_.size()));
    std::unordered_set<InstanceId> created;

    // 1. Creates and moves, shallowest first, so a parent exists before its children.
    std::vector<std::pair<int, InstanceId>> by_depth;
    for (const InstanceId id : moved) {
        if (game_.alive(id)) {
            by_depth.emplace_back(depth(id), id);
        }
    }
    std::stable_sort(by_depth.begin(), by_depth.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<InstanceId> left_filter;
    for (const auto& entry : by_depth) {
        const InstanceId id = entry.second;
        if (taken_with_parent(id)) {
            forget_subtree(id, moved_set);
            continue;
        }
        if (replicated(id)) {
            if (created.count(id) != 0) {
                continue;
            }
            if (known_.count(id) == 0) {
                write_subtree(writer, id, created);
            } else {
                writer.parent(id, game_.parent(id));
            }
        } else if (known_.count(id) != 0) {
            left_filter.push_back(id);
        }
    }
    // 2. Destroys: what left the filter, then what was destroyed. Moves came
    // first, so an instance moved out of something destroyed here is safe.
    for (const InstanceId id : left_filter) {
        if (known_.count(id) != 0) {
            writer.destroy(id);
            forget_subtree(id, moved_set);
        }
    }
    for (const InstanceId id : destroyed) {
        if (known_.erase(id) != 0 && !taken_with_parent(id) && taken_under.count(id) == 0) {
            writer.destroy(id);
        }
    }
    // 3. Renames, with the current name.
    for (const InstanceId id : renamed) {
        if (known_.count(id) != 0 && created.count(id) == 0 && game_.alive(id)) {
            writer.rename(id, game_.name(id));
        }
    }
    // 4. Property writes, with their current values.
    for (const auto& [id, name] : written) {
        if (known_.count(id) == 0 || created.count(id) != 0 || !game_.alive(id)) {
            continue;
        }
        const DataModel& object = *game_.instance(id);
        if (name == "VertexSource") {
            if (const auto* text = dynamic_cast<const engine_core::SourceText*>(&object)) {
                writer.source(id, text->source(1), 1);
            }
            continue;
        }
        if (name == "Source") {
            if (dynamic_cast<const engine_core::SourceText*>(&object) == nullptr) {
                continue;
            }
            const auto* script = dynamic_cast<const engine_core::Script*>(&object);
            const bool shell = script != nullptr && script->run_context() == engine_core::RunContext::Server;
            // A Server script's source edits stay on the server.
            if (shell && context_changed.count(id) == 0) {
                continue;
            }
            writer.source(id, client_source(object));
            continue;
        }
        // A name it does not answer (view-local or derived) is not sent.
        engine_core::JsonValue value;
        if (object.read_property_json(name, value)) {
            writer.set(id, name, value);
        }
    }

    if (writer.ops() == 0) {
        return false;   // the pending Define ops wait for the next message that carries an op
    }
    broadcast_strings_ = static_cast<std::uint32_t>(strings_.size());
    message = std::move(out.data());
    return true;
}

}  // namespace engine_network
