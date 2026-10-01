#include "ide/AssetImport.hpp"
#include "ide/CutSet.hpp"
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
    expect(button("import-files-import") == nullptr, "and asks nothing");

    expect(drop({outside / "Brick.png", outside / "notes.txt", outside / "Stone.jpg"}), "a drop with images is taken");
    expect(button("import-files-import") != nullptr, "and asks before importing");
    click("import-files-cancel");
    expect(textures().empty(), "Cancel makes no Texture");
    expect(!fs::exists(root / "resources" / "textures" / "Brick.png"), "and copies no file");

    drop({outside / "Brick.png", outside / "notes.txt", outside / "Stone.jpg"});
    click("import-files-import");
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
    click("import-files-import");
    made = textures();
    expect(made.size() == 4, "a second import adds Textures");
    if (made.size() == 4) {
        expect(made[2].path == "textures/Brick.png", "the same bytes reuse the copy there");
        expect(made[3].path == "textures/Brick-2.png", "a different file with that name gets another");
    }
    expect(ReadBytes(root / "resources" / "textures" / "Brick-2.png") == "other brick", "which holds its bytes");

    // A file already in the resources folder is named where it is.
    drop({root / "resources" / "textures" / "Brick-2.png"});
    click("import-files-import");
    made = textures();
    expect(made.size() == 5 && made.back().path == "textures/Brick-2.png", "a resource file is not copied again");
    expect(!fs::exists(root / "resources" / "textures" / "Brick-2-2.png"), "and leaves no copy");

    // Sounds: a Sound under Assets.Audio, its file in resources/sounds.
    expect(ide::is_sound_file("Boom.WAV") && ide::is_sound_file("a.mp3") && ide::is_sound_file("a.flac") &&
               ide::is_sound_file("a.ogg"),
           "WAV, MP3, FLAC, and Ogg files are sounds in any case");
    expect(!ide::is_sound_file("a.png") && !ide::is_sound_file("wav"), "an image, or no extension, is not");
    WriteBytes(outside / "Boom.wav", "boom bytes");
    auto sounds = [&layout](engine_core::InstanceId folder) {
        std::vector<Imported> found;
        layout.simulation().on_simulation([&found, folder](engine_core::DataModel& game) {
            for (engine_core::InstanceId id : game.get_children(folder != 0 ? folder : game.service("Audio"))) {
                if (const auto* sound = dynamic_cast<const engine_core::Sound*>(game.instance(id))) {
                    found.push_back({id, game.name(id), sound->path()});
                }
            }
        });
        return found;
    };
    expect(drop({outside / "Boom.wav"}), "a drop with a sound is taken");
    click("import-files-import");
    std::vector<Imported> boom = sounds(0);
    expect(boom.size() == 1 && boom[0].name == "Boom" && boom[0].path == "sounds/Boom.wav",
           "Import makes a Sound in Assets.Audio, Path at the copy");
    expect(ReadBytes(root / "resources" / "sounds" / "Boom.wav") == "boom bytes",
           "the file is copied into resources/sounds");
    expect(!boom.empty() && selection() == std::vector<engine_core::InstanceId>{boom[0].id}, "and it is selected");

    // Import Sound into a Folder under Assets.Audio puts it there.
    engine_core::InstanceId effects = 0;
    layout.simulation().on_simulation([&effects](engine_core::DataModel& game) {
        std::string refused;
        effects = ide::insert_instance(game, "Folder", game.service("Audio"), refused);
    });
    const std::vector<ide::PreparedAsset> prepared =
        ide::prepare_assets(root / "resources", {ide::utf8_path(outside / "Boom.wav")});
    expect(prepared.size() == 1 && prepared[0].sound && prepared[0].error.empty(), "prepare reads a sound");
    layout.simulation().on_simulation(
        [&prepared, effects](engine_core::DataModel& game) { ide::place_assets(game, prepared, effects); });
    expect(sounds(effects).size() == 1, "place_assets puts a sound in the folder it is given");

    // Import Texture into a Folder under Assets.Textures puts it there; a folder in another category does not take it.
    engine_core::InstanceId walls = 0;
    layout.simulation().on_simulation([&walls](engine_core::DataModel& game) {
        std::string refused;
        walls = ide::insert_instance(game, "Folder", game.service("Textures"), refused);
    });
    const std::vector<ide::PreparedAsset> brick =
        ide::prepare_assets(root / "resources", {ide::utf8_path(outside / "Brick.png")});
    std::vector<ide::PlacedAsset> placed;
    layout.simulation().on_simulation([&](engine_core::DataModel& game) {
        placed = ide::place_assets(game, brick, walls);
        const std::vector<ide::PlacedAsset> elsewhere = ide::place_assets(game, brick, effects);
        placed.insert(placed.end(), elsewhere.begin(), elsewhere.end());
    });
    engine_core::InstanceId walls_parent = 0;
    engine_core::InstanceId elsewhere_parent = 0;
    layout.simulation().on_simulation([&](engine_core::DataModel& game) {
        walls_parent = placed.size() == 2 ? game.parent(placed[0].root) : 0;
        elsewhere_parent = placed.size() == 2 && placed[1].root != 0 ? game.parent(placed[1].root) : 0;
        elsewhere_parent = elsewhere_parent == game.service("Textures") ? elsewhere_parent : 0;
    });
    expect(walls != 0 && walls_parent == walls, "place_assets puts a texture in a Folder under Textures it is given");
    expect(elsewhere_parent != 0, "and a folder under Audio leaves a texture in Assets.Textures");

    std::error_code error;
    fs::remove_all(folder, error);
    return failures;
}
