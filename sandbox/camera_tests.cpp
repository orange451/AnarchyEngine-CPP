// Camera: a GameObject with a FieldOfView, which a new place starts with in
// Workspace, and which the render snapshot carries for the Scene View.

#include "support.hpp"

#include "Camera.hpp"
#include "Light.hpp"
#include "ChangeHistoryService.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using engine_core::Camera;
using engine_core::InstanceId;

Camera& add_camera(engine_core::DataModel& game) {
    Camera& camera = game.create<Camera>();
    game.set_parent(camera.id(), workspace_of(game));
    return camera;
}

// The Cameras directly under Workspace.
std::vector<Camera*> workspace_cameras(engine_core::DataModel& game) {
    std::vector<Camera*> out;
    for (const InstanceId child : game.get_children(workspace_of(game))) {
        if (auto* camera = dynamic_cast<Camera*>(game.instance(child))) {
            out.push_back(camera);
        }
    }
    return out;
}

engine_core::Matrix4 default_view() {
    return engine_core::matrix4_look_at(engine_core::Vec3{0.f, 3.f, 7.f}, engine_core::Vec3{0.f, 0.f, 0.f},
                                        engine_core::Vec3{0.f, 1.f, 0.f});
}

void require_globals(ScriptRig& rig, std::initializer_list<const char*> names) {
    INFO(rig.runtime.last_error());
    for (const char* name : names) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

}  // namespace

TEST_CASE("CAM1 a Camera's FieldOfView is clamped, undoes, and comes back at Stop", "[camera]") {
    SimRole role;
    engine_core::Game game;
    Camera& camera = add_camera(game);
    REQUIRE(camera.field_of_view() == Camera::kDefaultFieldOfView);
    REQUIRE(game.game_object(camera.id()) == &camera);

    // A default Camera saves no FieldOfView.
    engine_core::PropertyBag saved;
    camera.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "FieldOfView") == nullptr);

    begin_step(game, "Set FieldOfView");
    REQUIRE_FALSE(camera.set_field_of_view(45.0));
    end_step(game);
    REQUIRE(camera.field_of_view() == 45.0);
    game.history().undo();
    REQUIRE(camera.field_of_view() == Camera::kDefaultFieldOfView);
    game.history().redo();
    REQUIRE(camera.field_of_view() == 45.0);

    REQUIRE_FALSE(camera.set_field_of_view(500.0));
    REQUIRE(camera.field_of_view() == Camera::kMaxFieldOfView);
    REQUIRE_FALSE(camera.set_field_of_view(0.0));
    REQUIRE(camera.field_of_view() == Camera::kMinFieldOfView);
    REQUIRE(*camera.set_field_of_view(std::nan("")) == "FieldOfView must be a finite number");
    REQUIRE(camera.field_of_view() == Camera::kMinFieldOfView);

    REQUIRE_FALSE(camera.set_field_of_view(50.0));
    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(camera.set_field_of_view(90.0));
    game.stop_simulation();
    REQUIRE(camera.field_of_view() == 50.0);
}

