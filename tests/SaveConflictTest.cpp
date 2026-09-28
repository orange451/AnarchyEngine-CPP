#include "ide/IdeLayout.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "GameObject.hpp"
#include "Project.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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
// nothing, and Overwrite writes the studio's version. Replaces the open place.
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
    layout.simulation().on_simulation([&](engine_core::DataModel&) {
        engine_core::Project project = engine_core::Project::create(root);
        engine_core::DataModel& game = project.datamodel();
        engine_core::GameObject& part = game.create_game_object();
        game.set_name(part.id(), "Part");
        game.set_parent(part.id(), 0);
        project.save();
        part_file = "src/Part." + game.guid(part.id()) + ".json";
    });
    layout.open_project_at(root);

    // The studio changes the part's color while something else edits its file.
    layout.simulation().on_simulation([](engine_core::DataModel& game) {
        engine_core::ColorRgb color;
        color.r = 0.25f;
        color.g = 0.5f;
        color.b = 0.75f;
        game.game_object(game.find_first_child(0, "Part"))->set_color(color);
    });
    const std::string outside = ReadBytes(root / part_file) + "\n";
    WriteBytes(root / part_file, outside);

    auto save = [&scene] {
        scene.noteKey(jadefx::Key::S, true, false, jadefx::Key::ModControl);
        scene.noteKey(jadefx::Key::S, false, false, 0);
    };
    save();
    auto* cancel = dynamic_cast<jadefx::Button*>(scene.getElementById("save-conflict-cancel"));
    expect(cancel != nullptr, "saving over a file changed on disk asks first");
    expect(ReadBytes(root / part_file) == outside, "and writes nothing while it asks");
    if (cancel != nullptr) {
        cancel->fire();
    }
    expect(ReadBytes(root / part_file) == outside, "Cancel leaves the changed file as it is");
    expect(layout.has_unsaved_changes(), "and the place still has unsaved changes");

    save();
    auto* overwrite = dynamic_cast<jadefx::Button*>(scene.getElementById("save-conflict-overwrite"));
    expect(overwrite != nullptr, "saving again asks again");
    if (overwrite != nullptr) {
        overwrite->fire();
    }
    expect(ReadBytes(root / part_file).find("\"Color\": [0.25, 0.5, 0.75]") != std::string::npos,
           "Overwrite writes the studio's color");
    expect(!layout.has_unsaved_changes(), "and the place is saved");

    std::error_code error;
    fs::remove_all(folder, error);
    return failures;
}
