// A Mesh's shape methods: the shapes themselves, and AddBox and the rest
// writing the Mesh's AMESH file from the console while the place is stopped.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "MeshShapes.hpp"
#include "Project.hpp"
#include "ScriptRuntime.hpp"
#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using anarchy::amesh::Data;
using engine_core::Vec3;

struct Box3 {
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
};

Box3 bounds(const Data& data) {
    Box3 box;
    for (const auto& v : data.vertices) {
        for (int axis = 0; axis < 3; ++axis) {
            box.lo[axis] = std::min(box.lo[axis], v.p[axis]);
            box.hi[axis] = std::max(box.hi[axis], v.p[axis]);
        }
    }
    return box;
}

bool near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

// Every triangle is CCW as seen from the side its vertex normals face, every
// normal is unit length, and, for a shape that is convex about center, faces out.
void require_faces_out(const Data& data, const float center[3], bool convex) {
    REQUIRE(data.indices.size() % 3 == 0);
    REQUIRE_FALSE(data.indices.empty());
    for (const auto& v : data.vertices) {
        REQUIRE(near(std::sqrt(v.n[0] * v.n[0] + v.n[1] * v.n[1] + v.n[2] * v.n[2]), 1.f, 1e-3f));
    }
    for (std::size_t t = 0; t < data.indices.size(); t += 3) {
        const float* a = data.vertices[data.indices[t]].p;
        const float* b = data.vertices[data.indices[t + 1]].p;
        const float* c = data.vertices[data.indices[t + 2]].p;
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float face[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                               e1[0] * e2[1] - e1[1] * e2[0]};
        float along_normals = 0.f;
        for (int k = 0; k < 3; ++k) {
            const float* n = data.vertices[data.indices[t + k]].n;
            along_normals += face[0] * n[0] + face[1] * n[1] + face[2] * n[2];
        }
        REQUIRE(along_normals > 0.f);
        if (convex) {
            const float mid[3] = {(a[0] + b[0] + c[0]) / 3.f - center[0], (a[1] + b[1] + c[1]) / 3.f - center[1],
                                  (a[2] + b[2] + c[2]) / 3.f - center[2]};
            REQUIRE(face[0] * mid[0] + face[1] * mid[1] + face[2] * mid[2] > 0.f);
        }
    }
    // What a shape makes is a mesh AMESH can hold.
    const Data back = anarchy::amesh::read(anarchy::amesh::write(data));
    REQUIRE(back.vertices.size() == data.vertices.size());
}

// A game with a project folder of its own, for the console to build Meshes in.
struct ShapeRig : ScriptRig {
    std::filesystem::path resources;

    ShapeRig() {
        resources = std::filesystem::temp_directory_path() / "anarchy-mesh-shapes-test";
        std::filesystem::remove_all(resources);
        std::filesystem::create_directories(resources);
        game.set_resources_root(resources);
    }
    ~ShapeRig() {
        std::error_code ignored;
        std::filesystem::remove_all(resources, ignored);
    }

    engine_core::Mesh& mesh(const char* name) {
        engine_core::Mesh& made = game.create<engine_core::Mesh>();
        game.set_name(made.id(), name);
        game.set_parent(made.id(), game.service("Meshes"));
        return made;
    }

    // Runs source on the command line and returns what it printed and any error, one line each.
    std::string run(const std::string& source) {
        runtime.drain_output();
        runtime.run_chunk(source);
        std::string out;
        for (const auto& line : runtime.drain_output().lines) {
            out += line.text;
        }
        return out;
    }

    Data file_of(const engine_core::Mesh& mesh) {
        const std::filesystem::path file = resources / std::filesystem::u8path(mesh.path());
        std::ifstream in(file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return anarchy::amesh::read(
            anarchy::amesh::ByteSpan(reinterpret_cast<const std::byte*>(text.data()), text.size()));
    }
};

}  // namespace

