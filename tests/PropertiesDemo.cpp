#include "ide/IdeTheme.hpp"
#include "ide/PropertiesPanel.hpp"
#include "runner/ViewCapture.hpp"
#include "runner/gl.hpp"

#include "ChangeHistoryService.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "SelectionService.hpp"

#include "jadefx/jadefx.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The Properties panel alone in a window, over a part with attributes, saved
// as PNGs: a look by hand at what it draws.
//   properties-demo <out-dir> <theme> [width height]
// writes <out-dir>/<theme>-attributes.png and -adding.png.
namespace {

using engine_core::InstanceId;
using engine_core::LuaSlot;

// Past the panel's fades.
constexpr int kSettleFrames = 24;

class PropertiesDemo : public jadefx::Application {
public:
    void start(jadefx::Stage& stage, int argc, char** argv) override {
        out_dir_ = argc > 1 ? argv[1] : ".";
        theme_ = argc > 2 ? argv[2] : "light";
        width_ = argc > 4 ? std::atoi(argv[3]) : 420;
        height_ = argc > 4 ? std::atoi(argv[4]) : 760;
        ide::set_current_theme(ide::shipped_theme(theme_));
        engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
        part_ = fill();

        panel_.bind(game_, game_.selection(), game_.history());
        auto root = jadefx::make<jadefx::StackPane>();
        root->setPrefWidthRatio(1);
        root->setPrefHeightRatio(1);
        root->getChildren().add(panel_.dock_widget());
        auto scene = jadefx::make<jadefx::Scene>(root, width_, height_);
        scene_ = scene.get();
        stage.setScene(scene);
        stage_ = &stage;
        game_.selection().set({part_});
        stage.setRenderingCallback([this](int width, int height) { frame(width, height); });
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {static_cast<double>(width_), static_cast<double>(height_)}; }
    std::string defaultTitle() const override { return "Properties demo"; }

private:
    InstanceId fill() {
        engine_core::GameObject& part = game_.create_game_object();
        game_.set_name(part.id(), "Crate");
        game_.set_parent(part.id(), game_.scene_service("Workspace"));
        LuaSlot slot;
        slot.kind = LuaSlot::Kind::Number;
        slot.number = 100;
        part.set_attribute("Health", slot);
        slot.number = 16;
        part.set_attribute("Speed", slot);
        LuaSlot team;
        team.kind = LuaSlot::Kind::String;
        team.text = "Red";
        part.set_attribute("Team", team);
        LuaSlot open;
        open.kind = LuaSlot::Kind::Bool;
        open.flag = true;
        part.set_attribute("Lootable", open);
        LuaSlot tint;
        tint.kind = LuaSlot::Kind::Color;
        tint.color = {0.9f, 0.3f, 0.2f, 1.f};
        part.set_attribute("Glow", tint);
        LuaSlot spawn;
        spawn.kind = LuaSlot::Kind::Vec3;
        spawn.vec = {12.f, 4.f, -8.f};
        part.set_attribute("Spawn", spawn);
        game_.history().reset_waypoints();
        return part.id();
    }

    struct Shot {
        std::string suffix;
        std::function<void()> act;
    };

    std::vector<Shot> shots() {
        return {
            {"attributes", nullptr},
            {"adding",
             [this] {
                 if (auto* plus = dynamic_cast<jadefx::Button*>(panel_.attribute_plus())) {
                     plus->fire();
                 }
             }},
        };
    }

    void frame(int width, int height) {
        const std::vector<Shot> all = shots();
        if (shot_ >= all.size()) {
            return;
        }
        const Shot& shot = all[shot_];
        if (settled_ == 2 && shot.act) {
            shot.act();
        }
        if (++settled_ < kSettleFrames) {
            return;
        }
        save(width, height, out_dir_ + "/" + theme_ + "-" + shot.suffix + ".png");
        settled_ = 0;
        if (++shot_ >= all.size()) {
            stage_->close();
        }
    }

    void save(int width, int height, const std::string& png) {
        runner::LoadGl([](const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); });
        runner::ViewPixels pixels;
        pixels.width = width;
        pixels.height = height;
        std::vector<unsigned char> bottom_up(static_cast<std::size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, bottom_up.data());
        pixels.rgba.resize(bottom_up.size());
        const std::size_t row = static_cast<std::size_t>(width) * 4;
        for (int y = 0; y < height; ++y) {
            std::copy_n(bottom_up.data() + row * static_cast<std::size_t>(height - 1 - y), row,
                        pixels.rgba.data() + row * static_cast<std::size_t>(y));
        }
        std::ofstream(png, std::ios::binary) << runner::EncodePng(pixels);
        std::printf("saved %s (%dx%d)\n", png.c_str(), width, height);
    }

    engine_core::Game game_;
    ide::PropertiesPanel panel_;
    InstanceId part_ = 0;
    jadefx::Scene* scene_ = nullptr;
    jadefx::Stage* stage_ = nullptr;
    std::string out_dir_;
    std::string theme_;
    int width_ = 420;
    int height_ = 760;
    std::size_t shot_ = 0;
    int settled_ = 0;
};

}  // namespace

int main(int argc, char** argv) { return jadefx::Application::launch(std::make_unique<PropertiesDemo>(), argc, argv); }
