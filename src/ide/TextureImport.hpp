#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace ide {

// Whether path, in UTF-8, names an image file by its extension: PNG, JPEG,
// TGA, BMP, GIF, HDR, PSD, DDS, KTX, WebP, EXR, or TIFF, in any case.
bool is_texture_file(const std::string& path);

// Puts source, a UTF-8 path, where a Texture's Path can name it, and returns
// that Path. A file already under resources_root is used where it is. Any
// other is copied into resources_root/textures under its own name, or
// name-2, name-3, and so on when another file has it; a file there with the
// same bytes is used instead of a second copy. Empty, with error set, when
// it cannot be read or written.
std::optional<std::string> import_texture_file(const std::filesystem::path& resources_root, const std::string& source,
                                               std::string& error);

}  // namespace ide
