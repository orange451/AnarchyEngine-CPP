#include "ide/IdePrefabEditor.hpp"
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
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The Prefab editor alone in a window, over a sample Prefab, saved as PNGs: a
// look by hand at what it draws.
//   prefab-demo <out-dir> <theme>
// writes <out-dir>/<theme>-prefab.png, -selected.png, -picker.png, -search.png, and -empty.png.
namespace {

using engine_core::InstanceId;

constexpr int kWidth = 960;
constexpr int kHeight = 560;
constexpr int kSettleFrames = 5;

template <typename T>
InstanceId Add(engine_core::Game& game, const char* name, InstanceId parent) {
    T& object = game.create<T>();
    game.set_name(object.id(), name);
    game.set_parent(object.id(), parent);
    return object.id();
}

class PrefabDemo : public jadefx::Application {
public:
    void start(jadefx::Stage& stage, int argc, char** argv) override {
        out_dir_ = argc > 1 ? argv[1] : ".";
        theme_ = argc > 2 ? argv[2] : "light";
        ide::set_current_theme(ide::shipped_theme(theme_));
        engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
        fill();

        root_ = jadefx::make<jadefx::StackPane>();
        root_->setPrefWidthRatio(1);
        root_->setPrefHeightRatio(1);
        show(crate_);
        auto scene = jadefx::make<jadefx::Scene>(root_, kWidth, kHeight);
        scene_ = scene.get();
        stage.setScene(scene);
        stage_ = &stage;
        stage.setRenderingCallback([this](int width, int height) { frame(width, height); });
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {kWidth, kHeight}; }
    std::string defaultTitle() const override { return "Prefab editor demo"; }

private:
    ide::PrefabEditorHost host() {
        ide::PrefabEditorHost host;
        host.add_model = [this](InstanceId prefab, InstanceId mesh, InstanceId material,
                                std::shared_ptr<ide::InsertResult> result) {
            std::string error;
            result->id = ide::add_model(game_, prefab, mesh, material, error);
            result->error = error;
            result->done = true;
        };
        host.set_part = [this](InstanceId model, ide::ModelPart part, InstanceId target) {
            ide::set_model_part(game_, model, part, target);
        };
        host.rename = [this](InstanceId id, std::string name) { game_.set_name(id, std::move(name)); };
        host.remove = [this](const std::vector<InstanceId>& ids) {
            for (InstanceId id : ids) {
                game_.destroy_tree(id);
            }
        };
        host.notice = [](std::string text) { std::printf("notice: %s\n", text.c_str()); };
        return host;
    }

    void show(InstanceId prefab) {
        root_->getChildren().clear();
        auto editor = jadefx::make<ide::IdePrefabEditor>(game_, prefab, host());
        ide::Fill(*editor);
        editor_ = editor.get();
        root_->getChildren().add(editor);
    }

    void fill() {
        const InstanceId meshes = game_.service("Meshes");
        const InstanceId props = Add<engine_core::Folder>(game_, "Props", meshes);
        rock_ = Add<engine_core::Mesh>(game_, "Rock", meshes);
        const InstanceId body_mesh = Add<engine_core::Mesh>(game_, "CrateBody", props);
        lid_mesh_ = Add<engine_core::Mesh>(game_, "CrateLid", props);
        Add<engine_core::Mesh>(game_, "Handle", props);
        Add<engine_core::Mesh>(game_, "Hinge", props);
        const InstanceId materials = game_.service("Materials");
        wood_ = Add<engine_core::Material>(game_, "Wood", materials);
        Add<engine_core::Material>(game_, "Metal", materials);
        Add<engine_core::Material>(game_, "Wall", materials);

        crate_ = Add<engine_core::Prefab>(game_, "Crate", game_.service("Prefabs"));
        std::string error;
        body_ = ide::add_model(game_, crate_, body_mesh, wood_, error);
        lid_ = ide::add_model(game_, crate_, lid_mesh_, 0, error);
        handle_ = ide::add_model(game_, crate_, 0, 0, error);
        empty_ = Add<engine_core::Prefab>(game_, "Barrel", game_.service("Prefabs"));
    }

    struct Shot {
        std::string suffix;
        InstanceId prefab;
        std::vector<InstanceId> selected;
        std::function<void()> act;
    };

    std::vector<Shot> shots() {
        return {
            {"prefab", crate_, {}, nullptr},
            {"selected", crate_, {lid_}, nullptr},
            {"picker", crate_, {handle_}, [this] { editor_->openPicker(handle_, ide::ModelPart::Mesh); }},
            {"search", crate_, {handle_},
             [this] {
                 editor_->openPicker(handle_, ide::ModelPart::Mesh);
                 if (jadefx::TextField* field = editor_->pickerField()) {
                     field->setText("crate");
                 }
             }},
            {"material", crate_, {body_}, [this] { editor_->openPicker(body_, ide::ModelPart::Material); }},
            {"empty", empty_, {}, nullptr},
        };
    }

    void frame(int width, int height) {
        const std::vector<Shot> all = shots();
        if (shot_ >= all.size()) {
            return;
        }
        const Shot& shot = all[shot_];
        if (settled_ == 0) {
            editor_->closePicker();
            if (editor_->prefab() != shot.prefab) {
                show(shot.prefab);
            }
            game_.selection().set(shot.selected);
        } else if (settled_ == 2 && shot.act) {
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
    std::shared_ptr<jadefx::StackPane> root_;
    ide::IdePrefabEditor* editor_ = nullptr;
    jadefx::Scene* scene_ = nullptr;
    jadefx::Stage* stage_ = nullptr;
    std::string out_dir_;
    std::string theme_;
    InstanceId crate_ = 0;
    InstanceId empty_ = 0;
    InstanceId body_ = 0;
    InstanceId lid_ = 0;
    InstanceId handle_ = 0;
    InstanceId rock_ = 0;
    InstanceId lid_mesh_ = 0;
    InstanceId wood_ = 0;
    std::size_t shot_ = 0;
    int settled_ = 0;
};

}  // namespace

int main(int argc, char** argv) { return jadefx::Application::launch(std::make_unique<PrefabDemo>(), argc, argv); }
