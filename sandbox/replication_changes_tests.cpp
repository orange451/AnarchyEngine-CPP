// Each tick's changes reach a joined client and leave it with the server's
// tree (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "replication_support.hpp"

#include "Folder.hpp"
#include "GameObject.hpp"
#include "Light.hpp"
#include "ValueBase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using engine_core::InstanceId;
using replication::require_same_world;

namespace {

// A server and one joined client, stepped by hand.
struct Pair {
    ScriptRig server;
    ScriptRig client;
    std::unique_ptr<engine_network::Replicator> replicator;
    std::unique_ptr<engine_network::ReplicaApplier> applier;

    Pair() {
        replication::start_server(server);
        replication::start_client(client);
        replicator = std::make_unique<engine_network::Replicator>(server.game);
        applier = std::make_unique<engine_network::ReplicaApplier>(client.game);
        std::vector<std::vector<std::uint8_t>> messages;
        replicator->snapshot_messages(0, messages);
        REQUIRE(replication::deliver(*applier, messages) == engine_network::ApplyResult::SnapshotApplied);
    }
    // Ends the server's tick and applies what it sends. False when it sent nothing.
    bool tick() {
        std::vector<std::uint8_t> message;
        if (!replicator->end_tick(message)) {
            REQUIRE(message.empty());
            return false;
        }
        REQUIRE(applier->receive_message(message.data(), message.size()) == engine_network::ApplyResult::Ok);
        return true;
    }
    InstanceId client_id(InstanceId server_id) const { return applier->local(server_id); }
    void same() { require_same_world(*replicator, server.game, client.game); }
};

engine_core::Folder& add_folder(engine_core::DataModel& game, InstanceId parent, const char* name) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    game.set_parent(folder.id(), parent);
    return folder;
}

// Every live instance in a world sits somewhere: nothing leaked unparented.
void require_no_orphans(engine_core::DataModel& game) {
    game.for_each_instance([&](engine_core::DataModel& object) {
        INFO(object.class_name());
        REQUIRE(game.parent(object.id()) != engine_core::DataModel::kNoParent);
    });
}

}  // namespace

TEST_CASE("RP30 creates, property writes, renames and source edits reach the client", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& folder = add_folder(game, workspace, "Box");
    engine_core::PointLight& lamp = game.create<engine_core::PointLight>();
    game.set_parent(lamp.id(), folder.id());
    REQUIRE(pair.tick());
    pair.same();
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[13] = 7.0f;
    lamp.set_transform(moved);
    game.set_name(folder.id(), "Crate");
    engine_core::Script& script = add_script(game, "Mover", "-- one");
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    REQUIRE(pair.tick());
    script.set_source("-- two");
    REQUIRE(pair.tick());
    pair.same();
    REQUIRE(static_cast<engine_core::PointLight*>(pair.client.game.instance(pair.client_id(lamp.id())))->transform().m[13] == 7.0f);
    REQUIRE(pair.applier->skipped() == 0);
}

TEST_CASE("RP31 the last write wins, a create and destroy in one tick sends nothing, a quiet tick sends nothing", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    const InstanceId workspace = game.scene_service("Workspace");
    REQUIRE_FALSE(pair.tick());
    engine_core::Folder& gone = add_folder(game, workspace, "Gone");
    game.destroy(gone.id());
    REQUIRE_FALSE(pair.tick());
    engine_core::Folder& folder = add_folder(game, workspace, "A");
    REQUIRE(pair.tick());
    game.set_name(folder.id(), "B");
    game.set_name(folder.id(), "C");
    std::vector<std::uint8_t> message;
    REQUIRE(pair.replicator->end_tick(message));
    engine_network::StringTable strings;
    engine_network::ByteReader in(message.data() + 1, message.size() - 1);   // past the message type
    in.u8();
    in.varuint();
    std::vector<engine_network::Op> ops;
    REQUIRE(engine_network::read_ops(in, strings, ops));
    REQUIRE(ops.size() == 1);
    REQUIRE(ops[0].kind == engine_network::Op::Kind::Rename);
    REQUIRE(ops[0].text == "C");
}

TEST_CASE("RP32 destroy_tree removes a subtree; a plain destroy's orphans leave too, leaking nothing", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& tree = add_folder(game, workspace, "Tree");
    add_folder(game, add_folder(game, tree.id(), "Branch").id(), "Leaf");
    engine_core::Folder& plain = add_folder(game, workspace, "Plain");
    add_folder(game, add_folder(game, plain.id(), "Kid").id(), "Grandkid");
    REQUIRE(pair.tick());
    game.destroy_tree(tree.id());
    game.destroy(plain.id());   // Kid (with Grandkid) is left nowhere on the server
    REQUIRE(pair.tick());
    pair.same();
    REQUIRE(pair.applier->skipped() == 0);
    require_no_orphans(pair.client.game);
    // An orphan destroyed in the same tick as its parent needs no Destroy of its own.
    engine_core::Folder& again = add_folder(game, workspace, "Again");
    engine_core::Folder& kid = add_folder(game, again.id(), "Kid");
    add_folder(game, kid.id(), "Grandkid");
    REQUIRE(pair.tick());
    game.destroy(again.id());
    game.destroy(kid.id());
    REQUIRE(pair.tick());
    pair.same();
    REQUIRE(pair.applier->skipped() == 0);
    require_no_orphans(pair.client.game);
}