TEST_CASE("each shape is closed toward the outside and the size asked for", "[shapes]") {
    const float origin[3] = {0.f, 0.f, 0.f};
    Data box;
    engine_core::add_box(box, Vec3{2.f, 4.f, 6.f}, Vec3{});
    REQUIRE(box.vertices.size() == 24);
    REQUIRE(box.indices.size() == 36);
    require_faces_out(box, origin, true);
    REQUIRE(near(bounds(box).hi[1], 2.f));
    REQUIRE(near(bounds(box).lo[2], -3.f));

    Data sphere;
    engine_core::add_sphere(sphere, 1.5f, 24, Vec3{});
    require_faces_out(sphere, origin, true);
    REQUIRE(near(bounds(sphere).hi[1], 1.5f));
    REQUIRE(near(bounds(sphere).lo[0], -1.5f, 1e-3f));

    Data cylinder;
    engine_core::add_cylinder(cylinder, 1.f, 4.f, 16, true, Vec3{});
    require_faces_out(cylinder, origin, true);
    REQUIRE(near(bounds(cylinder).hi[1], 2.f));
    REQUIRE(near(bounds(cylinder).lo[1], -2.f));

    Data cone;
    engine_core::add_cone(cone, 1.f, 2.f, 16, true, Vec3{});
    require_faces_out(cone, origin, true);
    REQUIRE(near(bounds(cone).hi[1], 1.f));

    Data plane;
    engine_core::add_plane(plane, 3.f, 2.f, Vec3{});
    const float below[3] = {0.f, -1.f, 0.f};
    require_faces_out(plane, below, true);
    REQUIRE(near(bounds(plane).hi[0], 1.5f));
}

TEST_CASE("an uncapped cylinder is open, and segments are held to 3 and up", "[shapes]") {
    Data open;
    engine_core::add_cylinder(open, 1.f, 1.f, 8, false, Vec3{});
    REQUIRE(open.indices.size() / 3 == 16);  // the side only
    // AddCylinder(1, 4, 2, true): two segments make no cylinder, so it has three.
    Data three;
    engine_core::add_cylinder(three, 1.f, 4.f, 2, true, Vec3{});
    REQUIRE(three.indices.size() / 3 == 6 + 3 + 3);
}

TEST_CASE("shapes land where they are placed and add to what is there", "[shapes]") {
    Data data;
    engine_core::add_box(data, Vec3{1.f, 1.f, 1.f}, Vec3{});
    engine_core::add_sphere(data, 0.5f, 12, Vec3{10.f, 0.f, 0.f});
    REQUIRE(data.vertices.size() > 24);
    REQUIRE(near(bounds(data).hi[0], 10.5f, 1e-3f));
    REQUIRE(near(bounds(data).lo[0], -0.5f));
}

TEST_CASE("the teapot stands size tall on its position, spout toward +X", "[shapes]") {
    Data teapot;
    engine_core::add_teapot(teapot, 2.f, Vec3{1.f, 3.f, 0.f});
    const float inside[3] = {1.f, 4.f, 0.f};
    require_faces_out(teapot, inside, false);
    const Box3 box = bounds(teapot);
    REQUIRE(near(box.lo[1], 3.f));
    REQUIRE(near(box.hi[1], 5.f, 1e-3f));
    // The spout reaches further from the middle than the handle.
    REQUIRE(box.hi[0] - 1.f > 1.f - box.lo[0]);
    REQUIRE(teapot.indices.size() / 3 > 1000);
}

TEST_CASE("the console builds a Mesh's file, and Clear empties it", "[shapes]") {
    ShapeRig rig;
    engine_core::Mesh& cube = rig.mesh("Cube");
    REQUIRE(cube.path().empty());
    const std::string printed = rig.run(R"(
        local m = game.Assets.Meshes.Cube
        m:AddBox(Vector3.new(1, 2, 3))
        m:AddCylinder(1, 4, 2, true, Vector3.new(0, 5, 0))
        print(m.Path)
    )");
    INFO(printed);
    // A Mesh with no Path gets one of its own under meshes/.
    const std::string expected = "meshes/Cube." + rig.game.guid(cube.id()) + ".amesh";
    REQUIRE(cube.path() == expected);
    REQUIRE(printed == expected + "\n");
    const Data built = rig.file_of(cube);
    REQUIRE(built.indices.size() / 3 == 12 + 12);
    REQUIRE(near(bounds(built).hi[1], 7.f));

    rig.run("game.Assets.Meshes.Cube:AddTeapot(1)");
    REQUIRE(rig.file_of(cube).vertices.size() > built.vertices.size());

    rig.run("game.Assets.Meshes.Cube:Clear()");
    REQUIRE(rig.file_of(cube).vertices.empty());
    REQUIRE(cube.path() == expected);
}

TEST_CASE("undo puts back the Path a shape set, and the file stays", "[shapes]") {
    ShapeRig rig;
    engine_core::Mesh& cube = rig.mesh("Cube");
    begin_step(rig.game);
    rig.run("game.Assets.Meshes.Cube:AddSphere(1)");
    end_step(rig.game);
    const std::string path = cube.path();
    REQUIRE_FALSE(path.empty());
    rig.game.history().undo();
    REQUIRE(cube.path().empty());
    REQUIRE(std::filesystem::exists(rig.resources / std::filesystem::u8path(path)));
}

