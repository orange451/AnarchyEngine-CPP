#include "ide/IdeTheme.hpp"
#include "runner/GameView.hpp"
#include "runner/Runner.hpp"
#include "runner/ViewCapture.hpp"
#include "runner/gl.hpp"

#include "AssetInstances.hpp"
#include "Camera.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "GameObject.hpp"
#include "Gui.hpp"
#include "LuaApi.hpp"
#include "Matrix4.hpp"
#include "MeshShapes.hpp"
#include "ScriptAnalysis.hpp"
#include "amesh.hpp"

#include "jadefx/jadefx.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// By hand, not ctest: viewport-demo out-dir shows two ViewportPanes in a
// ScreenGui over an empty place and saves the window. The left pane, 128
// points square with BackgroundTransparency 1, holds a gray cube, a
// DirectionalLight, and a Camera at (0, 0, 6) looking at it: an icon on
// nothing, so the dark view shows around the cube. The right pane, 320 points
// square, holds the same cube, its own light and Camera, and turns its cube a
// little each frame. It saves <out-dir>/viewports.png after 20 frames and
// viewports-later.png 30 frames after that; the cube in the right pane must
// have turned between the two, the left must not have changed, and the left
// pane's middle must be the lit cube. It exits 1 when any of these is not so.
namespace {

constexpr int kWidth = 900;
constexpr int kHeight = 600;
constexpr int kFirstSave = 20;
constexpr int kSecondSave = 50;
// About 45 degrees between the two saves, so the turn shows on a cube.
constexpr double kTurnPerFrame = 0.026;

bool g_failed = false;

engine_core::LuaSlot Slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

engine_core::LuaSlot Size(float x, float y) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Vec2;
    slot.vec = engine_core::Vec3{x, y, 0.f};
    return slot;
}

engine_core::LuaSlot Number(double value) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

engine_core::InstanceId Create(engine_core::DataModel& game, const char* className, const char* name,
                               engine_core::InstanceId parent) {
    const engine_core::InstanceId id = engine_core::lua_create_instance(game, className)->id();
    game.set_name(id, name);
    game.set_parent(id, parent);
    return id;
}

// A window's pixels, top row first, and the points-to-pixels scale.
struct Shot {
    runner::ViewPixels pixels;
    double scale = 1.0;
};

