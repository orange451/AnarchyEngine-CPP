#include "ide/CollisionGroupsModel.hpp"
#include "ide/IdeCollisionGroups.hpp"
#include "ide/IdeTheme.hpp"
#include "runner/ViewCapture.hpp"
#include "runner/gl.hpp"

#include "CollisionGroups.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "PhysicsObject.hpp"
#include "ScriptRuntime.hpp"
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

// The Collision Groups pane alone in a window, over a sample place, saved as
// PNGs: a look by hand at what it draws.
//   collision-groups-demo <out-dir> <theme> [width height]
// writes <out-dir>/<theme>-groups.png, -hover.png, -rename.png, -remove.png,
// -playing.png, and -empty.png.
namespace {

using engine_core::CollisionGroups;
using engine_core::InstanceId;
using FilterType = CollisionGroups::FilterType;

// Past the pane's 120 ms fades.
constexpr int kSettleFrames = 24;

class GroupsDemo : public jadefx::Application {
public:
    void start(jadefx::Stage& stage, int argc, char** argv) override {
        out_dir_ = argc > 1 ? argv[1] : ".";
        theme_ = argc > 2 ? argv[2] : "light";
        width_ = argc > 4 ? std::atoi(argv[3]) : 1180;
        height_ = argc > 4 ? std::atoi(argv[4]) : 520;
        ide::set_current_theme(ide::shipped_theme(theme_));
        engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
        fill();

        root_ = jadefx::make<jadefx::StackPane>();
        root_->setPrefWidthRatio(1);
        root_->setPrefHeightRatio(1);
        show(game_);
        auto scene = jadefx::make<jadefx::Scene>(root_, width_, height_);
        scene_ = scene.get();
        stage.setScene(scene);
        stage_ = &stage;
        stage.setRenderingCallback([this](int width, int height) { frame(width, height); });
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {static_cast<double>(width_), static_cast<double>(height_)}; }
    std::string defaultTitle() const override { return "Collision Groups demo"; }

private:
    ide::CollisionGroupsHost host(engine_core::Game& game) {
        ide::CollisionGroupsHost host;
        host.add = [&game](std::shared_ptr<ide::NewGroupResult> result) {
            std::string made;
            ide::run_new_group(game, made);
            result->name = made;
            result->done = true;
        };
        host.rename = [&game](std::string from, std::string to) { ide::run_rename_group(game, from, to); };
        host.remove = [&game](std::string name, ide::GroupBodies bodies) {
            ide::run_delete_group(game, name, bodies);
        };
        host.set_filter_type = [&game](std::string name, FilterType type) {
            ide::run_set_filter_type(game, name, type);
        };
        host.set_collidable = [&game](std::string a, std::string b, bool collide) {
            ide::run_set_collidable(game, a, b, collide);
        };
        host.notice = [](std::string text) { std::printf("notice: %s\n", text.c_str()); };
        return host;
    }

    void show(engine_core::Game& game) {
        root_->getChildren().clear();
        auto pane = jadefx::make<ide::IdeCollisionGroups>(game, host(game));
        ide::Fill(*pane);
        pane_ = pane.get();
        root_->getChildren().add(pane);
    }

    void bodies(const char* group, int count) {
        for (int i = 0; i < count; ++i) {
            engine_core::PhysicsObject& body = game_.create<engine_core::PhysicsObject>();
            body.set_collision_group(group);
            game_.set_parent(body.id(), game_.scene_service("Workspace"));
        }
    }

    void fill() {
        CollisionGroups& groups = game_.collision_groups();
        groups.register_group("Players", FilterType::Exclude);
        groups.register_group("NPCs", FilterType::Exclude);
        groups.register_group("Vehicles", FilterType::Exclude);
        groups.register_group("Projectiles", FilterType::Exclude);
        groups.register_group("Debris", FilterType::Exclude);
        groups.register_group("Ghosts", FilterType::Include);
        groups.register_group("Triggers", FilterType::Include);
        groups.set_collidable("Players", "Players", false);
        groups.set_collidable("Projectiles", "Projectiles", false);
        groups.set_collidable("Projectiles", "Debris", false);
        groups.set_collidable("Debris", "Players", false);
        groups.set_collidable("Debris", "NPCs", false);
        groups.set_collidable("Ghosts", "Default", true);
        groups.set_collidable("Triggers", "Players", true);
        groups.set_collidable("Triggers", "Vehicles", true);
        bodies("Default", 41);
        bodies("Players", 4);
        bodies("NPCs", 12);
        bodies("Vehicles", 3);
        bodies("Projectiles", 0);
        bodies("Debris", 26);
        bodies("Ghosts", 2);
        bodies("Triggers", 5);
        bodies("Crates", 3);
    }

    struct Shot {
        std::string suffix;
        std::function<void()> before;
        std::function<void()> act;
        std::function<void()> after;
    };

    void hover(const std::string& a, const std::string& b) {
        if (jadefx::Node* cell = pane_->cellNode(a, b)) {
            scene_->noteMove(cell->getAbsoluteX() + cell->getWidth() * 0.5, cell->getAbsoluteY() + cell->getHeight() * 0.5);
        }
    }

    std::vector<Shot> shots() {
        return {
            {"groups", nullptr, [this] { scene_->noteMove(2, height_ - 2); }, nullptr},
            {"hover", nullptr,
             [this] {
                 pane_->selectRow("NPCs");
                 hover("Ghosts", "NPCs");
             },
             [this] { scene_->noteMove(2, height_ - 2); }},
            {"rename", nullptr,
             [this] {
                 pane_->addGroup();
             },
             [this] {
                 if (jadefx::TextField* field = pane_->renameField()) {
                     field->fire();
                 }
             }},
            {"remove", nullptr, [this] { pane_->requestRemove("Debris"); },
             [this] {
                 if (auto* cancel = dynamic_cast<jadefx::Button*>(scene_->getElementById("collision-groups-remove-cancel"))) {
                     cancel->fire();
                 }
             }},
            {"playing",
             [this] {
                 game_.capture_place();
                 game_.start_simulation();
             },
             nullptr,
             [this] { game_.stop_simulation(); }},
            {"empty", [this] { show(bare_); }, nullptr, nullptr},
        };
    }

    void frame(int width, int height) {
        const std::vector<Shot> all = shots();
        if (shot_ >= all.size()) {
            return;
        }
        const Shot& shot = all[shot_];
        if (settled_ == 0 && shot.before) {
            shot.before();
        } else if (settled_ == 2 && shot.act) {
            shot.act();
        }
        if (++settled_ < kSettleFrames) {
            return;
        }
        save(width, height, out_dir_ + "/" + theme_ + "-" + shot.suffix + ".png");
        if (shot.after) {
            shot.after();
        }
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

    engine_core::ScriptRuntime runtime_;
    engine_core::Game game_;
    engine_core::Game bare_;
    std::shared_ptr<jadefx::StackPane> root_;
    ide::IdeCollisionGroups* pane_ = nullptr;
    jadefx::Scene* scene_ = nullptr;
    jadefx::Stage* stage_ = nullptr;
    std::string out_dir_;
    std::string theme_;
    int width_ = 1180;
    int height_ = 520;
    std::size_t shot_ = 0;
    int settled_ = 0;
};

}  // namespace

int main(int argc, char** argv) { return jadefx::Application::launch(std::make_unique<GroupsDemo>(), argc, argv); }
