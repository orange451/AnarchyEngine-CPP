#include "runner/GamePack.hpp"

#include "FileBytes.hpp"
#include "ide/GameExport.hpp"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

int gFailures = 0;

void Expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++gFailures;
    }
}

std::string Read(const fs::path& path) {
    std::string bytes;
    std::string error;
    engine_core::read_file(path, bytes, error);
    return bytes;
}

void Write(const fs::path& path, const std::string& bytes) {
    std::string error;
    engine_core::write_file(path, bytes, error);
}

// A folder of its own under the temporary folder, removed when the test ends.
struct TempFolder {
    TempFolder() {
        std::random_device random;
        path = fs::temp_directory_path() / ("anarchy-pack-test-" + std::to_string(random()));
        fs::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    fs::path path;
};

}  // namespace

int RunGamePackTests() {
    const int before = gFailures;
    // Where the player saves a profile: a played folder, else beside the program or its .app.
    Expect(runner::player_capture_folder(fs::path("/games/Maze"), false, fs::path("/apps/AnarchyPlayer")) ==
               fs::path("/games/Maze"),
           "a played project folder holds its profiles");
    Expect(runner::player_capture_folder(fs::path("/tmp/x/project"), true, fs::path("/games/Maze.exe")) ==
               fs::path("/games"),
           "a packed game's profiles go beside the program");
    Expect(runner::player_capture_folder(fs::path("/tmp/x/project"), true,
                                         fs::path("/Games/Maze.app/Contents/MacOS/Maze")) == fs::path("/Games"),
           "and beside the .app on a Mac");
    TempFolder temp;
    const fs::path project = temp.path / "project";
    Write(project / "project.json", "{\"format\": 1}");
    Write(project / "src" / "init.json", "{}");
    Write(project / "src" / "Box.1234" / "init.json", "{\"class\": \"GameObject\"}");
    // Binary bytes, a zero among them, come back whole.
    Write(project / "resources" / "textures" / "brick.png", std::string("\x89PNG\0\r\n", 7));
    Write(project / "resources" / "textures" / ".gitkeep", "");
    Write(project / ".git" / "HEAD", "ref: refs/heads/main");
    // Bigger than one copy chunk.
    const std::string big(3 * 1024 * 1024 + 17, 'x');
    Write(project / "resources" / "audio" / "song.ogg", big);
    const fs::path program = temp.path / "player.exe";
    Write(program, "MZ the player program");

    std::vector<runner::PackFile> files;
    std::string error;
    files.push_back({"game/project.json", project / "project.json"});
    Expect(runner::add_pack_folder(project / "src", "game/src/", files, error), "pack: adds src");
    Expect(runner::add_pack_folder(project / "resources", "game/resources/", files, error), "pack: adds resources");
    Expect(files.size() == 5, "pack: dot files are left out");
    Expect(!runner::add_pack_folder(project / "missing", "x/", files, error), "pack: a missing folder fails");

    const fs::path game = temp.path / "MyGame.exe";
    Expect(runner::write_game_pack(program, files, game, error), "pack: writes the game");
    Expect(Read(game).rfind("MZ the player program", 0) == 0, "pack: the game starts with the program");
    Expect(!fs::exists(temp.path / "MyGame.exe.part"), "pack: no part file is left");

    Expect(!runner::find_game_pack(program).has_value(), "pack: the bare program has no game");
    const std::optional<runner::PackLocation> where = runner::find_game_pack(game);
    Expect(where.has_value(), "pack: the game is found");
    if (!where) {
        return gFailures - before;
    }
    Expect(where->offset == fs::file_size(program), "pack: the archive starts after the program");

    const fs::path cache = temp.path / "cache";
    fs::path folder;
    Expect(runner::unpack_game(game, *where, cache, folder, error), "unpack: extracts");
    Expect(Read(folder / "game" / "src" / "Box.1234" / "init.json") == "{\"class\": \"GameObject\"}",
           "unpack: a nested file");
    Expect(Read(folder / "game" / "resources" / "textures" / "brick.png") == std::string("\x89PNG\0\r\n", 7),
           "unpack: binary bytes");
    Expect(Read(folder / "game" / "resources" / "audio" / "song.ogg") == big, "unpack: a large file");
    Expect(!fs::exists(folder / "game" / "resources" / "textures" / ".gitkeep"), "unpack: no dot file");

    // A second run reuses the folder, even with a file in it changed.
    Write(folder / "game" / "project.json", "changed");
    fs::path again;
    Expect(runner::unpack_game(game, *where, cache, again, error) && again == folder, "unpack: reuses the folder");
    Expect(Read(folder / "game" / "project.json") == "changed", "unpack: does not extract again");
    // A folder without its mark, as a crash leaves it, is extracted again.
    fs::remove(folder / ".complete");
    Expect(runner::unpack_game(game, *where, cache, again, error), "unpack: redoes a half folder");
    Expect(Read(folder / "game" / "project.json") == "{\"format\": 1}", "unpack: the half folder is replaced");

    // Without a program, the file is the archive alone, as a Mac game.pak is.
    const fs::path pak = temp.path / "game.pak";
    Expect(runner::write_game_pack({}, files, pak, error), "pak: writes");
    const std::optional<runner::PackLocation> alone = runner::find_game_pack(pak);
    Expect(alone && alone->offset == 0 && alone->hash == where->hash, "pak: same archive, same hash");

    // No path that leads out of the folder is packed.
    std::vector<runner::PackFile> escaping = {{"../outside.txt", project / "project.json"}};
    Expect(!runner::write_game_pack({}, escaping, temp.path / "bad.pak", error), "pack: refuses ..");
    escaping = {{"C:/outside.txt", project / "project.json"}};
    Expect(!runner::write_game_pack({}, escaping, temp.path / "bad.pak", error), "pack: refuses a drive");

    // A damaged archive is refused, and leaves no folder behind.
    std::string bytes = Read(game);
    bytes[static_cast<std::size_t>(where->offset)] = 'X';
    const fs::path damaged = temp.path / "Damaged.exe";
    Write(damaged, bytes);
    runner::PackLocation other = *where;
    other.hash ^= 1;
    fs::path broken;
    Expect(!runner::unpack_game(damaged, other, cache, broken, error), "unpack: refuses a damaged archive");
    Expect(!fs::exists(broken), "unpack: a damaged archive leaves no folder");

    // The name typed for the game gets the system's extension when it lacks it.
#if defined(_WIN32)
    Expect(ide::game_path("C:/games/Pong") == fs::path("C:/games/Pong.exe"), "export: adds .exe");
    Expect(ide::game_path("C:/games/Pong.EXE") == fs::path("C:/games/Pong.EXE"), "export: keeps .EXE");
    Expect(ide::game_file_name("My: Game") == "My_ Game.exe", "export: suggests a safe name");
#elif defined(__APPLE__)
    Expect(ide::game_path("/games/Pong") == fs::path("/games/Pong.app"), "export: adds .app");
#else
    Expect(ide::game_path("/games/Pong") == fs::path("/games/Pong"), "export: no extension on Linux");
#endif

    // Export packs the project's baked textures, so a player never bakes
    // what the studio already did.
    {
        TempFolder cached;
        const fs::path root = cached.path / "proj";
        Write(root / "project.json", "{\"format\": 1}");
        Write(root / "src" / "init.json", "{}");
        Write(root / "resources" / "textures" / "a.png", "png");
        Write(root / ".cache" / "textures" / "0123456789abcdef.atex", "ATEX");
        ide::GameExportRequest request;
        request.project_root = root;
        request.tree_root = root / "src";
        request.resources_root = root / "resources";
        std::vector<runner::PackFile> packed;
        std::string why;
        Expect(ide::collect_export_files(request, packed, why), "export: collects the project");
        bool hasCache = false;
        for (const runner::PackFile& file : packed) {
            hasCache = hasCache || file.path == "game/.cache/textures/0123456789abcdef.atex";
        }
        Expect(hasCache, "export: packs the baked textures beside the resources");
    }
    return gFailures - before;
}
