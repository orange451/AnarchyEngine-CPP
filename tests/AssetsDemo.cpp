#include "ide/CutSet.hpp"
#include "ide/IdeAssets.hpp"
#include "ide/IdeTheme.hpp"
#include "runner/ViewCapture.hpp"
#include "runner/gl.hpp"

#include "AssetInstances.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "LuaApi.hpp"
#include "SelectionService.hpp"

#include "jadefx/jadefx.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

// The Assets pane alone in a window, over a small sample place. It shows Textures
// > Bricks with BrickRed selected in each view, and saves each as a PNG: a look
// by hand at what the pane draws.
//   assets-demo <out-dir> <theme>
// writes <out-dir>/<theme>-icons.png, and the other views' as they exist.
namespace {

using engine_core::InstanceId;

constexpr int kWidth = 900;
constexpr int kHeight = 420;
// Frames each view gets to lay out before it is saved.
constexpr int kSettleFrames = 3;
// The views the pane draws so far, saved in this order.
constexpr ide::AssetView kViews[] = {ide::AssetView::Icons};

template <typename T>
InstanceId Add(engine_core::Game& game, const char* name, InstanceId parent) {
    T& object = game.create<T>();
    game.set_name(object.id(), name);
    game.set_parent(object.id(), parent);
    return object.id();
}

void Refer(engine_core::Game& game, InstanceId from, std::size_t index, InstanceId to) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = to;
    if (auto* asset = dynamic_cast<engine_core::ReferenceAsset*>(game.instance(from))) {
        asset->set_reference(index, slot);
    }
}

class AssetsDemo : public jadefx::Application {
public:
    void start(jadefx::Stage& stage, int argc, char** argv) override {
        out_dir_ = argc > 1 ? argv[1] : ".";
        theme_ = argc > 2 ? argv[2] : "light";
        ide::set_current_theme(ide::shipped_theme(theme_));
        engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
        fill();

        ide::AssetsHost host;
        host.actions.run = [](engine_core::InstanceAction, InstanceId) {};
        host.actions.run_many = [](engine_core::InstanceAction, const std::vector<InstanceId>&) {};
        host.actions.enabled = [](engine_core::InstanceAction) { return true; };
        host.actions.notice = [](std::string text) { std::printf("notice: %s\n", text.c_str()); };
        host.actions.insert = [this](std::string class_name, InstanceId parent,
                                     std::shared_ptr<ide::InsertResult> result) {
            std::string error;
            result->id = ide::insert_instance(game_, class_name, parent, error);
            result->error = error;
            result->done = true;
        };
        host.actions.rename = [this](InstanceId id, std::string name) { game_.set_name(id, name); };
        host.actions.move = [this](const std::vector<InstanceId>& ids, InstanceId parent) {
            ide::move_set(game_, ids, parent);
        };
        host.saved_view = [] { return std::string("icons"); };
        host.save_view = [](const std::string&) {};
        auto pane = jadefx::make<ide::IdeAssets>(game_, std::move(host));
        pane->setPrefWidthRatio(1);
        pane->setPrefHeightRatio(1);
        pane_ = pane.get();
        pane_->openFolder(game_.service("Textures"));
        pane_->openFolder(bricks_);
        game_.selection().set({brick_red_});
        stage.setScene(jadefx::make<jadefx::Scene>(pane, kWidth, kHeight));
        stage_ = &stage;
        stage.setRenderingCallback([this](int width, int height) { frame(width, height); });
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {kWidth, kHeight}; }
    std::string defaultTitle() const override { return "Assets demo"; }

private:
    void fill() {
        const InstanceId textures = game_.service("Textures");
        bricks_ = Add<engine_core::Folder>(game_, "Bricks", textures);
        brick_red_ = Add<engine_core::Texture>(game_, "BrickRed", bricks_);
        dynamic_cast<engine_core::Texture*>(game_.instance(brick_red_))->set_path("textures/brick_red.png");
        const InstanceId brick_normal = Add<engine_core::Texture>(game_, "BrickRed_N", bricks_);
        Add<engine_core::Texture>(game_, "Moss", textures);
        Add<engine_core::Texture>(game_, "Moss_R", textures);
        const InstanceId wall = Add<engine_core::Material>(game_, "Wall", game_.service("Materials"));
        Refer(game_, wall, 0, brick_red_);
        Refer(game_, wall, 1, brick_normal);
        const InstanceId rock = Add<engine_core::Mesh>(game_, "Rock", game_.service("Meshes"));
        const InstanceId crate = Add<engine_core::Prefab>(game_, "Crate", game_.service("Prefabs"));
        const InstanceId body = Add<engine_core::Model>(game_, "Body", crate);
        Refer(game_, body, 0, rock);
        Refer(game_, body, 1, wall);
        Add<engine_core::Sound>(game_, "Boom", game_.service("Audio"));
    }

    void frame(int width, int height) {
        if (view_index_ >= std::size(kViews)) {
            return;
        }
        if (settled_ == 0) {
            pane_->setView(kViews[view_index_]);
        }
        if (++settled_ < kSettleFrames) {
            return;
        }
        save(width, height, out_dir_ + "/" + theme_ + "-" + ide::asset_view_name(kViews[view_index_]) + ".png");
        settled_ = 0;
        if (++view_index_ >= std::size(kViews)) {
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
    ide::IdeAssets* pane_ = nullptr;
    jadefx::Stage* stage_ = nullptr;
    std::string out_dir_;
    std::string theme_;
    InstanceId bricks_ = 0;
    InstanceId brick_red_ = 0;
    std::size_t view_index_ = 0;
    int settled_ = 0;
};

}  // namespace

int main(int argc, char** argv) { return jadefx::Application::launch(std::make_unique<AssetsDemo>(), argc, argv); }
