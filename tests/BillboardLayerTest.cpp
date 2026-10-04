#include "ide/IdeLayout.hpp"
#include "runner/GameView.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "Gui.hpp"
#include "LuaApi.hpp"
#include "Matrix4.hpp"
#include "SnapshotPump.hpp"

#include "jadefx/jadefx.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

// BillboardGuis in the Scene View's GUI layer: placed from the frame's
// snapshot by its camera, sized in world units by CSS percentages, stacked by
// depth under the ScreenGuis, and passing the mouse where the scene hides them.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

bool Near(double a, double b) { return std::abs(a - b) < 0.75; }

runner::GameView* FindGameView(jadefx::Scene& scene) {
    for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("ide-pane")) {
        if (auto* view = dynamic_cast<runner::GameView*>(node)) {
            return view;
        }
    }
    return nullptr;
}

}  // namespace

int RunBillboardLayerTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
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
    engine_core::Engine& engine = layout.simulation();

    engine_core::InstanceId camera = 0;
    engine_core::InstanceId near = 0;
    engine_core::InstanceId far = 0;
    engine_core::InstanceId top = 0;
    engine_core::InstanceId label = 0;
    engine_core::InstanceId button = 0;
    engine_core::InstanceId screen = 0;
    std::string cameraGuid;
    engine.on_simulation([&](engine_core::DataModel& game) {
        const engine_core::InstanceId workspace = game.scene_service("Workspace");
        camera = engine_core::lua_create_instance(game, "Camera")->id();
        game.set_parent(camera, workspace);
        cameraGuid = game.guid(camera);
        auto board = [&](const char* name) {
            const engine_core::InstanceId id = engine_core::lua_create_instance(game, "BillboardGui")->id();
            game.set_name(id, name);
            game.set_parent(id, workspace);
            return id;
        };
        near = board("Near");
        far = board("Far");
        top = board("Top");
        label = engine_core::lua_create_instance(game, "Label")->id();
        game.set_parent(label, near);
        button = engine_core::lua_create_instance(game, "Button")->id();
        game.set_parent(button, near);
        const engine_core::InstanceId sheet = engine_core::lua_create_instance(game, "CSS")->id();
        game.set_parent(sheet, near);
        dynamic_cast<engine_core::Css*>(game.instance(sheet))
            ->set_text(engine_core::GuiProperty::Source,
                       "#Near { width: calc(200% + 32px); height: 50%; } label { width: 50%; } "
                       "button { width: 10px; height: 10px; }");
        screen = engine_core::lua_create_instance(game, "ScreenGui")->id();
        game.set_parent(screen, game.scene_service("Gui"));
    });
    frame();
    view->linkCamera(cameraGuid);

    // The frame both the layout and the paint use: the Camera at the origin,
    // looking down -Z with a 90 degree view, and three billboards ahead.
    auto shot = std::make_shared<engine_core::VisualSnapshot>();
    shot->frame = 1;
    engine_core::VisualInstance cam;
    cam.id = camera;
    cam.field_of_view = 90.f;
    shot->instances.push_back(cam);
    auto row = [&](engine_core::InstanceId id, float z, bool onTop) {
        engine_core::VisualBillboard board;
        board.id = id;
        board.anchor = engine_core::Vec3{0.f, 0.f, z};
        board.always_on_top = onTop;
        shot->billboards.push_back(board);
    };
    row(near, -10.f, false);
    row(far, -20.f, false);
    row(top, -50.f, true);
    view->setSnapshotForTest(shot);
    frame();

    runner::GuiLayer& layer = view->guiLayer();
    jadefx::Node* nearNode = layer.nodeFor(near);
    jadefx::Node* labelNode = layer.nodeFor(label);
    Expect(nearNode != nullptr && labelNode != nullptr, "a BillboardGui in Workspace and its Label are drawn");
    if (nearNode == nullptr || labelNode == nullptr) {
        return gFailures;
    }
    Expect(std::string(nearNode->getElementType()) == "billboardgui", "its element type is billboardgui");

    const double ppu = view->getHeight() / 20.0;
    Expect(Near(nearNode->getWidth(), 2.0 * ppu + 32.0), "calc(200% + 32px) is two world units and 32 points");
    Expect(Near(nearNode->getHeight(), 0.5 * ppu), "50% tall is half a world unit");
    Expect(Near(labelNode->getWidth(), 0.5 * nearNode->getWidth()), "a child's 50% is half the billboard");
    Expect(Near(nearNode->getAbsoluteX() + nearNode->getWidth() / 2, view->getAbsoluteX() + view->getWidth() / 2) &&
               Near(nearNode->getAbsoluteY() + nearNode->getHeight() / 2,
                    view->getAbsoluteY() + view->getHeight() / 2),
           "it is centred on its anchor's point on screen");

    const std::vector<jadefx::Node*> order = layer.paintOrder();
    Expect(order.size() == 4 && order[0] == layer.nodeFor(far) && order[1] == nearNode &&
               order[2] == layer.nodeFor(top) && order[3] == layer.nodeFor(screen),
           "depth-tested billboards far to near, then AlwaysOnTop ones, then ScreenGuis");

    // The same frame moves the camera and the anchor together: the billboard
    // follows that frame, not the one before.
    auto moved = std::make_shared<engine_core::VisualSnapshot>(*shot);
    moved->frame = 2;
    moved->instances[0].world = engine_core::matrix4_translation(3.f, 0.f, 0.f);
    moved->billboards[0].anchor = engine_core::Vec3{3.f, 0.f, -10.f};
    view->setSnapshotForTest(moved);
    frame();
    Expect(Near(nearNode->getAbsoluteX() + nearNode->getWidth() / 2, view->getAbsoluteX() + view->getWidth() / 2),
           "a camera and anchor moved in one frame leave it centred");

    // Where the scene under the cursor is nearer, a depth-tested billboard passes the mouse.
    // The Button sits at the billboard's top left (Alignment TopLeft); aim at its middle.
    jadefx::Node* buttonNode = layer.nodeFor(button);
    Expect(buttonNode != nullptr, "the billboard's Button is drawn");
    if (buttonNode == nullptr) {
        return gFailures;
    }
    const double cx = buttonNode->getAbsoluteX() + buttonNode->getWidth() / 2;
    const double cy = buttonNode->getAbsoluteY() + buttonNode->getHeight() / 2;
    layer.setCursorDepth(0.5f);
    frame();
    Expect(layer.pick(cx, cy) != buttonNode, "a Button behind nearer scene takes no click");
    layer.setCursorDepth(1.f);
    frame();
    Expect(layer.pick(cx, cy) == buttonNode, "with the scene farther, it does");
    layer.setCursorDepth(std::nullopt);

    // Behind the camera, it is hidden.
    auto behind = std::make_shared<engine_core::VisualSnapshot>(*shot);
    behind->billboards[0].anchor = engine_core::Vec3{0.f, 0.f, 10.f};
    view->setSnapshotForTest(behind);
    frame();
    Expect(!nearNode->isVisible(), "a billboard behind the camera is hidden");

    view->setSnapshotForTest(nullptr);
    engine.on_simulation([&](engine_core::DataModel& game) {
        for (engine_core::InstanceId id : {near, far, top, screen, camera}) {
            game.destroy_tree(id);
        }
    });
    frame();
    Expect(layer.nodeFor(near) == nullptr, "removing a BillboardGui takes its node away");
    if (gFailures == 0) {
        std::printf("billboard layer tests passed\n");
    }
    return gFailures;
}
