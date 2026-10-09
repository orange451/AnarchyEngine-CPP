// The Brush instance: its faces, saving, undo, Stop, the Lua API, physics,
// raycasts, and the snapshot draws BrushVisuals makes.

#include "Brush.hpp"
#include "BrushVisuals.hpp"
#include "PhysicsWorld.hpp"
#include "Project.hpp"
#include "PropertyBag.hpp"
#include "SnapshotPump.hpp"
#include "physics_rig.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using namespace engine_core;
using namespace physics_rig;

namespace {

Brush& add_brush(Game& game) {
    Brush& brush = game.create<Brush>();
    game.set_parent(brush.id(), workspace_of(game));
    return brush;
}

bool has_text(const ScriptRuntime::OutputBatch& out, const std::string& text) {
    for (const auto& line : out.lines) {
        if (line.text.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("BI1 a new Brush is an anchored 4-unit box", "[brush]") {
    SimRole role;
    Game game;
    Brush& brush = add_brush(game);
    REQUIRE(brush.anchored());
    REQUIRE(brush.faces().size() == 6);
    REQUIRE(brush.shape().vertices.size() == 8);
    REQUIRE(brush.shape().min.x == -2.0);
    REQUIRE(brush.shape().max.y == 2.0);
    REQUIRE(brush.can_collide());
}

TEST_CASE("BI2 a refused face list changes nothing", "[brush]") {
    SimRole role;
    Game game;
    Brush& brush = add_brush(game);
    const std::string before = brush.faces_json();
    std::vector<brush::Face> faces = brush.faces();
    faces.pop_back();
    const auto error = brush.set_faces(faces);
    REQUIRE(error);
    REQUIRE(*error == "the faces do not close a solid");
    REQUIRE(brush.faces_json() == before);
}

TEST_CASE("BI3 faces round-trip through JSON exactly", "[brush]") {
    std::vector<brush::Face> faces = brush::make_cylinder({3.0, 5.0, 7.0}, 7);
    faces[2].material = "0123456789abcdef0123456789abcdef";
    faces[3].offset_u = 0.25;
    faces[3].scale_v = 2.0;
    faces[4].rotation = 33.0;
    const std::string json = brush::faces_to_json(faces);
    std::string error;
    const auto back = brush::faces_from_json(json, error);
    REQUIRE(back);
    REQUIRE(back->size() == faces.size());
    for (std::size_t i = 0; i < faces.size(); ++i) {
        REQUIRE((*back)[i] == faces[i]);
    }
    // A plain box writes no axes, offsets, scales, or rotations.
    const std::string box = brush::faces_to_json(brush::make_box({4, 4, 4}));
    REQUIRE(box.find("\"u\"") == std::string::npos);
    REQUIRE(box.find("\"s\"") == std::string::npos);
}

TEST_CASE("BI4 an edit is one undo step and Stop restores the faces", "[brush]") {
    SimRole role;
    Game game;
    Brush& brush = add_brush(game);
    const std::string before = brush.faces_json();
    begin_step(game, "Grow");
    REQUIRE_FALSE(brush.apply(brush::move_face(brush.faces(), 0, 2.0)));
    end_step(game);
    const std::string after = brush.faces_json();
    REQUIRE(after != before);
    REQUIRE(game.history().can_undo().second == "Grow");
    game.history().undo();
    REQUIRE(brush.faces_json() == before);
    game.history().redo();
    REQUIRE(brush.faces_json() == after);

    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(brush.apply(brush::expand(brush.faces(), 1.0)));
    REQUIRE(brush.faces_json() != after);
    game.stop_simulation();
    auto* restored = dynamic_cast<Brush*>(game.instance(brush.id()));
    REQUIRE(restored != nullptr);
    REQUIRE(restored->faces_json() == after);
}

TEST_CASE("BI5 a project saves and loads a Brush's faces", "[brush]") {
    SimRole role;
    TempDir dir;
    std::string faces;
    {
        Game game;
        Project project = Project::create(dir.path, game);
        Brush& brush = add_brush(game);
        REQUIRE_FALSE(brush.set_faces(brush::make_cylinder({6, 2, 6}, 12)));
        faces = brush.faces_json();
        project.save();
    }
    Project loaded = Project::load(dir.path);
    const Brush* found = nullptr;
    for (InstanceId id = loaded.datamodel().first_child(workspace_of(loaded.datamodel())); id != 0;
         id = loaded.datamodel().next_sibling(id)) {
        if (const auto* brush = dynamic_cast<const Brush*>(loaded.datamodel().instance(id))) {
            found = brush;
        }
    }
    for (InstanceId id = loaded.datamodel().first_child(workspace_of(loaded.datamodel())); id != 0; id = loaded.datamodel().next_sibling(id)) { UNSCOPED_INFO(loaded.datamodel().instance(id)->class_name()); }
    REQUIRE(found != nullptr);
    REQUIRE(found->faces_json() == faces);
    REQUIRE(found->faces().size() == 14);
}

TEST_CASE("BL1 Lua makes and edits a Brush", "[brush]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local b = Instance.new("Brush", workspace)
        print("faces", #b:GetFaces())
        b:MakeBox(Vector3.new(2, 4, 6))
        local lo, hi = b:GetBounds()
        print("bounds", lo.X, hi.Y, hi.Z)
        b:MoveFace(1, 1)
        print("moved", #b:GetFaces())
        b:Clip(BrushFace.fromPlane(Vector3.new(1, 1, 0), Vector3.new(0, 0, 0)))
        print("clipped", #b:GetFaces(), b:ContainsPoint(Vector3.new(0.5, 0.5, 0)))
        local f = b:GetFace(1)
        local g = f:With({Rotation = 45, Offset = Vector2.new(0.5, 0)})
        b:SetFace(1, g)
        print("rot", b:GetFace(1).Rotation, b:GetFace(1).Offset.X)
        print(pcall(function() b:MoveFace(99, 1) end))
        print(pcall(function() b.Faces = "[]" end))
        print(pcall(function() b:SetFaces({BrushFace.new(Vector3.zero, Vector3.xAxis, Vector3.yAxis)}) end))
        print(typeof(f), f == b:GetFaces()[1] or f ~= nil)
    )");
    rig.frames(1);
    const auto out = rig.runtime.drain_output();
    REQUIRE(has_text(out, "faces\t6"));
    REQUIRE(has_text(out, "bounds\t-1\t2\t3"));
    REQUIRE(has_text(out, "moved\t6"));
    REQUIRE(has_text(out, "clipped\t6\tfalse"));
    REQUIRE(has_text(out, "rot\t45\t0.5"));
    REQUIRE(has_text(out, "out of range"));
    REQUIRE(has_text(out, "cannot set Faces"));
    REQUIRE(has_text(out, "the faces do not close a solid"));
    REQUIRE(has_text(out, "BrushFace\ttrue"));
}

TEST_CASE("BL2 Changed fires for Faces", "[brush]") {
    ScriptRig rig;
    add_script(rig.game, "T", R"(
        local b = Instance.new("Brush", workspace)
        local count = 0
        b.Changed:Connect(function(name) if name == "Faces" then count += 1 end end)
        b:Expand(1)
        task.wait()
        task.wait()
        _G.ok = count == 1
    )");
    rig.game.start_simulation();
    rig.frames(4, 0.05);
    bool ok = false;
    REQUIRE(rig.runtime.global_boolean("ok", ok));
    REQUIRE(ok);
}

TEST_CASE("BP1 an anchored Brush holds things up and rays report its face", "[brush]") {
    PhysicsRig rig;
    Brush& floor = rig.game.create<Brush>();
    REQUIRE_FALSE(floor.set_faces(brush::make_box({40, 2, 40})));
    rig.game.set_parent(floor.id(), workspace_of(rig.game));
    PhysicsObject& box = rig.body(at(0.f, 5.f, 0.f), Vec3{1.f, 1.f, 1.f}, false);
    rig.play();
    rig.seconds(2.0);
    REQUIRE(near(y_of(box.transform()), 1.5f, 0.05f));

    const auto hit = rig.physics.raycast(rig.game, Vec3{3.f, 10.f, 2.f}, Vec3{0.f, -20.f, 0.f},
                                         RayFilter{true, {floor.id()}});
    REQUIRE(hit);
    REQUIRE(hit->instance == floor.id());
    REQUIRE(hit->face >= 0);
    const brush::Plane plane = floor.shape().planes[static_cast<std::size_t>(hit->face)];
    REQUIRE(plane.normal.y > 0.99);
}

TEST_CASE("BP2 CanCollide false lets rays through", "[brush]") {
    PhysicsRig rig;
    Brush& brush = rig.game.create<Brush>();
    rig.game.set_parent(brush.id(), workspace_of(rig.game));
    REQUIRE(rig.physics.raycast(rig.game, Vec3{0.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, RayFilter{}));
    REQUIRE_FALSE(brush.set_can_collide(false));
    REQUIRE_FALSE(rig.physics.raycast(rig.game, Vec3{0.f, 10.f, 0.f}, Vec3{0.f, -20.f, 0.f}, RayFilter{}));
}

TEST_CASE("BP3 an unanchored Brush falls, and a big one splits into pieces", "[brush]") {
    PhysicsRig rig;
    rig.floor();
    Brush& ball = rig.game.create<Brush>();
    REQUIRE_FALSE(ball.set_faces(brush::make_sphere({2, 2, 2}, 3)));
    REQUIRE_FALSE(ball.set_transform(at(0.f, 5.f, 0.f)));
    rig.game.set_parent(ball.id(), workspace_of(rig.game));
    ball.set_anchored(false);
    rig.play();
    rig.seconds(3.0);
    REQUIRE(y_of(ball.transform()) < 2.f);
    REQUIRE(rig.physics.shape_count(ball.id()) > 1);
    REQUIRE(near(static_cast<float>(rig.physics.body_mass(ball.id())), 1.f, 1e-3f));
}

TEST_CASE("BR1 anchored brushes bake into cells; only edited cells rebake", "[brush]") {
    SimRole role;
    Game game;
    std::vector<Brush*> brushes;
    for (int i = 0; i < 50; ++i) {
        Brush& brush = add_brush(game);
        REQUIRE_FALSE(brush.set_transform(at(static_cast<float>(i % 10) * 8.f, 0.f, static_cast<float>(i / 10) * 100.f)));
        brushes.push_back(&brush);
    }
    BrushVisuals visuals;
    std::vector<VisualBrushDraw> draws;
    visuals.update(game, draws);
    const std::uint64_t first = visuals.bakes();
    REQUIRE(visuals.cell_count() <= 10);
    REQUIRE(draws.size() == visuals.cell_count());  // one Material: one draw per cell
    REQUIRE(std::count_if(draws.begin(), draws.end(), [](const VisualBrushDraw& d) { return d.casts_shadow; }) ==
            static_cast<long>(draws.size()));

    visuals.update(game, draws);
    REQUIRE(visuals.bakes() == first);

    REQUIRE_FALSE(brushes[0]->apply(brush::expand(brushes[0]->faces(), 1.0)));
    visuals.update(game, draws);
    REQUIRE(visuals.bakes() == first + 1);

    // Unanchored or transparent: drawn alone, in its own space.
    REQUIRE_FALSE(brushes[1]->set_transparency(0.5));
    visuals.update(game, draws);
    const auto alone = std::find_if(draws.begin(), draws.end(),
                                    [&](const VisualBrushDraw& d) { return d.owner == brushes[1]->id(); });
    REQUIRE(alone != draws.end());
    REQUIRE(alone->transparency == 0.5f);
}
