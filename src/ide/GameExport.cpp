#include "GameExport.hpp"

#include "texture/TextureBake.hpp"

#include "IdeResources.hpp"
#include "Project.hpp"
#include "runner/GamePack.hpp"

#if defined(__APPLE__)
#include "RunProcess.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <system_error>
#include <vector>

namespace ide {
namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
constexpr const char* kPlayerName = "AnarchyPlayer.exe";
constexpr const char* kGameExtension = ".exe";
#elif defined(__APPLE__)
constexpr const char* kPlayerName = "AnarchyPlayer.app";
constexpr const char* kGameExtension = ".app";
#else
constexpr const char* kPlayerName = "AnarchyPlayer";
constexpr const char* kGameExtension = "";
#endif

// The archive path of a folder inside the project, such as "game/src/".
bool ProjectFolder(const fs::path& folder, const fs::path& root, std::string& prefix, std::string& error) {
    const fs::path relative = folder.lexically_relative(root);
    const std::string text = relative.generic_u8string();
    if (relative.empty() || text == "." || text.rfind("..", 0) == 0) {
        error = utf8_path(folder) + " is not inside the project folder";
        return false;
    }
    prefix = std::string(runner::kPackedProject) + "/" + text + "/";
    return true;
}

// The shaders a game draws with: the renderer's, under pipeline/, and JadeFX's
// for its GUIs, which the build copies in beside them. The copy beside the
// studio program comes first: resources/ in the source tree, which a studio
// started from there would find first, has only the renderer's.
fs::path ShaderFolder() {
    const fs::path exeDir = executable_directory();
    std::vector<fs::path> candidates;
    if (!exeDir.empty()) {
#if defined(__APPLE__)
        candidates.push_back(exeDir / ".." / "Resources" / "shaders");
#endif
        candidates.push_back(exeDir / "resources" / "shaders");
    }
    candidates.push_back(find_resource_folder("shaders"));
    for (const fs::path& candidate : candidates) {
        std::error_code failure;
        if (!candidate.empty() && fs::is_directory(candidate / "pipeline", failure) &&
            fs::is_regular_file(candidate / "text.frag", failure)) {
            return candidate;
        }
    }
    return {};
}

}  // namespace

bool collect_export_files(const GameExportRequest& request, std::vector<runner::PackFile>& files, std::string& error) {
    files.push_back({std::string(runner::kPackedProject) + "/project.json", request.project_root / "project.json"});
    std::string prefix;
    if (!ProjectFolder(request.tree_root, request.project_root, prefix, error) ||
        !runner::add_pack_folder(request.tree_root, prefix, files, error)) {
        return false;
    }
    // A project that never imported anything may have no resources folder.
    std::error_code failure;
    if (fs::is_directory(request.resources_root, failure)) {
        if (!ProjectFolder(request.resources_root, request.project_root, prefix, error) ||
            !runner::add_pack_folder(request.resources_root, prefix, files, error)) {
            return false;
        }
    }
    // The textures the studio already baked, where the player looks for
    // them (beside the resources folder), so it never bakes them again.
    const fs::path baked = engine_core::texture::cache_path(request.resources_root, "x").parent_path();
    if (fs::is_directory(baked, failure)) {
        if (!ProjectFolder(baked, request.project_root, prefix, error) ||
            !runner::add_pack_folder(baked, prefix, files, error)) {
            return false;
        }
    }
    const fs::path shaders = ShaderFolder();
    if (shaders.empty()) {
        error = "the engine's shaders were not found beside the studio";
        return false;
    }
    return runner::add_pack_folder(shaders, std::string(runner::kPackedResources) + "/shaders/", files, error);
}

namespace {

#if defined(__APPLE__)
std::string EscapeXml(const std::string& text) {
    std::string escaped;
    for (const char c : text) {
        switch (c) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            default: escaped += c;
        }
    }
    return escaped;
}

// Letters, digits, '-' and '.', as a bundle identifier takes.
std::string IdentifierPart(const std::string& name) {
    std::string part;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        part += plain ? c : '-';
    }
    return part.empty() ? std::string("game") : part;
}