TEST_CASE("shape methods say why they wrote nothing", "[shapes]") {
    ShapeRig rig;
    engine_core::Mesh& cube = rig.mesh("Cube");

    CHECK(rig.run("game.Assets.Meshes.Cube:AddSphere(0)").find("radius must be a number above 0") !=
          std::string::npos);
    CHECK(rig.run("game.Assets.Meshes.Cube:AddBox(Vector3.new(1, 0, 1))").find("size must be a Vector3") !=
          std::string::npos);
    CHECK(rig.run("game.Assets.Meshes.Cube:AddPlane(1, 1, 'up')").find("position must be a Vector3") !=
          std::string::npos);
    REQUIRE(cube.path().empty());

    // A Path naming a file that is not AMESH: the file is left as it is.
    REQUIRE_FALSE(cube.set_path("meshes/notes.amesh"));
    std::filesystem::create_directories(rig.resources / "meshes");
    std::ofstream(rig.resources / "meshes" / "notes.amesh") << "not a mesh";
    CHECK(rig.run("game.Assets.Meshes.Cube:AddBox(Vector3.new(1, 1, 1))").find("is not an AMESH file") !=
          std::string::npos);
    std::ifstream kept(rig.resources / "meshes" / "notes.amesh");
    std::string text;
    std::getline(kept, text);
    REQUIRE(text == "not a mesh");

    // A file with LODs: new triangles would fall outside them.
    Data lodded;
    engine_core::add_box(lodded, Vec3{1.f, 1.f, 1.f}, Vec3{});
    lodded.lods = {{0, 6}, {6, 6}};
    const std::vector<std::byte> lodded_bytes = anarchy::amesh::write(lodded);
    std::ofstream(rig.resources / "meshes" / "lods.amesh", std::ios::binary)
        .write(reinterpret_cast<const char*>(lodded_bytes.data()), static_cast<std::streamsize>(lodded_bytes.size()));
    REQUIRE_FALSE(cube.set_path("meshes/lods.amesh"));
    CHECK(rig.run("game.Assets.Meshes.Cube:AddBox(Vector3.new(1, 1, 1))").find("has LODs") != std::string::npos);
    REQUIRE(rig.file_of(cube).lods.size() == 2);

    // No project: nowhere to write.
    rig.game.set_resources_root({});
    CHECK(rig.run("game.Assets.Meshes.Cube:Clear()").find("Open or save a project first") != std::string::npos);

    // During play, a file with LODs still cannot be added to.
    rig.game.set_resources_root(rig.resources);
    rig.game.start_simulation();
    REQUIRE(cube.edit_geometry([](Data&) {}).value_or("").find("has LODs") != std::string::npos);
    REQUIRE(cube.session_geometry().data == nullptr);
    rig.game.stop_simulation();
}

