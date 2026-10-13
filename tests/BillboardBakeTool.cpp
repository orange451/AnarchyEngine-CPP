#include "ide/BillboardBaker.hpp"
#include "ide/PrefabLods.hpp"
#include "runner/gl.hpp"

#include "AssetInstances.hpp"
#include "Game.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// Bakes a billboard card for every Prefab under a folder of a project's
// Prefabs, as the Prefab editor's Bake card does, gives each a Billboard
// LodLayer named Card from distance on, and saves the project. A look by hand
// at many cards at once. It writes to the project: run it on a copy.
//   billboard-bake <project> <folder under Prefabs, such as Vegetation/Trees> <distance> [height]
// With --files it writes only the card images, under the project's
// resources where a studio's disk checks do not look, and a JSON list of
// what each Prefab's card Material needs, for a studio with the project
// open to make the instances itself. The project's own files stay as they are.
//   billboard-bake <project> <folder under Prefabs> --files <out.json> [height]
namespace {

using engine_core::InstanceId;

// Every Prefab under id, depth first.
void collect(const engine_core::DataModel& game, InstanceId id, std::vector<InstanceId>& out) {
    for (InstanceId child = game.first_child(id); child != 0; child = game.next_sibling(child)) {
        if (dynamic_cast<const engine_core::Prefab*>(game.instance(child)) != nullptr) {
            out.push_back(child);
        } else {
            collect(game, child, out);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool files_only = argc > 3 && std::string(argv[3]) == "--files";
    if (argc < 4 || (files_only && argc < 5)) {
        std::fprintf(stderr,
                     "usage: billboard-bake <project> <folder under Prefabs> <distance> [height]\n"
                     "       billboard-bake <project> <folder under Prefabs> --files <out.json> [height]\n");
        return 2;
    }
    const std::filesystem::path root = std::filesystem::u8path(argv[1]);
    const std::string folder = argv[2];
    const double distance = files_only ? 0.0 : std::atof(argv[3]);
    const std::filesystem::path files_out = files_only ? std::filesystem::u8path(argv[4]) : std::filesystem::path();
    const int at_height = files_only ? 5 : 4;
    const int height = argc > at_height ? std::atoi(argv[at_height]) : 512;
    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::load(root, game);

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    GLFWwindow* window = glfwCreateWindow(64, 64, "billboard-bake", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "no GL 3.3 core context\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    if (!runner::LoadGl([](const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); })) {
        return 1;
    }

    // The folder, a name at a time from Prefabs.
    InstanceId at = game.service("Prefabs");
    for (std::size_t begin = 0; at != 0 && begin <= folder.size();) {
        const std::size_t end = std::min(folder.find('/', begin), folder.size());
        if (end > begin) {
            at = game.find_first_child(at, folder.substr(begin, end - begin));
        }
        begin = end + 1;
    }
    if (at == 0) {
        std::fprintf(stderr, "no folder Prefabs/%s\n", folder.c_str());
        return 1;
    }
    std::vector<InstanceId> prefabs;
    collect(game, at, prefabs);
    const std::filesystem::path resources = game.resources_root();
    int baked = 0;
    std::vector<engine_core::JsonValue> listed;
    {
        ide::BillboardBaker baker(height);
        baker.setRoot(resources);
        for (const InstanceId id : prefabs) {
            const auto* prefab = dynamic_cast<const engine_core::Prefab*>(game.instance(id));
            engine_core::Vec3 low{};
            engine_core::Vec3 high{};
            if (prefab == nullptr || !prefab->bounds(low, high)) {
                continue;
            }
            engine_core::VisualPrefab visual;
            InstanceId cached = id;
            engine_core::fill_visual_prefab(game, game.guid(id), cached, visual);
            ide::CardMaps card;
            bool done = false;
            for (int pass = 0; pass < 2000 && !done; ++pass) {
                done = baker.bake(visual, low, high, card);
                if (!done) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            if (card.color.empty()) {
                std::printf("%-28s no card\n", game.name(id).c_str());
                continue;
            }
            ide::CardFiles files;
            std::string why;
            if (!ide::save_card(card, resources, game.name(id), files, why)) {
                std::printf("%-28s %s\n", game.name(id).c_str(), why.c_str());
                continue;
            }
            files.height = ide::card_frame(low, high).height;
            files.width = files.height * static_cast<float>(card.color.width) / static_cast<float>(card.color.height);
            if (files_only) {
                using engine_core::JsonValue;
                JsonValue row = JsonValue::object();
                row.set("guid", JsonValue::string(game.guid(id)));
                row.set("name", JsonValue::string(game.name(id)));
                row.set("color", JsonValue::string(files.color));
                row.set("normal", JsonValue::string(files.normal));
                row.set("roughness_map", JsonValue::string(files.roughness_map));
                row.set("metalness_map", JsonValue::string(files.metalness_map));
                row.set("emissive", JsonValue::string(files.emissive));
                row.set("roughness", JsonValue::number_from_float(files.roughness));
                row.set("metalness", JsonValue::number_from_float(files.metalness));
                row.set("reflectivity", JsonValue::number_from_float(files.reflectivity));
                row.set("width", JsonValue::number_from_float(files.width));
                row.set("height", JsonValue::number_from_float(files.height));
                listed.push_back(std::move(row));
                ++baked;
                std::printf("%-28s %4d x %-4d %6.2f x %6.2f units\n", game.name(id).c_str(), card.color.width,
                            card.color.height, files.width, files.height);
                continue;
            }
            // Its Card layer, made the first time.
            InstanceId layer = game.find_first_child(id, "Card");
            if (dynamic_cast<const engine_core::LodLayer*>(game.instance(layer)) == nullptr) {
                std::string error;
                layer = ide::run_add_lod(game, id, error);
                game.set_name(layer, "Card");
            }
            ide::run_set_lod_kind(game, layer, engine_core::LodKind::Billboard);
            ide::run_set_lod_distance(game, layer, distance);
            if (std::optional<std::string> error = ide::run_set_lod_card(game, layer, files)) {
                std::printf("%-28s %s\n", game.name(id).c_str(), error->c_str());
                continue;
            }
            ++baked;
            std::printf("%-28s %4d x %-4d %6.2f x %6.2f units  maps: color%s%s%s%s  roughness %.2f metalness %.2f\n",
                        game.name(id).c_str(), card.color.width, card.color.height, files.width, files.height,
                        files.normal.empty() ? "" : " normal", files.roughness_map.empty() ? "" : " roughness",
                        files.metalness_map.empty() ? "" : " metalness", files.emissive.empty() ? "" : " glow",
                        files.roughness, files.metalness);
        }
        baker.release();
    }
    if (files_only) {
        std::ofstream(files_out, std::ios::binary) << engine_core::write_json(engine_core::JsonValue::array(listed));
        std::printf("baked %d of %zu Prefabs, card files only, listed in %s\n", baked, prefabs.size(),
                    files_out.u8string().c_str());
    } else {
        project.save();
        std::printf("baked %d of %zu Prefabs, saved\n", baked, prefabs.size());
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
