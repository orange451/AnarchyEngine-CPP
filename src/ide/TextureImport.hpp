#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ide {

// Whether path, in UTF-8, names an image file by its extension: PNG, JPEG,
// TGA, BMP, GIF, HDR, PSD, DDS, KTX, WebP, EXR, or TIFF, in any case.
bool is_texture_file(const std::string& path);
// The extensions is_texture_file takes, without the dot, for a file dialog.
const std::vector<std::string>& texture_file_extensions();

// file as a Path relative to root, when it is inside root and a Path can name
// it. Empty otherwise.
std::optional<std::string> resource_path_inside(const std::filesystem::path& root, const std::filesystem::path& file);

// Writes bytes into resources_root/folder, a Path such as "textures", as
// file_name, or name-2, name-3, and so on when another file has it, and
// returns its Path. A file there with the same bytes is used instead of a
// second copy. file_name is made safe for any file system first. Empty, with
// error set, when it cannot be written.
std::optional<std::string> store_resource_file(const std::filesystem::path& resources_root, const std::string& folder,
                                               const std::string& file_name, const std::string& bytes,
                                               std::string& error);

// Puts source, a UTF-8 path, where a Texture's Path can name it, and returns
// that Path. A file already under resources_root is used where it is. Any
// other is copied into resources_root/folder, as store_resource_file does.
// Empty, with error set, when it cannot be read or written.
std::optional<std::string> import_texture_file(const std::filesystem::path& resources_root, const std::string& source,
                                               std::string& error, const std::string& folder = "textures");

}  // namespace ide