void ReplaceAll(std::string& text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

// The player bundle, copied to app and named for the game, with the archive
// in its Resources and signed again so Finder opens it.
bool WriteBundle(const fs::path& player, const std::vector<runner::PackFile>& files, const std::string& name,
                 const fs::path& app, std::string& error) {
    std::error_code failure;
    fs::copy(player, app, fs::copy_options::recursive | fs::copy_options::copy_symlinks, failure);
    if (failure) {
        error = "cannot copy the player to " + utf8_path(app) + ": " + failure.message();
        return false;
    }
    const fs::path contents = app / "Contents";
    if (!runner::write_game_pack({}, files, contents / "Resources" / runner::kBundlePack, error)) {
        return false;
    }
    // The player's Info.plist names it "Anarchy Player"; the game takes its own name.
    const fs::path plist = contents / "Info.plist";
    std::string text;
    if (!read_file(plist, text, error)) {
        return false;
    }
    ReplaceAll(text, "<string>Anarchy Player</string>", "<string>" + EscapeXml(name) + "</string>");
    ReplaceAll(text, "local.anarchyengine.player", "local.anarchyengine.game." + IdentifierPart(name));
    if (!write_file(plist, text, error)) {
        return false;
    }
    const ProcessResult signed_ = run_process("/usr/bin/codesign", {"--force", "--deep", "--sign", "-", utf8_path(app)},
                                              std::chrono::minutes(2));
    if (!signed_.started || signed_.timed_out || signed_.exit_code != 0) {
        error = "codesign could not sign " + utf8_path(app) + ": " +
                (signed_.started ? signed_.output : signed_.error);
        return false;
    }
    return true;
}
#endif

}  // namespace

std::string game_file_name(const std::string& project_name) {
    return engine_core::sanitize_file_name(project_name) + kGameExtension;
}

fs::path game_path(const fs::path& path) {
    const std::string extension = kGameExtension;
    if (extension.empty()) {
        return path;
    }
    std::string name = utf8_path(path.filename());
    const bool has = name.size() >= extension.size() &&
                     std::equal(extension.rbegin(), extension.rend(), name.rbegin(), [](char a, char b) {
                         return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                     });
    if (has) {
        return path;
    }
    fs::path with = path;
    with += fs::u8path(extension);
    return with;
}

fs::path find_player() {
    const fs::path exeDir = executable_directory();
    if (exeDir.empty()) {
        return {};
    }
    std::vector<fs::path> candidates;
#if defined(__APPLE__)
    // In the studio bundle, then beside it, as a build folder has them.
    candidates.push_back(exeDir / ".." / "Resources" / kPlayerName);
    candidates.push_back(exeDir / ".." / ".." / ".." / kPlayerName);
#endif
    candidates.push_back(exeDir / kPlayerName);
    for (const fs::path& candidate : candidates) {
        std::error_code failure;
        if (fs::exists(candidate, failure)) {
            return candidate.lexically_normal();
        }
    }
    return {};
}

bool export_game(const GameExportRequest& request, fs::path& written, std::string& error) {
    const fs::path player = find_player();
    if (player.empty()) {
        error = std::string(kPlayerName) + " was not found beside the studio. Build it with the studio.";
        return false;
    }
    std::vector<runner::PackFile> files;
    if (!collect_export_files(request, files, error)) {
        return false;
    }
    const fs::path output = game_path(request.output);
    if (output.filename().empty()) {
        error = "no file name was given for the game";
        return false;
    }
    std::error_code failure;
    fs::create_directories(output.parent_path(), failure);
#if defined(__APPLE__)
    // Only an earlier export is replaced: a bundle with a pack in it.
    if (fs::exists(output, failure)) {
        if (!fs::is_regular_file(output / "Contents" / "Resources" / runner::kBundlePack, failure)) {
            error = utf8_path(output) + " is already there, and is not an exported game";
            return false;
        }
        fs::remove_all(output, failure);
        if (failure) {
            error = "cannot replace " + utf8_path(output) + ": " + failure.message();
            return false;
        }
    }
    if (!WriteBundle(player, files, request.name, output, error)) {
        fs::remove_all(output, failure);
        return false;
    }
#else
    if (fs::is_directory(output, failure)) {
        error = utf8_path(output) + " is a folder";
        return false;
    }
    if (!runner::write_game_pack(player, files, output, error)) {
        return false;
    }
#endif
    written = output;
    return true;
}

}  // namespace ide
