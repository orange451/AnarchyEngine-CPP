#include "ide/IdeLayout.hpp"
#include "runner/GameView.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "Gui.hpp"
#include "LuaApi.hpp"

#include "jadefx/jadefx.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

// An ImagePane in the Scene View's GUI layer: its node draws the Texture's
// file as a background image, faded by ImageTransparency, and follows the
// Image, the Texture's Path, and the file.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

// 4x4 opaque red PNG.
constexpr unsigned char kRedPng[] = {
    137, 80,  78, 71,  13, 10, 26,  10,  0,  0,  0,   13,  73,  72,  68,  82,  0,   0,   0,  4,  0, 0,  0,   4,
    8,   6,   0,  0,   0,  169, 241, 158, 126, 0, 0,   0,   18,  73,  68,  65,  84,  120, 218, 99, 248, 207, 192,
    240, 31,  25, 51,  144, 46,  0,   0,   60,  64, 31,  225, 26, 243, 165, 72,  0,   0,   0,  0,  73, 69,  78,
    68,  174, 66, 96,  130};

runner::GameView* FindGameView(jadefx::Scene& scene) {
    for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("ide-pane")) {
        if (auto* view = dynamic_cast<runner::GameView*>(node)) {
            return view;
        }
    }
    return nullptr;
}

engine_core::LuaSlot InstanceSlot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

}  // namespace

int RunGuiImageTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    double time = scene.timeSeconds() + 0.01;
    auto frame = [&] {
        for (int i = 0; i < 2; ++i) {
            scene.layout(1280, 800, time);
            time += 0.02;
        }
    };
    frame();
    runner::GameView* view = FindGameView(scene);
    Expect(view != nullptr, "the studio has a Scene View");
    if (view == nullptr) {
        return gFailures;
    }

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "anarchy-gui-image-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "ui");
    {
        std::ofstream out(root / "ui" / "red.png", std::ios::binary);
        out.write(reinterpret_cast<const char*>(kRedPng), sizeof kRedPng);
    }

    engine_core::Engine& engine = layout.simulation();
    std::filesystem::path previousRoot;
    engine_core::InstanceId screen = 0;
    engine_core::InstanceId pane = 0;
    engine_core::InstanceId texture = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        previousRoot = game.resources_root();
        game.set_resources_root(root);
        screen = engine_core::lua_create_instance(game, "ScreenGui")->id();
        game.set_parent(screen, game.scene_service("Gui"));
        pane = engine_core::lua_create_instance(game, "ImagePane")->id();
        game.set_parent(pane, screen);
        texture = engine_core::lua_create_instance(game, "Texture")->id();
        game.set_parent(texture, game.service("Textures"));
        dynamic_cast<engine_core::Texture*>(game.instance(texture))->set_path("ui/red.png");
    });
    auto onPane = [&](auto&& change) {
        engine.on_simulation([&](engine_core::DataModel& game) {
            if (auto* image = dynamic_cast<engine_core::ImagePane*>(game.instance(pane))) {
                change(*image);
            }
        });
    };
    frame();
    jadefx::Node* node = view->guiLayer().nodeFor(pane);
    Expect(node != nullptr, "an ImagePane in a ScreenGui is drawn");
    if (node == nullptr) {
        return gFailures;
    }
    Expect(std::string(node->getElementType()) == "imagepane", "its element type is imagepane");
    Expect(node->getBackgroundImage() == nullptr, "with no Image it draws none");

    onPane([&](engine_core::ImagePane& image) { image.set_image(InstanceSlot(texture)); });
    frame();
    const std::shared_ptr<jadefx::Image> drawn = node->getBackgroundImage();
    Expect(drawn != nullptr && drawn->getWidth() == 4 && drawn->getHeight() == 4,
           "its Image's file is the node's background image");
    Expect(node->getBackgroundImageOpacity() == 1.f, "drawn opaque at ImageTransparency 0");

    onPane([&](engine_core::ImagePane& image) {
        engine_core::LuaSlot value;
        value.kind = engine_core::LuaSlot::Kind::Number;
        value.number = 0.25;
        image.set_value(engine_core::GuiProperty::ImageTransparency, value);
    });
    frame();
    Expect(std::abs(node->getBackgroundImageOpacity() - 0.75f) < 1e-4f, "ImageTransparency 0.25 draws it at 0.75");
    Expect(node->getBackgroundImage() == drawn, "the same file is not loaded again");

    engine.on_simulation([&](engine_core::DataModel& game) {
        dynamic_cast<engine_core::Texture*>(game.instance(texture))->set_path("ui/missing.png");
    });
    frame();
    Expect(node->getBackgroundImage() == nullptr, "a Texture whose Path names no file draws nothing");

    engine.on_simulation([&](engine_core::DataModel& game) {
        dynamic_cast<engine_core::Texture*>(game.instance(texture))->set_path("ui/red.png");
    });
    frame();
    Expect(node->getBackgroundImage() != nullptr, "setting the Path back draws the file again");

    onPane([&](engine_core::ImagePane& image) { image.set_image(engine_core::LuaSlot{}); });
    frame();
    Expect(node->getBackgroundImage() == nullptr, "clearing Image draws none");

    engine.on_simulation([&](engine_core::DataModel& game) {
        game.destroy_tree(screen);
        game.destroy_tree(texture);
        game.set_resources_root(previousRoot);
    });
    frame();
    std::filesystem::remove_all(root);
    if (gFailures == 0) {
        std::printf("gui image tests passed\n");
    }
    return gFailures;
}
