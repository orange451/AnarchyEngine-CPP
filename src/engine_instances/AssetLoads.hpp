#pragma once

// AssetLoads: which asset files are showing yet, as the loaders that draw
// them report it, for the read-only Loaded property of Texture and Mesh.
// A loader (TextureCache, MeshCache) marks a path loaded the first time it
// shows anything of it (a texture's blurry first levels count) and unloaded
// when its file goes missing or will not read. Keyed by resources folder and
// Path, so two projects never share an entry. Every call is safe on any thread.

#include <filesystem>
#include <string>

namespace engine_core {

enum class AssetKind { Texture, Mesh };

void set_asset_loaded(AssetKind kind, const std::filesystem::path& root, const std::string& path, bool loaded);
bool asset_loaded(AssetKind kind, const std::filesystem::path& root, const std::string& path);
// Forgets every entry. Tests.
void clear_asset_loads();

}  // namespace engine_core
