// Persistent: what survives a world swap. Saved and scriptable, never replicated
// (docs/superpowers/specs/2026-10-19-join-from-lua-design.md, "Persistent").

#include "replication_support.hpp"

#include "Folder.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using engine_core::InstanceId;

InstanceId add_folder(engine_core::DataModel& game, const char* name, InstanceId parent) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    game.set_parent(folder.id(), parent);
    return folder.id();
}

bool has_line(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const auto& line : batch.lines) {
        if (line.text == text) {
            return true;
        }
    }
    return false;
}

// Whether any file the project wrote holds text.
bool project_mentions(const fs::path& root, const std::string& text) {
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (body.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("JN1 Persistent defaults to false, is scriptable, and round-trips a project save", "[join][persistent]") {
    {
        ScriptRig rig;
        add_script(rig.game, "Probe", R"(
            local folder = Instance.new("Folder")
            print("default", folder.Persistent)
            folder.Persistent = true
            print("set", folder.Persistent)
        )");
        rig.game.start_simulation();
        rig.frames(1);
        const auto output = rig.runtime.drain_output();
        INFO(rig.runtime.last_error());
        REQUIRE(has_line(output, "default\tfalse\n"));
        REQUIRE(has_line(output, "set\ttrue\n"));
    }

    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const InstanceId workspace = game.scene_service("Workspace");
    const InstanceId plain = add_folder(game, "Plain", workspace);
    const InstanceId kept = add_folder(game, "Kept", workspace);
    REQUIRE_FALSE(game.persistent(plain));
    project.save();
    // A Folder left false writes no Persistent key.
    REQUIRE_FALSE(project_mentions(dir.path, "\"Persistent\""));

    // As Properties writes it: one undo step, which marks the place unsaved.
    begin_step(game);
    game.set_persistent(kept, true);
    end_step(game);
    REQUIRE(game.persistent(kept));
    REQUIRE(project.unsaved());
    project.save();
    REQUIRE(project_mentions(dir.path, "\"Persistent\""));

    engine_core::Game other;
    engine_core::Project read = engine_core::Project::load(dir.path, other);
    const InstanceId other_workspace = other.scene_service("Workspace");
    const InstanceId read_kept = other.find_first_child(other_workspace, "Kept");
    const InstanceId read_plain = other.find_first_child(other_workspace, "Plain");
    REQUIRE(read_kept != 0);
    REQUIRE(read_plain != 0);
    REQUIRE(other.persistent(read_kept));
    REQUIRE_FALSE(other.persistent(read_plain));
}

TEST_CASE("JN2 a service refuses Persistent", "[join][persistent]") {
    ScriptRig rig;
    add_script(rig.game, "Probe", R"(
        local ok, err = pcall(function() workspace.Persistent = true end)
        print("ok", ok)
        print("err", err)
        print("still", workspace.Persistent)
    )");
    rig.game.start_simulation();
    rig.frames(1);
    const auto output = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(output, "ok\tfalse\n"));
    bool found = false;
    for (const auto& line : output.lines) {
        if (line.text.rfind("err\t", 0) == 0 &&
            line.text.find("Workspace is a service, and a service always survives") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
    REQUIRE(has_line(output, "still\tfalse\n"));
    REQUIRE_FALSE(rig.game.persistent(rig.game.scene_service("Workspace")));
}

TEST_CASE("JN3 Persistent never replicates", "[join][persistent]") {
    ScriptRig server;
    ScriptRig client;
    replication::start_server(server);
    replication::start_client(client);
    const InstanceId workspace = server.game.scene_service("Workspace");
    const InstanceId marked = add_folder(server.game, "Marked", workspace);
    const InstanceId later = add_folder(server.game, "Later", workspace);
    server.game.set_persistent(marked, true);

    engine_network::Replicator replicator(server.game);
    engine_network::ReplicaApplier applier(client.game);
    std::vector<std::vector<std::uint8_t>> messages;
    replicator.snapshot_messages(0, messages);
    REQUIRE(replication::deliver(applier, messages) == engine_network::ApplyResult::SnapshotApplied);

    const InstanceId client_marked = applier.local(marked);
    REQUIRE(client_marked != 0);
    REQUIRE_FALSE(client.game.persistent(client_marked));

    // The snapshot's pending changes go out first.
    std::vector<std::uint8_t> message;
    replicator.end_tick(message);
    // Setting it during play sends no op.
    server.game.set_persistent(later, true);
    REQUIRE(server.game.persistent(later));
    REQUIRE_FALSE(replicator.end_tick(message));
    REQUIRE(message.empty());
    REQUIRE_FALSE(client.game.persistent(applier.local(later)));
}
