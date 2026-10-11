#include "ide/IdeLayout.hpp"
#include "runner/GameView.hpp"
#include "runner/ViewportNode.hpp"
#include "runner/ViewportRenderer.hpp"

#include "Camera.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "GameObject.hpp"
#include "Gui.hpp"
#include "LuaApi.hpp"
#include "SnapshotPump.hpp"

#include "jadefx/jadefx.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

// A ViewportPane in the Scene View's GUI layer: a viewportpane node holding its
// pane's scene from the frame's snapshot, GUI children as nodes over it, 3D
// children not. Headless, the engine publishes no snapshots, so the view is
// handed made ones, as the billboard tests do; what the pump puts in them is
// checked by the sandbox's VS tests.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

runner::GameView* FindGameView(jadefx::Scene& scene) {
    for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("ide-pane")) {
        if (auto* view = dynamic_cast<runner::GameView*>(node)) {
            return view;
        }
    }
    return nullptr;
}

// A snapshot holding one pane's scene at revision, drawn from camera.
std::shared_ptr<engine_core::VisualSnapshot> PaneShot(engine_core::InstanceId pane, engine_core::InstanceId camera,
                                                      std::uint64_t revision) {
    auto shot = std::make_shared<engine_core::VisualSnapshot>();
    shot->frame = revision;
    auto scene = std::make_shared<engine_core::VisualViewport>();
    scene->pane = pane;
    scene->revision = revision;
    scene->camera = camera;
    scene->has_camera = true;
    scene->camera_inside = true;
    scene->prefabs.push_back(nullptr);
    shot->viewports.push_back(scene);
    return shot;
}

}  // namespace

int RunViewportPaneTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    double time = scene.timeSeconds() + 0.01;
    auto frame = [&] {
        for (int i = 0; i < 4; ++i) {
            scene.layout(1280, 800, time);
            time += 0.02;
        }
    };
    frame();
    runner::GameView* view = FindGameView(scene);
    Expect(view != nullptr, "VN the studio has a Scene View");
    if (view == nullptr) {
        return gFailures;
    }
    engine_core::Engine& engine = layout.simulation();
    engine_core::InstanceId screen = 0;
    engine_core::InstanceId pane = 0;
    engine_core::InstanceId label = 0;
    engine_core::InstanceId thing = 0;
    engine_core::InstanceId camera = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        screen = engine_core::lua_create_instance(game, "ScreenGui")->id();
        game.set_parent(screen, game.scene_service("Gui"));
        pane = engine_core::lua_create_instance(game, "ViewportPane")->id();
        game.set_parent(pane, screen);
        label = engine_core::lua_create_instance(game, "Label")->id();
        game.set_parent(label, pane);
        thing = engine_core::lua_create_instance(game, "GameObject")->id();
        game.set_parent(thing, pane);
        camera = engine_core::lua_create_instance(game, "Camera")->id();
        game.set_parent(camera, pane);
    });
    frame();
    jadefx::Node* node = view->guiLayer().nodeFor(pane);
    Expect(node != nullptr, "VN1 a ViewportPane in a ScreenGui is drawn");
    if (node == nullptr) {
        return gFailures;
    }
    Expect(std::string(node->getElementType()) == "viewportpane", "VN1 its element type is viewportpane");
    auto* viewport = dynamic_cast<runner::ViewportNode*>(node);
    Expect(viewport != nullptr, "VN2 its node is a ViewportNode");
    Expect(view->guiLayer().nodeFor(label) != nullptr, "VN3 a GUI child is a node over it");
    Expect(view->guiLayer().nodeFor(thing) == nullptr && view->guiLayer().nodeFor(camera) == nullptr,
           "VN3 its 3D children are not nodes");
    if (viewport != nullptr) {
        view->setSnapshotForTest(PaneShot(pane, camera, 1));
        frame();
        Expect(viewport->scene() != nullptr && viewport->scene()->pane == pane,
               "VN4 the node holds its pane's scene from the frame's snapshot");
        Expect(viewport->scene() != nullptr && viewport->scene()->camera == camera,
               "VN4 the scene draws from the Camera inside");
        view->setSnapshotForTest(PaneShot(pane, camera, 2));
        frame();
        Expect(viewport->scene() != nullptr && viewport->scene()->revision == 2,
               "VN5 a newer snapshot gives the node its newer scene");
        auto other = PaneShot(pane + 1, camera, 3);
        view->setSnapshotForTest(other);
        frame();
        Expect(viewport->scene() == nullptr, "VN5 a snapshot without its pane leaves the node no scene");
        view->setSnapshotForTest(nullptr);
        engine.on_simulation([&](engine_core::DataModel& game) {
            engine_core::LuaSlot value;
            value.kind = engine_core::LuaSlot::Kind::Number;
            value.number = 0.25;
            if (auto* object = dynamic_cast<engine_core::ViewportPane*>(game.instance(pane))) {
                object->set_value(engine_core::GuiProperty::ImageTransparency, value);
            }
        });
        frame();
        Expect(std::abs(viewport->imageOpacity() - 0.75f) < 1e-4f,
               "VN6 ImageTransparency 0.25 draws the image at 0.75");
    }
    // VN7: a Camera in a pane under Workspace is not offered by the Scene View.
    engine_core::InstanceId anchor = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        anchor = engine_core::lua_create_instance(game, "GameObject")->id();
        game.set_parent(anchor, game.scene_service("Workspace"));
        const engine_core::InstanceId board = engine_core::lua_create_instance(game, "BillboardGui")->id();
        game.set_parent(board, anchor);
        const engine_core::InstanceId inner = engine_core::lua_create_instance(game, "ViewportPane")->id();
        game.set_parent(inner, board);
        const engine_core::InstanceId hidden = engine_core::lua_create_instance(game, "Camera")->id();
        game.set_name(hidden, "PaneCamera");
        game.set_parent(hidden, inner);
    });
    frame();
    bool offered = false;
    for (const std::string& item : view->cameraList().getItems()) {
        offered = offered || item == "PaneCamera";
    }
    Expect(!offered, "VN7 a Camera inside a ViewportPane is not in the Scene View's camera list");

    // VN8: the pane reports its size, in points, to the Camera inside it.
    if (viewport != nullptr) {
        view->setSnapshotForTest(PaneShot(pane, camera, 4));
        frame();
        frame();
        float reportedWidth = 0.f;
        engine.on_simulation([&](engine_core::DataModel& game) {
            if (auto* own = dynamic_cast<engine_core::Camera*>(game.instance(camera))) {
                reportedWidth = own->viewport_size().x;
            }
        });
        frame();
        Expect(reportedWidth > 0.f && std::abs(reportedWidth - static_cast<float>(node->getWidth())) < 0.5f,
               "VN8 a pane's Camera gets the pane's size as its ViewportSize");
        view->setSnapshotForTest(nullptr);
    }

    // VN9: a plugin pane's owned caches change root only in paint, with their context current.
    {
        runner::ViewportRenderer owned(nullptr, nullptr);
        owned.setResourcesRoot("elsewhere");
        Expect(owned.resourcesRoot().empty(), "VN9 a new root waits for the next paint");
    }

    engine.on_simulation([&](engine_core::DataModel& game) {
        game.destroy_tree(screen);
        game.destroy_tree(anchor);
    });
    frame();
    if (gFailures == 0) {
        std::printf("viewport pane tests passed\n");
    }
    return gFailures;
}