TEST_CASE("RP33 an instance moved into ServerStorage disappears; moved back, it returns whole", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& box = add_folder(game, workspace, "Box");
    engine_core::PointLight& lamp = game.create<engine_core::PointLight>();
    game.set_parent(lamp.id(), box.id());
    REQUIRE(pair.tick());
    game.set_parent(box.id(), game.service("ServerStorage"));
    REQUIRE(pair.tick());
    REQUIRE(pair.client.game.find_first_child(pair.client.game.scene_service("Workspace"), "Box") == 0);
    // Changed while hidden: the client hears nothing, and gets the current state when it returns.
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 9.0f;
    lamp.set_transform(moved);
    REQUIRE_FALSE(pair.tick());
    game.set_parent(box.id(), workspace);
    REQUIRE(pair.tick());
    pair.same();
    require_no_orphans(pair.client.game);
}

TEST_CASE("RP34 reparents work, under something new this tick, and when two swap places", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& a = add_folder(game, workspace, "A");
    engine_core::Folder& b = add_folder(game, a.id(), "B");
    engine_core::Folder& c = add_folder(game, workspace, "C");
    REQUIRE(pair.tick());
    engine_core::Folder& fresh = add_folder(game, workspace, "Fresh");
    game.set_parent(c.id(), fresh.id());
    REQUIRE(pair.tick());
    pair.same();
    // A held B; now B holds A.
    game.set_parent(b.id(), workspace);
    game.set_parent(a.id(), b.id());
    REQUIRE(pair.tick());
    pair.same();
    REQUIRE(pair.applier->skipped() == 0);
}

TEST_CASE("RP35 RunContext decides what source a client holds; a Server script's edits are never sent", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    engine_core::Script& script = add_script(game, "Logic", "-- secret");
    REQUIRE(pair.tick());
    pair.same();   // a Server shell: empty source
    script.set_source("-- still secret");
    REQUIRE_FALSE(pair.tick());
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    REQUIRE(pair.tick());
    pair.same();   // now the client holds the source
    REQUIRE_FALSE(script.set_run_context(static_cast<int>(engine_core::RunContext::Server)));
    REQUIRE(pair.tick());
    pair.same();   // and it is emptied again
}

TEST_CASE("RP36 quiet physics writes and view-local properties send nothing", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    engine_core::GameObject& object = game.create<engine_core::GameObject>();
    game.set_parent(object.id(), game.scene_service("Workspace"));
    REQUIRE(pair.tick());
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 2.0f;
    game.write_simulated_transform(object.id(), moved);
    REQUIRE_FALSE(pair.tick());
}

TEST_CASE("RP37 a client's own writes stay local until the server writes that property", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    engine_core::Folder& folder = add_folder(game, game.scene_service("Workspace"), "Shared");
    REQUIRE(pair.tick());
    const InstanceId mine = pair.client_id(folder.id());
    pair.client.game.set_name(mine, "ClientName");
    REQUIRE_FALSE(pair.tick());
    REQUIRE(pair.client.game.name(mine) == "ClientName");
    game.set_name(folder.id(), "ServerName");
    REQUIRE(pair.tick());
    REQUIRE(pair.client.game.name(mine) == "ServerName");
}

TEST_CASE("RP38 an operation on an instance the client destroyed is skipped and counted", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    engine_core::Folder& folder = add_folder(game, game.scene_service("Workspace"), "Doomed");
    REQUIRE(pair.tick());
    pair.client.game.destroy(pair.client_id(folder.id()));
    game.set_name(folder.id(), "Renamed");
    REQUIRE(pair.tick());
    REQUIRE(pair.applier->skipped() == 1);
}

TEST_CASE("RP39 a string only a later client's snapshot named reaches the first client in the next change", "[replication][changes]") {
    Pair pair;
    engine_core::DataModel& game = pair.server.game;
    // Created at the identity transform, so its Create names no Transform.
    engine_core::GameObject& object = game.create<engine_core::GameObject>();
    game.set_parent(object.id(), game.scene_service("Workspace"));
    REQUIRE(pair.tick());
    // Moved by a quiet write, as physics moves things: no message says so.
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 5.0f;
    game.write_simulated_transform(object.id(), moved);
    REQUIRE_FALSE(pair.tick());
    // A second client joins: its snapshot is the first message to name "Transform".
    ScriptRig second;
    replication::start_client(second);
    engine_network::ReplicaApplier second_applier(second.game);
    std::vector<std::vector<std::uint8_t>> snapshot;
    pair.replicator->snapshot_messages(0, snapshot);
    REQUIRE(replication::deliver(second_applier, snapshot) == engine_network::ApplyResult::SnapshotApplied);
    // A checked write of Transform: both clients must read the property's name.
    moved.m[12] = 6.0f;
    object.set_transform(moved);
    std::vector<std::uint8_t> message;
    REQUIRE(pair.replicator->end_tick(message));
    REQUIRE(pair.applier->receive_message(message.data(), message.size()) == engine_network::ApplyResult::Ok);
    REQUIRE(second_applier.receive_message(message.data(), message.size()) == engine_network::ApplyResult::Ok);
    pair.same();
    require_same_world(*pair.replicator, game, second.game);
}