TEST_CASE("a project tells its game where its resources are, and a new place forgets", "[shapes]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(game.resources_root().empty());
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "anarchy-mesh-shapes-project";
    std::filesystem::remove_all(root);
    {
        engine_core::Project project = engine_core::Project::create(root, game);
        REQUIRE(game.resources_root() == root / "resources");
        REQUIRE(project.resources_root() == root / "resources");
    }
    engine_core::Game reopened;
    engine_core::Project loaded = engine_core::Project::load(root, reopened);
    REQUIRE(reopened.resources_root() == root / "resources");
    engine_core::Project::reset_place(reopened);
    REQUIRE(reopened.resources_root().empty());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

namespace {

// The meshes the row for object draws in the pump's newest frame.
std::vector<engine_core::VisualMesh> drawn(const engine_core::SnapshotPump& pump, engine_core::InstanceId object) {
    const engine_core::VisualInstance* row = pump.find(object);
    REQUIRE(row != nullptr);
    REQUIRE(row->prefab != 0);
    REQUIRE(row->prefab < pump.front().prefabs.size());
    return pump.front().prefabs[row->prefab].meshes;
}

void frame(engine_core::SnapshotPump& pump, engine_core::DataModel& game) {
    pump.prepare_copy(game);
    pump.publish();
}

std::size_t triangles(const Data& data) { return data.indices.size() / 3; }

}  // namespace

TEST_CASE("during play, shapes change the session's copy and leave the file alone", "[shapes]") {
    ShapeRig rig;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    engine_core::Mesh& cube = rig.mesh("Cube");
    rig.run(R"(
        local mesh = game.Assets.Meshes.Cube
        mesh:AddBox(Vector3.new(1, 1, 1))
        local prefab = Instance.new("Prefab", game.Assets.Prefabs)
        local model = Instance.new("Model", prefab)
        model.Mesh = mesh
        local object = Instance.new("GameObject", workspace)
        object.Name = "Thing"
        object.Prefab = prefab
    )");
    const std::string path = cube.path();
    REQUIRE_FALSE(path.empty());
    const engine_core::InstanceId thing = rig.game.find_first_child(workspace_of(rig.game), "Thing");
    REQUIRE(thing != 0);
    frame(pump, rig.game);
    REQUIRE(drawn(pump, thing).size() == 1);
    REQUIRE(drawn(pump, thing)[0].path == path);
    REQUIRE(drawn(pump, thing)[0].session == nullptr);

    // Play: the first edit starts from the file, and the file stays as it was.
    rig.game.start_simulation();
    INFO(rig.run("game.Assets.Meshes.Cube:AddSphere(0.5, 8, Vector3.new(3, 0, 0))"));
    REQUIRE(triangles(rig.file_of(cube)) == 12);
    REQUIRE(cube.path() == path);
    const engine_core::Mesh::SessionGeometry first = cube.session_geometry();
    REQUIRE(first.data != nullptr);
    REQUIRE(triangles(*first.data) > 12);
    frame(pump, rig.game);
    std::vector<engine_core::VisualMesh> meshes = drawn(pump, thing);
    REQUIRE(meshes.size() == 1);
    REQUIRE(meshes[0].session == first.data);
    REQUIRE(meshes[0].revision == first.revision);
    REQUIRE(meshes[0].mesh == cube.id());
    REQUIRE(meshes[0].path.empty());

    // Each edit is a new copy with a new revision; the frame still holding the old one keeps it.
    rig.run("game.Assets.Meshes.Cube:Clear()");
    const engine_core::Mesh::SessionGeometry cleared = cube.session_geometry();
    REQUIRE(cleared.data != nullptr);
    REQUIRE(cleared.data->vertices.empty());
    REQUIRE(cleared.revision != first.revision);
    REQUIRE(triangles(*meshes[0].session) > 12);
    REQUIRE(triangles(rig.file_of(cube)) == 12);

    // Stop drops the session's copy: the file draws again.
    rig.game.stop_simulation();
    REQUIRE(cube.session_geometry().data == nullptr);
    frame(pump, rig.game);
    REQUIRE(drawn(pump, thing)[0].session == nullptr);
    REQUIRE(drawn(pump, thing)[0].path == path);

    // A new session starts from the file, not from the last session's copy.
    rig.game.start_simulation();
    rig.run("game.Assets.Meshes.Cube:AddBox(Vector3.new(1, 1, 1), Vector3.new(0, 2, 0))");
    REQUIRE(triangles(*cube.session_geometry().data) == 24);
    rig.game.stop_simulation();
}

TEST_CASE("a Mesh made during play draws what its script builds, with no project", "[shapes]") {
    ShapeRig rig;
    rig.game.set_resources_root({});
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    rig.game.start_simulation();
    const std::string printed = rig.run(R"(
        local mesh = Instance.new("Mesh", game.Assets.Meshes)
        mesh:AddTeapot(1)
        mesh:AddCone(0.5, 1, nil, true, Vector3.new(2, 0, 0))
        local prefab = Instance.new("Prefab", game.Assets.Prefabs)
        Instance.new("Model", prefab).Mesh = mesh
        local object = Instance.new("GameObject", workspace)
        object.Name = "Made"
        object.Prefab = prefab
        print(mesh.Path == "")
    )");
    REQUIRE(printed == "true\n");
    const engine_core::InstanceId made = rig.game.find_first_child(workspace_of(rig.game), "Made");
    REQUIRE(made != 0);
    frame(pump, rig.game);
    const std::vector<engine_core::VisualMesh> meshes = drawn(pump, made);
    REQUIRE(meshes.size() == 1);
    REQUIRE(meshes[0].session != nullptr);
    REQUIRE(triangles(*meshes[0].session) > 1000);
    rig.game.stop_simulation();
}

TEST_CASE("a Script that rebuilds a Mesh every Heartbeat never writes its file", "[shapes]") {
    ShapeRig rig;
    engine_core::Mesh& cube = rig.mesh("Cube");
    rig.run("game.Assets.Meshes.Cube:AddBox(Vector3.new(1, 1, 1))");
    const std::filesystem::path file = rig.resources / std::filesystem::u8path(cube.path());
    const auto written = std::filesystem::last_write_time(file);
    add_script(rig.game, "Pulse", R"(
        local mesh = game.Assets.Meshes.Cube
        local t = 0
        game:GetService("RunService").Heartbeat:Connect(function(dt)
            t += dt
            mesh:Clear()
            mesh:AddSphere(0.5 + t, 8)
        end)
    )");
    for (int session = 0; session < 2; ++session) {
        rig.game.start_simulation();
        rig.frames(3);
        INFO(rig.runtime.last_error());
        const engine_core::Mesh::SessionGeometry pulsing = cube.session_geometry();
        REQUIRE(pulsing.data != nullptr);
        rig.frames(1);
        // Each frame's rebuild is a new copy.
        REQUIRE(cube.session_geometry().revision != pulsing.revision);
        rig.game.stop_simulation();
        // Stop ends the Script's connection: nothing edits the Mesh while stopped.
        rig.frames(2);
        REQUIRE(cube.session_geometry().data == nullptr);
    }
    // A bool, since Catch cannot print a file time's __int128 count on older libc++.
    const bool unwritten = std::filesystem::last_write_time(file) == written;
    REQUIRE(unwritten);
    REQUIRE(triangles(rig.file_of(cube)) == 12);
}

TEST_CASE("OriginOffset runs from a Mesh's origin to the middle of its box, and a Prefab's of its Models'", "[shapes]") {
    ShapeRig rig;
    engine_core::Mesh& low = rig.mesh("Low");
    engine_core::Mesh& high = rig.mesh("High");
    rig.run(R"(
        local prefab = Instance.new("Prefab", game.Assets.Prefabs)
        prefab.Name = "Pair"
        Instance.new("Model", prefab).Mesh = game.Assets.Meshes.Low
        -- A Model with no Mesh adds nothing.
        Instance.new("Model", prefab)
    )");
    const auto* pair = dynamic_cast<const engine_core::Prefab*>(
        rig.game.instance(rig.game.find_first_child(rig.game.service("Prefabs"), "Pair")));
    REQUIRE(pair != nullptr);
    const auto same = [](Vec3 a, Vec3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); };

    // Nothing to measure yet.
    REQUIRE(same(low.origin_offset(), Vec3{}));
    REQUIRE(same(pair->origin_offset(), Vec3{}));

    INFO(rig.run(R"(
        game.Assets.Meshes.Low:AddBox(Vector3.new(2, 2, 2), Vector3.new(0, 1, 0))
        game.Assets.Meshes.High:AddBox(Vector3.new(2, 2, 2), Vector3.new(4, 0, 0))
    )"));
    REQUIRE(same(low.origin_offset(), Vec3{0.f, 1.f, 0.f}));
    REQUIRE(same(high.origin_offset(), Vec3{4.f, 0.f, 0.f}));
    REQUIRE(same(pair->origin_offset(), Vec3{0.f, 1.f, 0.f}));

    // Another Model widens the box: x -1 to 5, y -1 to 2.
    rig.run(R"(Instance.new("Model", game.Assets.Prefabs.Pair).Mesh = game.Assets.Meshes.High)");
    REQUIRE(same(pair->origin_offset(), Vec3{2.f, 0.5f, 0.f}));

    // The file is read again when it changes on disk. Its time is moved on, so
    // a file system that keeps whole seconds still sees the change.
    rig.run("game.Assets.Meshes.Low:AddBox(Vector3.new(2, 2, 2), Vector3.new(0, 5, 0))");
    const std::filesystem::path file = rig.resources / std::filesystem::u8path(low.path());
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(5));
    REQUIRE(same(low.origin_offset(), Vec3{0.f, 3.f, 0.f}));
    REQUIRE(same(pair->origin_offset(), Vec3{2.f, 2.5f, 0.f}));

    // Scripts read it and cannot write it.
    REQUIRE(rig.run(R"(
        local mesh = game.Assets.Meshes.Low
        local prefab = game.Assets.Prefabs.Pair
        print(mesh.OriginOffset == Vector3.new(0, 3, 0), prefab.OriginOffset == Vector3.new(2, 2.5, 0),
            pcall(function() mesh.OriginOffset = Vector3.new() end),
            (pcall(function() prefab.OriginOffset = Vector3.new() end)))
    )") == "true\ttrue\tfalse\tfalse\n");

    // During play it is the session's geometry.
    rig.game.start_simulation();
    rig.run("game.Assets.Meshes.Low:AddBox(Vector3.new(2, 2, 2), Vector3.new(0, -10, 0))");
    REQUIRE(same(low.origin_offset(), Vec3{0.f, -2.5f, 0.f}));
}
