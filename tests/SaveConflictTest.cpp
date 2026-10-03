#include "ide/IdeLayout.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "GameObject.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace {

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

}  // namespace

// Saving over a file changed outside the studio asks first. Cancel writes
// nothing, and Overwrite writes the studio's version over the files it listed.
// Replaces the open place.
int RunSaveConflictTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    int failures = 0;
    auto expect = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL %s\n", message);
            ++failures;
        }
    };
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path folder = fs::temp_directory_path() / ("anarchy-conflict-test-" + std::to_string(stamp));
    const fs::path root = folder / "ConflictPlace";
    std::string part_file;
    std::string spare_file;
    layout.simulation().on_simulation([&](engine_core::DataModel&) {
        engine_core::Project project = engine_core::Project::create(root);
        engine_core::DataModel& game = project.datamodel();
        for (const char* name : {"Part", "Spare"}) {
            engine_core::GameObject& part = game.create_game_object();
            game.set_name(part.id(), name);
            game.set_parent(part.id(), game.scene_service("Workspace"));
            (part_file.empty() ? part_file : spare_file) = "src/Workspace.workspace/" + std::string(name) + "." + game.guid(part.id()) + ".json";
        }
        project.save();
    });
    layout.open_project_at(root);

    // Moves a part in the studio to (0.25, 0.5, z).
    auto move_part = [&layout](const char* name, float z) {
        layout.simulation().on_simulation([name, z](engine_core::DataModel& game) {
            // As an edit made in the studio: one undo step, so there is something to save.
            const std::optional<std::string> step = game.history().try_begin_recording("Move");
            game.game_object(game.find_first_child(game.scene_service("Workspace"), name))
                ->set_position(engine_core::Vec3{0.25f, 0.5f, z});
            if (step) {
                game.history().finish_recording(*step, engine_core::FinishRecordingOperation::Commit);
            }
        });
    };
    // Something outside the studio changes a property in a file.
    auto touch = [&root](const std::string& file) {
        engine_core::JsonValue doc;
        std::string error;
        engine_core::parse_json(ReadBytes(root / file), doc, error);
        // The studio changes Transform too, so both sides changed the same property.
        const engine_core::Matrix4 moved = engine_core::matrix4_translation(0.f, 1.f, 0.f);
        doc.set("Transform", engine_core::json_floats(moved.m, 16));
        const std::string bytes = engine_core::write_json(doc);
        WriteBytes(root / file, bytes);
        return bytes;
    };
    auto press = [&scene](int key) {
        scene.noteKey(key, true, false, jadefx::Key::ModControl);
        scene.noteKey(key, false, false, 0);
    };
    auto button = [&scene](const char* id) { return dynamic_cast<jadefx::Button*>(scene.getElementById(id)); };
    auto click = [&button](const char* id) {
        if (jadefx::Button* found = button(id)) {
            found->fire();
        }
    };
    auto has_part = [&layout] {
        bool found = false;
        layout.simulation().on_simulation(
            [&found](engine_core::DataModel& game) { found = game.find_first_child(game.scene_service("Workspace"), "Part") != 0; });
        return found;
    };

    // The studio changes both parts while something else edits Part's file.
    move_part("Part", 0.75f);
    move_part("Spare", 0.75f);
    const std::string outside = touch(part_file);

    press(jadefx::Key::S);
    expect(button("save-conflict-cancel") != nullptr, "saving over a file changed on disk asks first");
    expect(ReadBytes(root / part_file) == outside, "and writes nothing while it asks");
    click("save-conflict-cancel");
    expect(ReadBytes(root / part_file) == outside, "Cancel leaves the changed file as it is");
    expect(layout.has_unsaved_changes(), "and the place still has unsaved changes");

    // Spare's file changes while the studio asks about Part's.
    press(jadefx::Key::S);
    const std::string spare_outside = touch(spare_file);
    click("save-conflict-overwrite");
    expect(button("save-conflict-overwrite") != nullptr, "a file that changed while it asked is asked about");
    expect(ReadBytes(root / spare_file) == spare_outside, "and is not written over unasked");
    click("save-conflict-overwrite");
    const std::string studio_transform = "\"Transform\": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.25, 0.5, 0.75, 1]";
    expect(ReadBytes(root / part_file).find(studio_transform) != std::string::npos,
           "Overwrite writes the studio's transform");
    expect(ReadBytes(root / spare_file).find(studio_transform) != std::string::npos, "over every file it listed");
    expect(!layout.has_unsaved_changes(), "and the place is saved");

    // File > New offers to save first; a conflict then asks too, and only a
    // save that happens lets the new place start.
    move_part("Part", 0.5f);
    touch(part_file);
    press(jadefx::Key::N);
    click("unsaved-save");
    expect(button("save-conflict-cancel") != nullptr, "saving before New asks about the changed file");
    click("save-conflict-cancel");
    expect(has_part(), "Cancel keeps the place open");
    press(jadefx::Key::N);
    click("unsaved-save");
    click("save-conflict-overwrite");
    expect(!has_part(), "Overwrite saves, then the new place starts");
    expect(ReadBytes(root / part_file).find("\"Transform\": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.25, 0.5, 0.5, 1]") !=
               std::string::npos,
           "with the studio's transform on disk");

    std::error_code error;
    fs::remove_all(folder, error);
    return failures;
}
