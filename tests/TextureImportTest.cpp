#include "ide/IdeLayout.hpp"
#include "ide/IdeResources.hpp"
#include "ide/TextureImport.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "Project.hpp"
#include "SelectionService.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

// A Texture under Assets.Textures: its Name and Path.
struct Imported {
    engine_core::InstanceId id = 0;
    std::string name;
    std::string path;
};

}  // namespace

// Image files dropped on the studio ask first, then become Textures under
// Assets.Textures whose files are copied into resources/textures, and are
// selected. Replaces the open place.
int RunTextureImportTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    int failures = 0;
    auto expect = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL %s\n", message);
            ++failures;
        }
    };
    namespace fs = std::filesystem;

    expect(ide::is_texture_file("a/Brick.PNG") && ide::is_texture_file("x.jpeg") && ide::is_texture_file("y.tga"),
           "PNG, JPEG, and TGA files are images in any case");
    expect(!ide::is_texture_file("notes.txt") && !ide::is_texture_file("png") && !ide::is_texture_file("theme.css"),
           "a file without an image extension is not");

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path folder = fs::temp_directory_path() / ("anarchy-texture-test-" + std::to_string(stamp));
    const fs::path root = folder / "TexturePlace";
    const fs::path outside = folder / "Downloads";
    fs::create_directories(outside);
    WriteBytes(outside / "Brick.png", "brick bytes");
    WriteBytes(outside / "Stone.jpg", "stone bytes");
    WriteBytes(outside / "notes.txt", "not an image");
    layout.simulation().on_simulation([&](engine_core::DataModel&) {
        engine_core::Project project = engine_core::Project::create(root);
        project.save();
    });
    layout.open_project_at(root);

    auto button = [&scene](const char* id) { return dynamic_cast<jadefx::Button*>(scene.getElementById(id)); };
    auto click = [&button](const char* id) {
        if (jadefx::Button* found = button(id)) {
            found->fire();
        }
    };
    auto drop = [&scene](const std::vector<fs::path>& files) {
        std::vector<std::string> paths;
        for (const fs::path& file : files) {
            paths.push_back(ide::utf8_path(file));
        }
        return scene.noteFileDrop(640, 400, std::move(paths));
    };
    auto textures = [&layout] {
        std::vector<Imported> found;
        layout.simulation().on_simulation([&found](engine_core::DataModel& game) {
            for (engine_core::InstanceId id : game.get_children(game.service("Textures"))) {
                if (const auto* texture = dynamic_cast<const engine_core::Texture*>(game.instance(id))) {
                    found.push_back({id, game.name(id), texture->path()});
                }
            }
        });
        return found;
    };
    auto selection = [&layout] {
        std::vector<engine_core::InstanceId> selected;
        layout.simulation().on_simulation(
            [&selected](engine_core::DataModel& game) { selected = game.selection().get(); });
        return selected;
    };

    expect(!drop({outside / "notes.txt"}), "a drop with no image is not taken");
    expect(button("import-textures-import") == nullptr, "and asks nothing");

    expect(drop({outside / "Brick.png", outside / "notes.txt", outside / "Stone.jpg"}), "a drop with images is taken");
    expect(button("import-textures-import") != nullptr, "and asks before importing");
    click("import-textures-cancel");
    expect(textures().empty(), "Cancel makes no Texture");
    expect(!fs::exists(root / "resources" / "textures" / "Brick.png"), "and copies no file");

    drop({outside / "Brick.png", outside / "notes.txt", outside / "Stone.jpg"});
    click("import-textures-import");
    std::vector<Imported> made = textures();
    expect(made.size() == 2, "Import makes a Texture for each image, and none for the other file");
    if (made.size() == 2) {
        expect(made[0].name == "Brick" && made[0].path == "textures/Brick.png", "named after its file, Path at the copy");
        expect(made[1].name == "Stone" && made[1].path == "textures/Stone.jpg", "in the order dropped");
        expect(selection() == std::vector<engine_core::InstanceId>{made[0].id, made[1].id}, "and they are selected");
    }
    expect(ReadBytes(root / "resources" / "textures" / "Brick.png") == "brick bytes",
           "the file is copied into resources/textures");

    // The same file again shares the copy; another with its name gets its own.
    WriteBytes(folder / "Brick.png", "other brick");
    drop({outside / "Brick.png", folder / "Brick.png"});
    click("import-textures-import");
    made = textures();
    expect(made.size() == 4, "a second import adds Textures");
    if (made.size() == 4) {
        expect(made[2].path == "textures/Brick.png", "the same bytes reuse the copy there");
        expect(made[3].path == "textures/Brick-2.png", "a different file with that name gets another");
    }
    expect(ReadBytes(root / "resources" / "textures" / "Brick-2.png") == "other brick", "which holds its bytes");

    // A file already in the resources folder is named where it is.
    drop({root / "resources" / "textures" / "Brick-2.png"});
    click("import-textures-import");
    made = textures();
    expect(made.size() == 5 && made.back().path == "textures/Brick-2.png", "a resource file is not copied again");
    expect(!fs::exists(root / "resources" / "textures" / "Brick-2-2.png"), "and leaves no copy");

    std::error_code error;
    fs::remove_all(folder, error);
    return failures;
}
