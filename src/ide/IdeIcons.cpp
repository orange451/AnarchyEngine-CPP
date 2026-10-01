#include "IdeIcons.hpp"

#include "IdeResources.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <string>
#include <unordered_map>

namespace ide {
namespace {

// ClassName.png when that file exists. These classes have no file of that name.
const char* IconFileOverride(const std::string& class_name) {
    if (class_name == "Game" || class_name == "Workspace") {
        return "World.png";
    }
    if (class_name == "Lighting") {
        return "Light.png";
    }
    if (class_name == "Scripts") {
        return "ScriptService.png";
    }
    if (class_name == "Assets" || class_name == "Audio") {
        return "AssetFolder.png";
    }
    if (class_name == "Materials") {
        return "AssetFolderMaterial.png";
    }
    if (class_name == "Meshes") {
        return "AssetFolderMesh.png";
    }
    if (class_name == "Prefabs") {
        return "AssetFolderPrefab.png";
    }
    if (class_name == "Textures") {
        return "AssetFolderTexture.png";
    }
    if (class_name == "Prefab") {
        return "ModelAlt.png";
    }
    if (class_name == "PointLight") {
        return "Light.png";
    }
    if (class_name == "SpotLight") {
        return "LightSpot.png";
    }
    if (class_name == "PhysicsObject") {
        return "Box.png";
    }
    return nullptr;
}

std::shared_ptr<jadefx::Image> IconImage(const std::string& filename) {
    static std::unordered_map<std::string, std::shared_ptr<jadefx::Image>> cache;
    const auto found = cache.find(filename);
    if (found != cache.end()) {
        return found->second;
    }
    std::shared_ptr<jadefx::Image> image;
    const std::filesystem::path path = find_resource("icons/" + filename);
    if (!path.empty()) {
        image = jadefx::Image::load(utf8_path(path));
        if (!image) {
            std::fprintf(stderr, "Could not decode icon %s\n", path.string().c_str());
        }
    }
    cache.emplace(filename, image);
    return image;
}

}  // namespace

std::shared_ptr<jadefx::ImageView> icon_view(const std::string& class_name) {
    if (class_name.empty()) {
        return nullptr;
    }
    const char* aliased = IconFileOverride(class_name);
    const std::string filename = aliased != nullptr ? aliased : class_name + ".png";
    const std::shared_ptr<jadefx::ImageView> icon = icon_file(filename);
    if ( !icon ) {
        return icon_file("wat.gif");
    }
    
    return icon;
}

std::shared_ptr<jadefx::ImageView> icon_file(const std::string& filename) {
    if (filename.empty()) {
        return nullptr;
    }
    const std::shared_ptr<jadefx::Image> image = IconImage(filename);
    if (!image) {
        return nullptr;
    }
    return jadefx::make<jadefx::ImageView>(image);
}

std::shared_ptr<jadefx::ImageView> icon_graphic(const std::string& filename) {
    std::shared_ptr<jadefx::ImageView> view = icon_file(filename);
    if (!view) {
        return nullptr;
    }
    view->setMouseTransparent(true);
    view->setPrefSize(16, 16);
    view->setMinSize(16, 16);
    return view;
}

std::shared_ptr<jadefx::ImageView> drag_icon(std::shared_ptr<jadefx::Image> image) {
    if (!image) {
        return nullptr;
    }
    auto view = jadefx::make<jadefx::ImageView>(std::move(image));
    view->setElementId("instance-drag-icon");
    view->setPrefSize(kDragIconSize, kDragIconSize);
    view->setMinSize(kDragIconSize, kDragIconSize);
    view->setMaxSize(kDragIconSize, kDragIconSize);
    return view;
}

}  // namespace ide
