// The join snapshot: the server's place, filtered, built on an empty client
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "replication_support.hpp"

#include "Connections.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "Light.hpp"
#include "ValueBase.hpp"

#include <catch2/catch_test_macros.hpp>

using engine_core::InstanceId;
using replication::require_same_world;

namespace {

engine_core::Folder& add_folder(engine_core::DataModel& game, InstanceId parent, const char* name) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    game.set_parent(folder.id(), parent);
    return folder;
}

engine_core::ConnectionInstance& add_connection(engine_core::DataModel& game) {
    engine_core::ConnectionInstance& c = game.create<engine_core::ConnectionInstance>();
    c.set_connection_id(engine_core::make_connection_uuid());
    game.set_parent(c.id(), game.connections());
    return c;
}

// A place with something of most kinds: nested folders, a moved part, a light,
// scripts of each RunContext, and things in both Storage services.
void build_place(engine_core::DataModel& game) {
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& level = add_folder(game, workspace, "Level");
    add_folder(game, level.id(), "Inner");
    engine_core::GameObject& part = game.create<engine_core::GameObject>();
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 4.0f;
    part.set_transform(moved);
    game.set_name(part.id(), "Part");
    game.set_parent(part.id(), level.id());
    engine_core::PointLight& lamp = game.create<engine_core::PointLight>();
    game.set_parent(lamp.id(), workspace);
    engine_core::Script& on_server = add_script(game, "ServerLogic", "_G.server_ran = true");
    engine_core::Script& on_client = add_script(game, "ClientLogic", "_G.client_ran = true");
    engine_core::Script& anywhere = add_script(game, "Shared", "_G.shared_ran = true");
    REQUIRE_FALSE(on_server.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
    REQUIRE_FALSE(on_client.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    REQUIRE_FALSE(anywhere.set_run_context(static_cast<int>(engine_core::RunContext::Any)));
    add_folder(game, game.service("ServerStorage"), "Secrets");
    add_folder(game, game.service("ClientStorage"), "Effects");
}

}  // namespace

TEST_CASE("RP20 a snapshot builds the server's place on an empty client", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    build_place(server.game);
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    REQUIRE(applier.joined());
    REQUIRE(applier.skipped() == 0);
    require_same_world(replicator, server.game, client.game);
}

TEST_CASE("RP21 ServerStorage and Core never arrive; ClientStorage does; a Server script is a shell", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    build_place(server.game);
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    REQUIRE_FALSE(replicator.replicated(server.game.service("ServerStorage")));
    REQUIRE_FALSE(replicator.replicated(server.game.core()));
    REQUIRE(replicator.replicated(server.game.service("ClientStorage")));
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    REQUIRE(client.game.find_first_child(client.game.service("ServerStorage"), "Secrets") == 0);
    REQUIRE(client.game.find_first_child(client.game.service("ClientStorage"), "Effects") != 0);
    const InstanceId shell = client.game.find_first_child(client.game.scene_service("Workspace"), "ServerLogic");
    REQUIRE(shell != 0);
    REQUIRE(static_cast<engine_core::Script*>(client.game.instance(shell))->source().empty());
}

TEST_CASE("RP22 a service's changed properties arrive", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    engine_core::DataModel& lighting = *server.game.instance(server.game.scene_service("Lighting"));
    const engine_core::LuaField* exposure = engine_core::lua_class_find("Lighting", "Exposure");
    REQUIRE(exposure != nullptr);
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Number;
    slot.number = 3.25;
    REQUIRE(exposure->write(server.game, lighting, slot));
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    engine_core::JsonValue value;
    REQUIRE(client.game.instance(client.game.scene_service("Lighting"))->read_property_json("Exposure", value));
    REQUIRE(value.as_number() == 3.25);
}

TEST_CASE("RP23 Connections arrive with their Ids, and LocalConnection is the client's own", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    engine_core::ConnectionInstance& first = add_connection(server.game);
    engine_core::ConnectionInstance& mine = add_connection(server.game);
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(mine.id(), messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    require_same_world(replicator, server.game, client.game);
    auto* connections = static_cast<engine_core::Connections*>(client.game.instance(client.game.connections()));
    const InstanceId local = connections->local_connection();
    REQUIRE(local != 0);
    REQUIRE(static_cast<engine_core::ConnectionInstance*>(client.game.instance(local))->connection_id() ==
            mine.connection_id());
    REQUIRE(applier.local(first.id()) != engine_core::DataModel::kNoParent);
    REQUIRE(static_cast<engine_core::Connections*>(server.game.instance(server.game.connections()))->local_connection() == 0);
}

TEST_CASE("RP24 a reference to a later instance resolves; one into ServerStorage reads nil", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    const InstanceId workspace = server.game.scene_service("Workspace");
    engine_core::ObjectValue& early = server.game.create<engine_core::ObjectValue>();
    server.game.set_name(early.id(), "Early");
    server.game.set_parent(early.id(), workspace);
    engine_core::ObjectValue& hidden = server.game.create<engine_core::ObjectValue>();
    server.game.set_name(hidden.id(), "Hidden");
    server.game.set_parent(hidden.id(), workspace);
    engine_core::Folder& later = add_folder(server.game, workspace, "Later");
    engine_core::Folder& secret = add_folder(server.game, server.game.service("ServerStorage"), "Secret");
    engine_core::LuaSlot to_later;
    to_later.kind = engine_core::LuaSlot::Kind::Instance;
    to_later.id = later.id();
    REQUIRE_FALSE(early.set_value(to_later));
    engine_core::LuaSlot to_secret = to_later;
    to_secret.id = secret.id();
    REQUIRE_FALSE(hidden.set_value(to_secret));
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    const InstanceId cw = client.game.scene_service("Workspace");
    engine_core::LuaSlot slot;
    const engine_core::LuaField* value = engine_core::lua_class_find("ObjectValue", "Value");
    REQUIRE(value->read(client.game, *client.game.instance(client.game.find_first_child(cw, "Early")), slot));
    REQUIRE(slot.kind == engine_core::LuaSlot::Kind::Instance);
    REQUIRE(slot.id == client.game.find_first_child(cw, "Later"));
    REQUIRE(value->read(client.game, *client.game.instance(client.game.find_first_child(cw, "Hidden")), slot));
    REQUIRE(slot.id == 0);
}

TEST_CASE("RP25 client scripts in the snapshot run on the client; a Server shell does not", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    build_place(server.game);
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    client.frames(2);
    bool ran = false;
    REQUIRE(client.runtime.global_boolean("client_ran", ran));
    REQUIRE(ran);
    REQUIRE(client.runtime.global_boolean("shared_ran", ran));
    REQUIRE(ran);
    // The Server script arrived as a shell: no Source, and nothing it would have set.
    bool server_ran = false;
    (void)client.runtime.global_boolean("server_ran", server_ran);
    REQUIRE_FALSE(server_ran);
    const InstanceId shell = client.game.find_first_child(client.game.scene_service("Workspace"), "ServerLogic");
    REQUIRE(shell != 0);
    REQUIRE(static_cast<engine_core::Script*>(client.game.instance(shell))->source().empty());
}

TEST_CASE("RP26 a large snapshot splits into parts; a missing or corrupt part is bad data", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    const InstanceId workspace = server.game.scene_service("Workspace");
    for (int i = 0; i < 2000; ++i) {
        add_folder(server.game, workspace, ("F" + std::to_string(i)).c_str());
    }
    replication::start_server(server);
    replication::start_client(client);
    engine_network::ReplicatorOptions options;
    options.snapshot_part_bytes = 4096;
    engine_network::Replicator replicator(server.game, options);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    REQUIRE(messages.size() > 1);
    {
        engine_network::ReplicaApplier applier(client.game);
        std::vector<std::vector<std::uint8_t>> skipped = messages;
        skipped.erase(skipped.begin() + 1);
        REQUIRE(replication::deliver(applier, skipped) == engine_network::ApplyResult::Bad);
    }
    {
        ScriptRig other;
        replication::start_client(other);
        engine_network::ReplicaApplier applier(other.game);
        std::vector<std::vector<std::uint8_t>> corrupt = messages;
        corrupt.back().back() ^= 0xFF;
        REQUIRE(replication::deliver(applier, corrupt) == engine_network::ApplyResult::Bad);
    }
    {
        // A first part of another codec version is refused before any of it is buffered.
        ScriptRig other;
        replication::start_client(other);
        engine_network::ReplicaApplier applier(other.game);
        std::vector<std::uint8_t> first = messages.front();
        REQUIRE(first[1] == engine_network::kCodecVersion);
        first[1] = 1;
        REQUIRE(applier.receive_message(first.data(), first.size()) == engine_network::ApplyResult::Bad);
    }
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    require_same_world(replicator, server.game, client.game);
    // A tick's changes before any snapshot are out of order.
    server.game.set_name(server.game.find_first_child(workspace, "F0"), "Renamed");
    std::vector<std::uint8_t> changes;
    REQUIRE(replicator.end_tick(changes));
    ScriptRig fresh;
    replication::start_client(fresh);
    engine_network::ReplicaApplier unjoined(fresh.game);
    REQUIRE(unjoined.receive_message(changes.data(), changes.size()) == engine_network::ApplyResult::Bad);
}

TEST_CASE("RP27 a subtree destroyed and made again under the same netids is accepted", "[replication][snapshot]") {
    ScriptRig server;
    ScriptRig client;
    const InstanceId workspace = server.game.scene_service("Workspace");
    engine_core::Folder& box = add_folder(server.game, workspace, "Box");
    engine_core::Folder& child = add_folder(server.game, box.id(), "Child");
    replication::start_server(server);
    replication::start_client(client);
    engine_network::Replicator replicator(server.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    engine_network::ReplicaApplier applier(client.game);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    const auto tick = [&](std::vector<std::uint8_t>& message) {
        REQUIRE(replicator.end_tick(message));
        return applier.receive_message(message.data(), message.size());
    };

    // Hidden in ServerStorage, the subtree is destroyed on the client.
    std::vector<std::uint8_t> message;
    server.game.set_parent(box.id(), server.game.service("ServerStorage"));
    REQUIRE(tick(message) == engine_network::ApplyResult::Ok);
    REQUIRE(client.game.find_first_child(client.game.scene_service("Workspace"), "Box") == 0);
    // Moved back, it is made again under the same netids.
    server.game.set_parent(box.id(), workspace);
    REQUIRE(tick(message) == engine_network::ApplyResult::Ok);
    require_same_world(replicator, server.game, client.game);
    REQUIRE(applier.local(child.id()) != engine_core::DataModel::kNoParent);
    REQUIRE(applier.skipped() == 0);
    // The same creates again name live netids: bad data.
    REQUIRE(applier.receive_message(message.data(), message.size()) == engine_network::ApplyResult::Bad);
    // A Bind of netid 0 is bad data too (a Bind names no table string).
    engine_network::StringTable table;
    engine_network::ByteWriter body;
    body.u8(static_cast<std::uint8_t>(engine_network::MessageType::Changes));
    body.u8(engine_network::kCodecVersion);
    body.varuint(replicator.tick() + 1);
    engine_network::OpWriter writer(body, table);
    writer.bind(0, server.game.guid(workspace));
    REQUIRE(applier.receive_message(body.data().data(), body.data().size()) == engine_network::ApplyResult::Bad);
}