class ViewportDemo : public jadefx::Application {
public:
    void start(jadefx::Stage& stage, int argc, char** argv) override {
        out_dir_ = argc > 1 ? argv[1] : ".";
        resources_ = std::filesystem::temp_directory_path() / "anarchy-viewport-demo";
        std::filesystem::remove_all(resources_);
        std::filesystem::create_directories(resources_ / "meshes");
        anarchy::amesh::Data box;
        engine_core::add_box(box, engine_core::Vec3{2.f, 2.f, 2.f}, engine_core::Vec3{});
        const std::vector<std::byte> bytes = anarchy::amesh::write(box);
        std::ofstream(resources_ / "meshes" / "cube.amesh", std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

        ide::set_current_theme(ide::shipped_theme("dark"));
        runner_.prepare();
        engine_core::Engine& engine = runner_.simulation();
        engine.analysis().set_enabled(false);
        runner_.setSceneGrid(false);
        auto view = jadefx::make<runner::GameView>(runner_, "Scene View");
        view->setPrefWidthRatio(1);
        view->setPrefHeightRatio(1);
        view_ = view.get();
        auto scene = jadefx::make<jadefx::Scene>(view, kWidth, kHeight);
        stage.setScene(scene);
        stage.setTitle("Viewport demo");
        stage_ = &stage;
        runner_.start();
        engine.on_simulation([this](engine_core::DataModel& game) { build(game); });
        engine.resume();
        stage.setRenderingCallback([this](int width, int height) { frame(width, height); });
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {kWidth, kHeight}; }
    std::string defaultTitle() const override { return "Viewport demo"; }

private:
    // SimulationThread. A ScreenGui holding an HBox of two panes, each with a cube, a light, and a Camera.
    void build(engine_core::DataModel& game) {
        game.set_resources_root(resources_);
        const engine_core::InstanceId mesh = Create(game, "Mesh", "Cube", game.service("Meshes"));
        dynamic_cast<engine_core::Mesh*>(game.instance(mesh))->set_path("meshes/cube.amesh");
        const engine_core::InstanceId material = Create(game, "Material", "Gray", game.service("Materials"));
        dynamic_cast<engine_core::Material*>(game.instance(material))
            ->set_color(engine_core::ColorRgb{0.7f, 0.7f, 0.7f, 1.f});
        const engine_core::InstanceId prefab = Create(game, "Prefab", "CubePrefab", game.service("Prefabs"));
        const engine_core::InstanceId model = Create(game, "Model", "Body", prefab);
        auto* modelObject = dynamic_cast<engine_core::ReferenceAsset*>(game.instance(model));
        modelObject->set_reference(engine_core::Model::kMeshReference, Slot(mesh));
        modelObject->set_reference(engine_core::Model::kMaterialReference, Slot(material));

        const engine_core::InstanceId screen = Create(game, "ScreenGui", "Screen", game.scene_service("Gui"));
        const engine_core::InstanceId row = Create(game, "HBox", "Row", screen);
        auto* rowValues = dynamic_cast<engine_core::GuiValues*>(game.instance(row));
        rowValues->set_value(engine_core::GuiProperty::Spacing, Number(40));
        // No box behind the panes, so the icon's own clear background shows the view.
        rowValues->set_value(engine_core::GuiProperty::BackgroundTransparency, Number(1));
        icon_ = pane(game, row, "Icon", 128.f, prefab, &iconCube_);
        preview_ = pane(game, row, "Preview", 320.f, prefab, &previewCube_);
        dynamic_cast<engine_core::GuiValues*>(game.instance(icon_))
            ->set_value(engine_core::GuiProperty::BackgroundTransparency, Number(1));
        built_ = true;
    }

    // SimulationThread. A ViewportPane side points square in parent with a cube, a sun, and its Camera.
    engine_core::InstanceId pane(engine_core::DataModel& game, engine_core::InstanceId parent, const char* name,
                                 float side, engine_core::InstanceId prefab, engine_core::InstanceId* cube) {
        const engine_core::InstanceId id = Create(game, "ViewportPane", name, parent);
        auto* viewport = dynamic_cast<engine_core::ViewportPane*>(game.instance(id));
        viewport->set_value(engine_core::GuiProperty::Size, Size(side, side));
        const engine_core::InstanceId camera = Create(game, "Camera", "Camera", id);
        auto* cameraObject = dynamic_cast<engine_core::Camera*>(game.instance(camera));
        cameraObject->set_field_of_view(40.0);
        cameraObject->set_transform(engine_core::matrix4_translation(0.f, 0.f, 6.f));
        viewport->set_current_camera(Slot(camera));
        *cube = Create(game, "GameObject", "Cube", id);
        auto* cubeObject = dynamic_cast<engine_core::GameObject*>(game.instance(*cube));
        cubeObject->set_prefab(Slot(prefab));
        cubeObject->set_transform(engine_core::matrix4_axis_angle(engine_core::Vec3{1.f, 1.f, 0.f}, 0.6));
        Create(game, "DirectionalLight", "Sun", id);
        return id;
    }

    void frame(int width, int height) {
        if (!built_) {
            return;
        }
        ++frames_;
        // The preview's cube turns a little every frame; the icon's never moves.
        const double turn = frames_ * kTurnPerFrame;
        runner_.simulation().on_simulation([cube = previewCube_, turn](engine_core::DataModel& game) {
            if (auto* object = dynamic_cast<engine_core::GameObject*>(game.instance(cube))) {
                object->set_transform(engine_core::matrix4_axis_angle(engine_core::Vec3{0.f, 1.f, 0.f}, turn));
            }
        });
        if (frames_ == kFirstSave) {
            first_ = save(width, height, "viewports.png");
        } else if (frames_ == kSecondSave) {
            const Shot second = save(width, height, "viewports-later.png");
            check(first_, second);
            stage_->close();
        }
    }

    // The pane's rectangle in framebuffer pixels: x, y, width, height, top left origin.
    bool rect(engine_core::InstanceId id, double scale, int out[4]) const {
        const jadefx::Node* node = view_->guiLayer().nodeFor(id);
        if (node == nullptr) {
            return false;
        }
        out[0] = static_cast<int>(node->getAbsoluteX() * scale);
        out[1] = static_cast<int>(node->getAbsoluteY() * scale);
        out[2] = static_cast<int>(node->getWidth() * scale);
        out[3] = static_cast<int>(node->getHeight() * scale);
        return out[2] > 0 && out[3] > 0;
    }

    // How many pixels inside r differ between a and b by more than a rounding step.
    static int Differing(const Shot& a, const Shot& b, const int r[4]) {
        int count = 0;
        for (int y = r[1]; y < r[1] + r[3] && y < a.pixels.height; ++y) {
            for (int x = r[0]; x < r[0] + r[2] && x < a.pixels.width; ++x) {
                const std::size_t at = (static_cast<std::size_t>(y) * a.pixels.width + x) * 4;
                for (int channel = 0; channel < 3; ++channel) {
                    if (std::abs(a.pixels.rgba[at + channel] - b.pixels.rgba[at + channel]) > 2) {
                        ++count;
                        break;
                    }
                }
            }
        }
        return count;
    }

    void check(const Shot& first, const Shot& second) {
        int icon[4] = {};
        int preview[4] = {};
        const bool placed = rect(icon_, second.scale, icon) && rect(preview_, second.scale, preview);
        const bool same = first.pixels.width == second.pixels.width && first.pixels.height == second.pixels.height;
        if (!placed || !same || first.pixels.rgba.empty()) {
            std::printf("panes placed and both shots the same size: NO\n");
            g_failed = true;
            return;
        }
        const int iconChanged = Differing(first, second, icon);
        const int previewChanged = Differing(first, second, preview);
        const std::size_t middle =
            (static_cast<std::size_t>(icon[1] + icon[3] / 2) * second.pixels.width + (icon[0] + icon[2] / 2)) * 4;
        const int lit = std::max({second.pixels.rgba[middle], second.pixels.rgba[middle + 1],
                                  second.pixels.rgba[middle + 2]});
        std::printf("icon pixels changed: %d (want 0)\n", iconChanged);
        std::printf("preview pixels changed: %d (want many)\n", previewChanged);
        std::printf("icon middle brightness: %d (want over 60)\n", lit);
        if (iconChanged != 0 || previewChanged < 100 || lit <= 60) {
            g_failed = true;
        }
    }

    Shot save(int width, int height, const std::string& name) {
        runner::LoadGl([](const char* symbol) { return reinterpret_cast<void*>(glfwGetProcAddress(symbol)); });
        Shot shot;
        shot.scale = static_cast<double>(width) / kWidth;
        shot.pixels.width = width;
        shot.pixels.height = height;
        std::vector<unsigned char> bottomUp(static_cast<std::size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, bottomUp.data());
        shot.pixels.rgba.resize(bottomUp.size());
        const std::size_t row = static_cast<std::size_t>(width) * 4;
        for (int y = 0; y < height; ++y) {
            std::copy_n(bottomUp.data() + row * static_cast<std::size_t>(height - 1 - y), row,
                        shot.pixels.rgba.data() + row * static_cast<std::size_t>(y));
        }
        const std::string path = out_dir_ + "/" + name;
        std::ofstream(path, std::ios::binary) << runner::EncodePng(shot.pixels);
        std::printf("saved %s (%dx%d)\n", path.c_str(), width, height);
        return shot;
    }

    runner::Runner runner_;
    runner::GameView* view_ = nullptr;
    jadefx::Stage* stage_ = nullptr;
    std::string out_dir_;
    std::filesystem::path resources_;
    // Set on the simulation thread before built_.
    engine_core::InstanceId icon_ = 0;
    engine_core::InstanceId preview_ = 0;
    engine_core::InstanceId iconCube_ = 0;
    engine_core::InstanceId previewCube_ = 0;
    std::atomic<bool> built_{false};
    int frames_ = 0;
    Shot first_;
};

}  // namespace

int main(int argc, char** argv) {
    const int code = jadefx::Application::launch(std::make_unique<ViewportDemo>(), argc, argv);
    return code != 0 ? code : (g_failed ? 1 : 0);
}
