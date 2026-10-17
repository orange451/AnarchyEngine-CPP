// The tree change sink replication reads
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 1).

#include "support.hpp"

#include "Attachment.hpp"
#include "Folder.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "Script.hpp"
#include "TreeChange.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using engine_core::DataModel;
using engine_core::Field;
using engine_core::InstanceId;
using engine_core::TreeChange;

namespace {

struct Recorder {
    std::vector<TreeChange> changes;
    void attach(DataModel& game) {
        game.set_change_sink([this](const TreeChange& change) { changes.push_back(change); });
    }
    std::size_t count(TreeChange::Kind kind, InstanceId id) const {
        std::size_t n = 0;
        for (const TreeChange& c : changes) n += (c.kind == kind && c.id == id) ? 1 : 0;
        return n;
    }
};

}  // namespace

TEST_CASE("SC1 parenting a new instance reports Parented from nowhere, once", "[server-client][tree-change]") {
    SimRole role;
    engine_core::Game game;
    Recorder recorder;
    recorder.attach(game);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    REQUIRE(recorder.changes.empty());   // create alone reports nothing
    game.set_parent(folder.id(), game.scene_service("Workspace"));
    REQUIRE(recorder.count(TreeChange::Kind::Parented, folder.id()) == 1);
    REQUIRE(recorder.changes.back().previous_parent == DataModel::kNoParent);
    // set_parent's own Parent field change is not reported a second time.
    for (const TreeChange& c : recorder.changes) {
        REQUIRE_FALSE((c.kind == TreeChange::Kind::Property && c.field == Field::Parent));
    }
    // Moving reports the old parent.
    game.set_parent(folder.id(), game.scene_service("Storage"));
    REQUIRE(recorder.changes.back().kind == TreeChange::Kind::Parented);
    REQUIRE(recorder.changes.back().previous_parent == game.scene_service("Workspace"));
    // Setting the same parent again reports nothing.
    const std::size_t before = recorder.changes.size();
    game.set_parent(folder.id(), game.scene_service("Storage"));
    REQUIRE(recorder.changes.size() == before);
}

TEST_CASE("SC2 rename, a built-in field, and a registry property each report once", "[server-client][tree-change]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Script& script = add_script(game, "S", "-- nothing");
    engine_core::Attachment& attachment = game.create<engine_core::Attachment>();
    game.set_parent(attachment.id(), game.scene_service("Workspace"));
    Recorder recorder;
    recorder.attach(game);
    game.set_name(script.id(), "Renamed");
    REQUIRE(recorder.count(TreeChange::Kind::Renamed, script.id()) == 1);
    for (const TreeChange& c : recorder.changes) {
        REQUIRE_FALSE((c.kind == TreeChange::Kind::Property && c.field == Field::Name));
    }
    recorder.changes.clear();
    script.set_source("print(1)");
    REQUIRE(recorder.changes.size() == 1);
    REQUIRE(recorder.changes[0].kind == TreeChange::Kind::Property);
    REQUIRE(recorder.changes[0].field == Field::Source);
    recorder.changes.clear();
    REQUIRE_FALSE(attachment.set_offset_space(static_cast<int>(engine_core::TransformSpace::World)));
    bool saw_offset_space = false;
    for (const TreeChange& c : recorder.changes) {
        if (c.kind == TreeChange::Kind::Property && c.field == Field::Reflected &&
            c.payload == engine_core::lua_property_id("OffsetSpace")) {
            saw_offset_space = true;
        }
    }
    REQUIRE(saw_offset_space);
}

TEST_CASE("SC3 destroy reports Destroyed for the instance", "[server-client][tree-change]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), game.scene_service("Workspace"));
    const InstanceId id = folder.id();
    Recorder recorder;
    recorder.attach(game);
    game.destroy(id);
    REQUIRE(recorder.count(TreeChange::Kind::Destroyed, id) == 1);
}

TEST_CASE("SC4 a quiet simulated transform write reports nothing; a script transform write reports once", "[server-client][tree-change]") {
    SimRole role;
    engine_core::Game game;
    engine_core::GameObject& object = game.create<engine_core::GameObject>();
    game.set_parent(object.id(), game.scene_service("Workspace"));
    Recorder recorder;
    recorder.attach(game);
    engine_core::Matrix4 moved = engine_core::matrix4_identity();
    moved.m[12] = 5.0f;
    // Physics' quiet path (DataModel::write_simulated_transform).
    game.write_simulated_transform(object.id(), moved);
    REQUIRE(recorder.changes.empty());
    REQUIRE(object.transform().m[12] == 5.0f);   // the write took effect, so the silence is real
    moved.m[12] = 6.0f;
    object.set_transform(moved);   // the checked setter scripts reach
    REQUIRE(recorder.changes.size() == 1);
    REQUIRE(recorder.changes[0].field == Field::Transform);
}

TEST_CASE("SC5 with no sink set nothing is reported and clearing it stops reports", "[server-client][tree-change]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE_FALSE(game.has_change_sink());
    Recorder recorder;
    recorder.attach(game);
    REQUIRE(game.has_change_sink());
    game.set_change_sink({});
    REQUIRE_FALSE(game.has_change_sink());
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), game.scene_service("Workspace"));
    REQUIRE(recorder.changes.empty());
}

TEST_CASE("SC6 destroy reports the orphaned children; destroy_tree reports only Destroyed", "[server-client][tree-change]") {
    SimRole role;
    engine_core::Game game;
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), game.scene_service("Workspace"));
    engine_core::Folder& a = game.create<engine_core::Folder>();
    engine_core::Folder& b = game.create<engine_core::Folder>();
    game.set_parent(a.id(), folder.id());
    game.set_parent(b.id(), folder.id());
    const InstanceId folder_id = folder.id(), a_id = a.id(), b_id = b.id();
    Recorder recorder;
    recorder.attach(game);
    game.destroy(folder_id);
    REQUIRE(recorder.count(TreeChange::Kind::Destroyed, folder_id) == 1);
    for (const InstanceId child : {a_id, b_id}) {
        REQUIRE(recorder.count(TreeChange::Kind::Parented, child) == 1);
        REQUIRE(game.parent(child) == DataModel::kNoParent);
        for (const TreeChange& c : recorder.changes) {
            if (c.kind == TreeChange::Kind::Parented && c.id == child) REQUIRE(c.previous_parent == folder_id);
        }
    }

    engine_core::Folder& root = game.create<engine_core::Folder>();
    game.set_parent(root.id(), game.scene_service("Workspace"));
    engine_core::Folder& mid = game.create<engine_core::Folder>();
    engine_core::Folder& leaf = game.create<engine_core::Folder>();
    game.set_parent(mid.id(), root.id());
    game.set_parent(leaf.id(), mid.id());
    const InstanceId ids[3] = {root.id(), mid.id(), leaf.id()};
    recorder.changes.clear();
    game.destroy_tree(ids[0]);
    for (const InstanceId id : ids) REQUIRE(recorder.count(TreeChange::Kind::Destroyed, id) == 1);
    for (const TreeChange& c : recorder.changes) REQUIRE(c.kind == TreeChange::Kind::Destroyed);
}