TEST_CASE("CAM2 scripts make a Camera and set its FieldOfView", "[camera]") {
    ScriptRig rig;
    add_script(rig.game, "Cameras", R"(
        local camera = Instance.new("Camera", workspace)
        _G.isa = camera:IsA("GameObject") and camera.ClassName == "Camera"
        _G.default = camera.FieldOfView == 70
        camera.FieldOfView = 30
        _G.set = camera.FieldOfView == 30
        camera.Transform = camera.Transform.Rotation + Vector3.new(1, 2, 3)
        _G.moved = camera.Transform.Position == Vector3.new(1, 2, 3)
        _G.no_nan = not pcall(function() camera.FieldOfView = 0 / 0 end)
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    require_globals(rig, {"isa", "default", "set", "moved", "no_nan"});
}

TEST_CASE("CAM3 the snapshot row of a Camera in Workspace carries its FieldOfView", "[camera][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    Camera& camera = add_camera(game);
    engine_core::GameObject& part = create_part(game);
    frame();
    REQUIRE(pump.find(camera.id()) != nullptr);
    REQUIRE(pump.find(camera.id())->field_of_view == 70.f);
    REQUIRE(pump.find(part.id())->field_of_view == 0.f);

    REQUIRE_FALSE(camera.set_field_of_view(40.0));
    camera.set_transform(default_view());
    frame();
    REQUIRE(pump.find(camera.id())->field_of_view == 40.f);
    REQUIRE(engine_core::same_matrix4(pump.find(camera.id())->world, default_view()));

    // Out of Workspace, the row is gone; back in, it reads every field again.
    game.set_parent(camera.id(), game.scene_service("Storage"));
    frame();
    REQUIRE(pump.find(camera.id()) == nullptr);
    game.set_parent(camera.id(), workspace_of(game));
    frame();
    REQUIRE(pump.find(camera.id())->field_of_view == 40.f);
}

TEST_CASE("CAM4 a new place and a new project start with a Camera in Workspace", "[camera][project]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        const std::vector<Camera*> cameras = workspace_cameras(project.datamodel());
        REQUIRE(cameras.size() == 1);
        REQUIRE(engine_core::same_matrix4(cameras[0]->transform(), default_view()));
        REQUIRE(cameras[0]->field_of_view() == Camera::kNewPlaceFieldOfView);
        REQUIRE_FALSE(project.unsaved());
        REQUIRE_FALSE(cameras[0]->set_field_of_view(90.0));
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    std::vector<Camera*> cameras = workspace_cameras(game);
    REQUIRE(cameras.size() == 1);
    REQUIRE(cameras[0]->field_of_view() == 90.0);
    // By value: a file keeps no sign on a zero.
    const engine_core::Matrix4 loaded_view = cameras[0]->transform();
    const engine_core::Matrix4 expected_view = default_view();
    for (int index = 0; index < 16; ++index) {
        INFO(index);
        REQUIRE(loaded_view.m[index] == expected_view.m[index]);
    }

    engine_core::Project::reset_place(game);
    cameras = workspace_cameras(game);
    REQUIRE(cameras.size() == 1);
    REQUIRE(cameras[0]->field_of_view() == Camera::kNewPlaceFieldOfView);
    REQUIRE_FALSE(game.history().can_undo().first);
}

TEST_CASE("CAM5 a camera flown outside any recording is written by the next save", "[camera][project]") {
    SimRole role;
    TempDir dir;
    const engine_core::Matrix4 flown = engine_core::matrix4_translation(3.f, 4.f, 5.f);
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        Camera* camera = workspace_cameras(project.datamodel())[0];
        camera->set_transform(flown);
        REQUIRE_FALSE(project.datamodel().history().can_undo().first);
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    REQUIRE(engine_core::same_matrix4(workspace_cameras(game)[0]->transform(), flown));
}

TEST_CASE("CAM6 Lua turns view points into rays and world points into view points", "[camera]") {
    ScriptRig rig;
    Camera& camera = add_camera(rig.game);
    rig.game.set_name(camera.id(), "Cam");
    camera.set_field_of_view(90);
    Camera& hidden = add_camera(rig.game);
    rig.game.set_name(hidden.id(), "Hidden");
    camera.set_viewport_size(engine_core::Vec2{200, 100});
    rig.runtime.run_chunk(R"(
        local cam = workspace.Cam
        local origin, dir = cam:ViewportPointToRay(100, 50)
        print("ray", origin.Magnitude, dir.Z, dir.Magnitude)
        local _, corner = cam:ViewportPointToRay(200, 0)
        print("corner", math.floor(corner.X / -corner.Z * 100 + 0.5), math.floor(corner.Y / -corner.Z * 100 + 0.5))
        local p, on = cam:WorldToViewportPoint(Vector3.new(0, 0, -10))
        print("mid", p.X, p.Y, p.Z, on)
        p, on = cam:WorldToViewportPoint(Vector3.new(1, 0, -1))
        print("side", p.X, on)
        p, on = cam:WorldToViewportPoint(Vector3.new(0, 0, 10))
        print("behind", p.Z, on)
        print(pcall(function() workspace.Hidden:ViewportPointToRay(0, 0) end))
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    const auto has = [&](const std::string& text) {
        for (const auto& line : out.lines) {
            if (line.text.find(text) != std::string::npos) return true;
        }
        return false;
    };
    for (const auto& line : out.lines) UNSCOPED_INFO(line.text);
    CHECK(has("ray\t0\t-1\t1"));
    CHECK(has("corner\t200\t100"));
    CHECK(has("mid\t100\t50\t10\ttrue"));
    CHECK(has("side\t150\ttrue"));
    CHECK(has("behind\t-10\tfalse"));
    CHECK(has("not shown in a view"));
}

TEST_CASE("CAM7 a new place and a new project start with a DirectionalLight in Lighting", "[camera][project]") {
    SimRole role;
    TempDir dir;
    const auto suns = [](engine_core::DataModel& game) {
        int count = 0;
        for (InstanceId id : game.get_children(game.scene_service("Lighting"))) {
            count += dynamic_cast<engine_core::DirectionalLight*>(game.instance(id)) != nullptr ? 1 : 0;
        }
        return count;
    };
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        REQUIRE(suns(project.datamodel()) == 1);
        REQUIRE_FALSE(project.unsaved());
        project.save();
    }
    engine_core::Game game;
    engine_core::Project loaded = engine_core::Project::load(dir.path, game);
    REQUIRE(suns(game) == 1);
    engine_core::Project::reset_place(game);
    REQUIRE(suns(game) == 1);
    REQUIRE_FALSE(game.history().can_undo().first);
}
