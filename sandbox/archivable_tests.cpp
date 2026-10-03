// Archivable: whether a save writes an instance.

#include "support.hpp"

#include "Folder.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

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

}  // namespace

TEST_CASE("AR1 Archivable is true by default, and Lua reads and writes it", "[AR1]") {
    ScriptRig rig;
    add_script(rig.game, "Probe", R"(
        local folder = Instance.new("Folder")
        print("default", folder.Archivable)
        folder.Archivable = false
        print("set", folder.Archivable)
    )");
    rig.game.start_simulation();
    rig.frames(1);
    const auto output = rig.runtime.drain_output();
    INFO(rig.runtime.last_error());
    REQUIRE(has_line(output, "default\ttrue\n"));
    REQUIRE(has_line(output, "set\tfalse\n"));
}

TEST_CASE("AR2 a save leaves out a non-archivable instance and its subtree, and Stop still restores it",
          "[AR2][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const InstanceId workspace = game.scene_service("Workspace");
    const InstanceId kept = add_folder(game, "Kept", workspace);
    const InstanceId gone = add_folder(game, "Gone", workspace);
    add_folder(game, "Inner", gone);
    project.save();
    REQUIRE_FALSE(project.unsaved());
    game.set_archivable(gone, false);
    REQUIRE(project.unsaved());
    project.save();

    engine_core::Game other;
    engine_core::Project read = engine_core::Project::load(dir.path, other);
    REQUIRE(other.find_first_child(other.scene_service("Workspace"), "Kept") != 0);
    REQUIRE(other.find_first_child(other.scene_service("Workspace"), "Gone") == 0);

    // As the studio's Test does: capture the place as it is now, then play.
    game.capture_place();
    game.start_simulation();
    game.set_name(gone, "Renamed");
    game.stop_simulation();
    REQUIRE(game.alive(gone));
    REQUIRE(game.name(gone) == "Gone");
    REQUIRE(game.alive(kept));
}

TEST_CASE("AR2b a disk sync does not destroy a live non-archivable instance", "[AR2b][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    const InstanceId gone = add_folder(game, "Gone", game.scene_service("Workspace"));
    game.set_archivable(gone, false);
    project.save();
    project.apply_disk();
    REQUIRE(game.alive(gone));
}
