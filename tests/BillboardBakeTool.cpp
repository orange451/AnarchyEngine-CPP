#include "ide/BillboardBaker.hpp"
#include "ide/PrefabLods.hpp"
#include "runner/gl.hpp"

#include "AssetInstances.hpp"
#include "Game.hpp"
#include "Project.hpp"
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
    if (argc < 4) {
        std::fprintf(stderr, "usage: billboard-bake <project> <folder under Prefabs> <distance> [height]\n");
        return 2;
    }
    const std::filesystem::path root = std::filesystem::u8path(argv[1]);
    const std::string folder = argv[2];
    const double distance = std::atof(argv[3]);
    const int height = argc > 4 ? std::atoi(argv[4]) : 512;
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
            runner::ViewPixels card;
            bool done = false;
            for (int pass = 0; pass < 2000 && !done; ++pass) {
                done = baker.bake(visual, low, high, card);
                if (!done) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            if (card.empty()) {
                std::printf("%-28s no card\n", game.name(id).c_str());
                continue;
            }
            const std::string png = runner::EncodePngWithAlpha(card);
            const std::string path = ide::card_file(game.name(id), png);
            const std::filesystem::path file = resources / std::filesystem::u8path(path);
            std::filesystem::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary) << png;
            // Its Card layer, made the first time.
            InstanceId layer = game.find_first_child(id, "Card");
            if (dynamic_cast<const engine_core::LodLayer*>(game.instance(layer)) == nullptr) {
                std::string error;
                layer = ide::run_add_lod(game, id, error);
                game.set_name(layer, "Card");
            }
            ide::run_set_lod_kind(game, layer, engine_core::LodKind::Billboard);
            ide::run_set_lod_distance(game, layer, distance);
            const float card_height = ide::card_frame(low, high).height;
            const float card_width = card_height * static_cast<float>(card.width) / static_cast<float>(card.height);
            if (std::optional<std::string> error = ide::run_set_lod_card(game, layer, path, card_width, card_height)) {
                std::printf("%-28s %s\n", game.name(id).c_str(), error->c_str());
                continue;
            }
            ++baked;
            std::printf("%-28s %4d x %-4d %6.2f x %6.2f units  %s\n", game.name(id).c_str(), card.width, card.height,
                        card_width, card_height, path.c_str());
        }
        baker.release();
    }
    project.save();
    std::printf("baked %d of %zu Prefabs, saved\n", baked, prefabs.size());
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
