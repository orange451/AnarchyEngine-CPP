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
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// By hand, not ctest: billboard-demo out-dir draws two BillboardGuis beside a
// 4 by 4 by 1 cube in a real window and saves what the Scene View shows. The
// Camera at (0, 0, 10) looks at the origin; the cube sits at (-2, 0, 0). A red
// billboard 4 units wide at (0, 0, -2) is behind the cube on its left half, so
// only its right half shows; a blue AlwaysOnTop one 3 units above it shows
// whole, over the cube. It saves <out-dir>/billboards.png, then orbits the
// Camera 30 degrees around the origin and saves <out-dir>/billboards-moved.png.
namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 760;
// Frames the view gets to make its render targets and draw before a save.
constexpr int kSettleFrames = 20;
constexpr double kOrbitDegrees = 30.0;
constexpr double kCameraDistance = 10.0;

engine_core::LuaSlot Slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

engine_core::InstanceId Create(engine_core::DataModel& game, const char* className, const char* name,
                               engine_core::InstanceId parent) {
    const engine_core::InstanceId id = engine_core::lua_create_instance(game, className)->id();
    game.set_name(id, name);
    game.set_parent(id, parent);
    return id;
}

// The Camera's Transform orbited degrees around the origin's Y axis, still looking at the origin.
engine_core::Matrix4 CameraTransform(double degrees) {
    const double radians = degrees * 3.14159265358979323846 / 180.0;
    engine_core::Matrix4 transform = engine_core::matrix4_axis_angle(engine_core::Vec3{0.f, 1.f, 0.f}, radians);
    transform.m[12] = static_cast<float>(kCameraDistance * std::sin(radians));
    transform.m[13] = 0.f;
    transform.m[14] = static_cast<float>(kCameraDistance * std::cos(radians));
    return transform;
}

class BillboardDemo : public jadefx::Application {
public:
    void start(jadefx::Stage& stage, int argc, char** argv) override {
        out_dir_ = argc > 1 ? argv[1] : ".";
        resources_ = std::filesystem::temp_directory_path() / "anarchy-billboard-demo";
        std::filesystem::remove_all(resources_);
        std::filesystem::create_directories(resources_ / "meshes");
        anarchy::amesh::Data box;
        engine_core::add_box(box, engine_core::Vec3{4.f, 4.f, 1.f}, engine_core::Vec3{});
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
        stage.setTitle("Billboard demo");
        stage_ = &stage;
        runner_.start();
        engine.on_simulation([this](engine_core::DataModel& game) { build(game); });
        engine.resume();
        stage.setRenderingCallback([this](int width, int height) { frame(width, height); });
    }

protected:
    jadefx::Size defaultWindowSize() const override { return {kWidth, kHeight}; }
    std::string defaultTitle() const override { return "Billboard demo"; }

private:
    // SimulationThread. The place: the Camera, the cube, a light, and the two billboards.
    void build(engine_core::DataModel& game) {
        game.set_resources_root(resources_);
        const engine_core::InstanceId workspace = game.scene_service("Workspace");

        const engine_core::InstanceId camera = Create(game, "Camera", "Camera", workspace);
        auto* cameraObject = dynamic_cast<engine_core::Camera*>(game.instance(camera));
        cameraObject->set_field_of_view(engine_core::Camera::kNewPlaceFieldOfView);
        cameraObject->set_transform(CameraTransform(0.0));
        camera_ = camera;
        cameraGuid_ = game.guid(camera);

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

        const engine_core::InstanceId cube = Create(game, "GameObject", "Cube", workspace);
        auto* cubeObject = dynamic_cast<engine_core::GameObject*>(game.instance(cube));
        cubeObject->set_prefab(Slot(prefab));
        cubeObject->set_transform(engine_core::matrix4_translation(-2.f, 0.f, 0.f));

        Create(game, "DirectionalLight", "Sun", game.scene_service("Lighting"));

        board(game, workspace, "Behind", engine_core::Vec3{0.f, 0.f, -2.f}, false, "red");
        board(game, workspace, "OnTop", engine_core::Vec3{0.f, 3.f, -2.f}, true, "blue");
        built_ = true;
    }

    // SimulationThread. A GameObject with no Prefab at where, holding a
    // BillboardGui 4 by 2 units filled with one Pane of color. The size is on
    // the BillboardGui itself: a Pane's percentage height of a BillboardGui
    // whose height is its content's would be its own content's, none.
    void board(engine_core::DataModel& game, engine_core::InstanceId workspace, const char* name,
               engine_core::Vec3 where, bool onTop, const char* color) {
        const engine_core::InstanceId anchor = Create(game, "GameObject", name, workspace);
        dynamic_cast<engine_core::GameObject*>(game.instance(anchor))
            ->set_transform(engine_core::matrix4_translation(where.x, where.y, where.z));
        const engine_core::InstanceId gui = Create(game, "BillboardGui", "Board", anchor);
        if (onTop) {
            engine_core::LuaSlot yes;
            yes.kind = engine_core::LuaSlot::Kind::Bool;
            yes.flag = true;
            dynamic_cast<engine_core::BillboardGui*>(game.instance(gui))
                ->set_value(engine_core::GuiProperty::AlwaysOnTop, yes);
        }
        Create(game, "Pane", "Fill", gui);
        const engine_core::InstanceId sheet = Create(game, "CSS", "Style", gui);
        dynamic_cast<engine_core::Css*>(game.instance(sheet))
            ->set_text(engine_core::GuiProperty::Source,
                       std::string("billboardgui { width: 400%; height: 200%; } "
                                   "pane { width: 100%; height: 100%; background-color: ") +
                           color + "; }");
    }

    void frame(int width, int height) {
        ++frames_;
        if (built_ && !linked_) {
            view_->linkCamera(cameraGuid_);
            linked_ = true;
            frames_ = 0;
        } else if (!linked_) {
            return;
        } else if (frames_ == kSettleFrames) {
            save(width, height, "billboards.png");
            runner_.simulation().on_simulation([camera = camera_](engine_core::DataModel& game) {
                if (auto* object = dynamic_cast<engine_core::Camera*>(game.instance(camera))) {
                    object->set_transform(CameraTransform(kOrbitDegrees));
                }
            });
        } else if (frames_ == kSettleFrames * 2) {
            save(width, height, "billboards-moved.png");
            stage_->close();
        }
    }

    void save(int width, int height, const std::string& name) {
        runner::LoadGl([](const char* symbol) { return reinterpret_cast<void*>(glfwGetProcAddress(symbol)); });
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
        const std::string path = out_dir_ + "/" + name;
        std::ofstream(path, std::ios::binary) << runner::EncodePng(pixels);
        std::printf("saved %s (%dx%d)\n", path.c_str(), width, height);
    }

    runner::Runner runner_;
    runner::GameView* view_ = nullptr;
    jadefx::Stage* stage_ = nullptr;
    std::string out_dir_;
    std::filesystem::path resources_;
    engine_core::InstanceId camera_ = 0;
    std::string cameraGuid_;
    // Set on the simulation thread once the place is built; cameraGuid_ is written before it.
    std::atomic<bool> built_{false};
    bool linked_ = false;
    int frames_ = 0;
};

}  // namespace

int main(int argc, char** argv) { return jadefx::Application::launch(std::make_unique<BillboardDemo>(), argc, argv); }
