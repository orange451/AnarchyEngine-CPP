#include "ide/IdeLayout.hpp"
#include "ide/IdeResources.hpp"
#include "ide/ScratchResources.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "Project.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

}  // namespace

// A place never saved keeps what it imports in a scratch folder under the
// system's temporary folder; its first Save moves that into the project, and
// New and Open delete it. Replaces the open place.
int RunScratchResourcesTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    int failures = 0;
    auto expect = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL %s\n", message);
            ++failures;
        }
    };

    const fs::path scratch = ide::new_scratch_resources();
    expect(!scratch.empty() && ide::is_scratch_resources(scratch), "a scratch folder has the scratch shape");
    std::error_code same;
    expect(fs::equivalent(scratch.parent_path().parent_path().parent_path(), fs::temp_directory_path(), same),
           "under the system's temporary folder");
    expect(!fs::exists(scratch), "and is not made until something writes there");
    expect(ide::new_scratch_resources() != scratch, "each one is new");
    expect(!ide::is_scratch_resources(fs::temp_directory_path() / "resources") &&
               !ide::is_scratch_resources(fs::temp_directory_path() / "Project" / "resources"),
           "a resources folder elsewhere is not scratch");

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path folder = fs::temp_directory_path() / ("anarchy-scratch-test-" + std::to_string(stamp));

    // Moving: every file goes, keeping its folders, except one the project has.
    {
        const fs::path resources = folder / "Moved" / "resources";
        WriteBytes(scratch / "textures" / "Brick.png", "scratch brick");
        WriteBytes(scratch / "meshes" / "Crate" / "Wood.amesh", "wood mesh");
        WriteBytes(resources / "textures" / "Brick.png", "project brick");
        std::string error;
        expect(ide::move_scratch_resources(scratch, resources, error), "the scratch folder moves into a project");
        expect(ReadBytes(resources / "meshes" / "Crate" / "Wood.amesh") == "wood mesh", "with its folders");
        expect(ReadBytes(resources / "textures" / "Brick.png") == "project brick", "keeping a file the project has");
        expect(!fs::exists(scratch.parent_path()), "and the scratch folder is gone");
    }
    {
        const fs::path elsewhere = folder / "NotScratch" / "resources";
        WriteBytes(elsewhere / "keep.txt", "keep");
        ide::remove_scratch_resources(elsewhere);
        expect(fs::exists(elsewhere / "keep.txt"), "a folder that is not scratch is never deleted");
    }

    // In the studio: an untitled place imports without a project.
    WriteBytes(folder / "Downloads" / "Brick.png", "brick bytes");
    auto click = [&scene](const char* id) {
        if (auto* button = dynamic_cast<jadefx::Button*>(scene.getElementById(id))) {
            button->fire();
        }
    };
    // File > New, dropping the place it replaces.
    auto new_place = [&] {
        scene.noteKey(jadefx::Key::N, true, false, jadefx::Key::ModControl);
        scene.noteKey(jadefx::Key::N, false, false, 0);
        click("unsaved-discard");
    };
    auto import_brick = [&] {
        scene.noteFileDrop(640, 400, {ide::utf8_path(folder / "Downloads" / "Brick.png")});
        click("import-files-import");
    };
    auto resources_root = [&layout] {
        fs::path root;
        layout.simulation().on_simulation([&root](engine_core::DataModel& game) { root = game.resources_root(); });
        return root;
    };
    auto texture_paths = [&layout] {
        std::vector<std::string> paths;
        layout.simulation().on_simulation([&paths](engine_core::DataModel& game) {
            for (engine_core::InstanceId id : game.get_children(game.service("Textures"))) {
                if (const auto* texture = dynamic_cast<const engine_core::Texture*>(game.instance(id))) {
                    paths.push_back(texture->path());
                }
            }
        });
        return paths;
    };

    new_place();
    const fs::path first = resources_root();
    expect(ide::is_scratch_resources(first), "New points the untitled place's resources at a scratch folder");
    import_brick();
    expect(texture_paths() == std::vector<std::string>{"textures/Brick.png"}, "an untitled place imports a texture");
    expect(ReadBytes(first / "textures" / "Brick.png") == "brick bytes", "into its scratch folder");

    new_place();
    const fs::path second = resources_root();
    expect(ide::is_scratch_resources(second) && second != first, "New starts another scratch folder");
    expect(!fs::exists(first.parent_path()), "and deletes the last one, which went with its place");

    import_brick();
    const fs::path saved = folder / "Saved";
    expect(layout.save_project_to(saved), "the untitled place saves");
    expect(resources_root() == saved / "resources", "its resources are the project's from then on");
    expect(ReadBytes(saved / "resources" / "textures" / "Brick.png") == "brick bytes",
           "and the first Save moved what it imported into the project, under the same Path");
    expect(texture_paths() == std::vector<std::string>{"textures/Brick.png"}, "so the Texture still names it");
    expect(!fs::exists(second.parent_path()), "and deleted the scratch folder");

    new_place();
    const fs::path third = resources_root();
    import_brick();
    expect(fs::exists(third / "textures" / "Brick.png"), "a new place imports into scratch again");
    layout.open_project_at(saved);
    expect(resources_root() == saved / "resources", "opening a project uses its resources");
    expect(!fs::exists(third.parent_path()), "and deletes the untitled place's scratch folder");

    std::error_code error;
    fs::remove_all(folder, error);
    return failures;
}
