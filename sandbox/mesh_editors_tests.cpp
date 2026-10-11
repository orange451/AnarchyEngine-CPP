// MeshEditors: one MeshEditor per Mesh, committed each step to the Mesh's
// live copy, written to its file when idle or let go, and ended when its
// Mesh, world, or file moves on (docs/superpowers/specs/2026-10-11-mesh-editor-design.md, section 2).

#include "support.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "MeshEditors.hpp"
#include "MeshShapes.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using anarchy::amesh::Data;
using engine_core::Mesh;
using engine_core::MeshEditor;
using engine_core::Vec3;

void add_box(MeshEditor& editor) {
    editor.add_shape([](Data& data) { engine_core::add_box(data, Vec3{1.f, 1.f, 1.f}, Vec3{}); });
}

void write_bytes(const std::filesystem::path& file, const Data& data) {
    const std::vector<std::byte> bytes = anarchy::amesh::write(data);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

struct EditorRig : ScriptRig {
    std::filesystem::path resources;

    EditorRig() {
        resources = std::filesystem::temp_directory_path() / "anarchy-mesh-editors-test";
        std::filesystem::remove_all(resources);
        std::filesystem::create_directories(resources);
        game.set_resources_root(resources);
    }
    ~EditorRig() {
        std::error_code ignored;
        std::filesystem::remove_all(resources, ignored);
    }

    engine_core::MeshEditors& editors() { return game.mesh_editors(); }

    Mesh& mesh(const char* name) {
        Mesh& made = game.create<Mesh>();
        game.set_name(made.id(), name);
        game.set_parent(made.id(), game.service("Meshes"));
        return made;
    }

    std::shared_ptr<MeshEditor> open(Mesh& mesh) {
        std::string error;
        std::shared_ptr<MeshEditor> editor = editors().open(game, mesh, error);
        INFO(error);
        REQUIRE(editor != nullptr);
        return editor;
    }

    Data file_at(const std::string& path) {
        std::ifstream in(resources / std::filesystem::u8path(path), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return anarchy::amesh::read(
            anarchy::amesh::ByteSpan(reinterpret_cast<const std::byte*>(text.data()), text.size()));
    }
    Data file_of(const Mesh& mesh) { return file_at(mesh.path()); }
};

std::size_t live_triangles(const Mesh& mesh) {
    const Mesh::SessionGeometry live = mesh.session_geometry();
    return live.data != nullptr ? live.data->indices.size() / 3 : 0;
}

}  // namespace

TEST_CASE("R1 one editor per Mesh, and opening one stopped gives the Mesh its file", "[mesh-editors]") {
    EditorRig rig;
    Mesh& cube = rig.mesh("Cube");
    const std::shared_ptr<MeshEditor> first = rig.open(cube);
    REQUIRE(rig.open(cube) == first);
    REQUIRE(rig.editors().size() == 1);
    REQUIRE(rig.editors().usable(rig.game, *first));
    REQUIRE(cube.path() == "meshes/Cube." + rig.game.guid(cube.id()) + ".amesh");
    REQUIRE(rig.file_of(cube).vertices.empty());

    // A file with LODs is not opened: new triangles would fall outside them.
    Data lodded;
    engine_core::add_box(lodded, Vec3{1.f, 1.f, 1.f}, Vec3{});
    lodded.lods = {{0, 6}, {6, 6}};
    write_bytes(rig.resources / "meshes" / "lods.amesh", lodded);
    Mesh& lods = rig.mesh("Lods");
    REQUIRE_FALSE(lods.set_path("meshes/lods.amesh"));
    std::string error;
    REQUIRE(rig.editors().open(rig.game, lods, error) == nullptr);
    REQUIRE(error.find("has LODs") != std::string::npos);

    // With no project there is nowhere to write.
    rig.game.set_resources_root({});
    Mesh& loose = rig.mesh("Loose");
    error.clear();
    REQUIRE(rig.editors().open(rig.game, loose, error) == nullptr);
    REQUIRE(error.find("Open or save a project first") != std::string::npos);
}

TEST_CASE("R2 stopped, an edit draws at the next step, and reaches the file once idle", "[mesh-editors]") {
    EditorRig rig;
    Mesh& cube = rig.mesh("Cube");
    const std::shared_ptr<MeshEditor> editor = rig.open(cube);
    add_box(*editor);
    REQUIRE(cube.session_geometry().data == nullptr);
    rig.frames(1);
    REQUIRE(live_triangles(cube) == 12);
    REQUIRE(rig.file_of(cube).vertices.empty());
    // Half a second without edits: not yet.
    rig.frames(30);
    REQUIRE(rig.file_of(cube).vertices.empty());
    // Past a second: written, and still drawn from the live copy for a while.
    rig.frames(40);
    REQUIRE(rig.file_of(cube).vertices.size() == 24);
    REQUIRE(live_triangles(cube) == 12);
    // Two seconds after the write the file draws instead.
    rig.frames(130);
    REQUIRE(cube.session_geometry().data == nullptr);
    REQUIRE(rig.editors().usable(rig.game, *editor));

    // More edits draw again.
    editor->add_vertex(Vec3{});
    rig.frames(1);
    REQUIRE(cube.session_geometry().data != nullptr);
    REQUIRE(cube.session_geometry().data->vertices.size() == 25);
}

TEST_CASE("R3 an editor nobody holds is written at the next step and forgotten", "[mesh-editors]") {
    EditorRig rig;
    Mesh& cube = rig.mesh("Cube");
    add_box(*rig.open(cube));
    rig.frames(1);
    REQUIRE(rig.file_of(cube).vertices.size() == 24);
    rig.frames(130);
    REQUIRE(rig.editors().size() == 0);
    REQUIRE(cube.session_geometry().data == nullptr);
    // A new one starts from the file.
    REQUIRE(rig.open(cube)->vertices().size() == 24);
}

TEST_CASE("R4 a Path change or another writer ends an editor, and nothing is written", "[mesh-editors]") {
    EditorRig rig;
    Mesh& cube = rig.mesh("Cube");
    begin_step(rig.game);
    const std::shared_ptr<MeshEditor> editor = rig.open(cube);
    end_step(rig.game);
    const std::string path = cube.path();
    REQUIRE_FALSE(path.empty());
    add_box(*editor);
    rig.frames(1);
    REQUIRE(live_triangles(cube) == 12);

    // Undo takes back the Path the editor gave.
    rig.game.history().undo();
    REQUIRE(cube.path().empty());
    rig.frames(1);
    REQUIRE_FALSE(rig.editors().usable(rig.game, *editor));
    REQUIRE(cube.session_geometry().data == nullptr);
    rig.frames(100);
    REQUIRE(rig.file_at(path).vertices.empty());
    REQUIRE(rig.editors().size() == 0);

    // Something else writes the file.
    Mesh& other = rig.mesh("Other");
    const std::shared_ptr<MeshEditor> second = rig.open(other);
    add_box(*second);
    rig.frames(1);
    Data sphere;
    engine_core::add_sphere(sphere, 1.f, 8, Vec3{});
    const std::filesystem::path file = rig.resources / std::filesystem::u8path(other.path());
    write_bytes(file, sphere);
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(5));
    rig.frames(1);
    REQUIRE_FALSE(rig.editors().usable(rig.game, *second));
    REQUIRE(other.session_geometry().data == nullptr);
    rig.frames(100);
    REQUIRE(rig.file_of(other).vertices.size() == sphere.vertices.size());
}

TEST_CASE("R5 playing, edits change the session's copy, never the file, and Stop ends them", "[mesh-editors]") {
    EditorRig rig;
    Mesh& cube = rig.mesh("Cube");
    std::shared_ptr<MeshEditor> setup = rig.open(cube);
    add_box(*setup);
    rig.editors().write_all(rig.game, true);
    REQUIRE_FALSE(rig.editors().usable(rig.game, *setup));
    setup.reset();
    REQUIRE(rig.file_of(cube).vertices.size() == 24);

    rig.game.start_simulation();
    const std::shared_ptr<MeshEditor> playing = rig.open(cube);
    REQUIRE(playing->vertices().size() == 24);
    playing->clear();
    rig.frames(1);
    REQUIRE(cube.session_geometry().data != nullptr);
    REQUIRE(cube.session_geometry().data->vertices.empty());
    rig.frames(200);
    REQUIRE(rig.file_of(cube).vertices.size() == 24);

    rig.game.stop_simulation();
    REQUIRE_FALSE(rig.editors().usable(rig.game, *playing));
    REQUIRE(cube.session_geometry().data == nullptr);
    rig.frames(1);
    REQUIRE(rig.editors().size() == 0);

    // The next session starts from the file again.
    rig.game.start_simulation();
    const std::shared_ptr<MeshEditor> again = rig.open(cube);
    REQUIRE(again != playing);
    REQUIRE(again->vertices().size() == 24);
    rig.game.stop_simulation();
}

TEST_CASE("R6 a destroyed Mesh ends its editor", "[mesh-editors]") {
    EditorRig rig;
    Mesh& cube = rig.mesh("Cube");
    const std::shared_ptr<MeshEditor> editor = rig.open(cube);
    add_box(*editor);
    rig.game.destroy(cube.id());
    REQUIRE_FALSE(rig.editors().usable(rig.game, *editor));
    rig.frames(1);
    REQUIRE(rig.editors().size() == 0);
}

TEST_CASE("H1 Play writes a stopped editor's edits first, and ends it", "[mesh-editors]") {
    EditorRig rig;
    Mesh& cube = rig.mesh("Cube");
    const std::shared_ptr<MeshEditor> editor = rig.open(cube);
    add_box(*editor);
    // Not even committed yet: Play still writes it.
    rig.game.start_simulation();
    REQUIRE(rig.file_of(cube).vertices.size() == 24);
    REQUIRE_FALSE(rig.editors().usable(rig.game, *editor));
    REQUIRE(rig.editors().size() == 0);
    REQUIRE(cube.session_geometry().data == nullptr);
    rig.game.stop_simulation();
    REQUIRE(cube.session_geometry().data == nullptr);
}

TEST_CASE("H2 a project save writes pending edits, and the editor carries on", "[mesh-editors]") {
    ScriptRig rig;
    TempDir dir;
    engine_core::Project project = engine_core::Project::create(dir.path, rig.game);
    Mesh& cube = rig.game.create<Mesh>();
    rig.game.set_name(cube.id(), "Cube");
    rig.game.set_parent(cube.id(), rig.game.service("Meshes"));
    std::string error;
    const std::shared_ptr<MeshEditor> editor = rig.game.mesh_editors().open(rig.game, cube, error);
    INFO(error);
    REQUIRE(editor != nullptr);
    add_box(*editor);
    project.save();
    const std::filesystem::path file = rig.game.resources_root() / std::filesystem::u8path(cube.path());
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const Data saved =
        anarchy::amesh::read(anarchy::amesh::ByteSpan(reinterpret_cast<const std::byte*>(text.data()), text.size()));
    REQUIRE(saved.vertices.size() == 24);
    REQUIRE(rig.game.mesh_editors().usable(rig.game, *editor));
    // Still drawn from the live copy until the file has been seen.
    REQUIRE(cube.session_geometry().data != nullptr);
}

TEST_CASE("R7 a Path changed since an editor opened ends it at the next open or save, and nothing goes to either file", "[mesh-editors]") {
    EditorRig rig;
    Mesh& cube = rig.mesh("Cube");
    const std::shared_ptr<MeshEditor> first = rig.open(cube);
    add_box(*first);
    rig.frames(1);
    const std::string old_path = cube.path();

    // A generator run again within the same few seconds, on a new Path.
    REQUIRE_FALSE(cube.set_path("meshes/gen2.amesh"));
    const std::shared_ptr<MeshEditor> second = rig.open(cube);
    REQUIRE(second != first);
    REQUIRE_FALSE(rig.editors().usable(rig.game, *first));
    REQUIRE(second->vertices().empty());
    second->add_shape([](Data& data) { engine_core::add_sphere(data, 1.f, 8, Vec3{}); });
    rig.editors().write_all(rig.game, true);
    Data sphere;
    engine_core::add_sphere(sphere, 1.f, 8, Vec3{});
    REQUIRE(rig.file_at("meshes/gen2.amesh").vertices.size() == sphere.vertices.size());
    REQUIRE(rig.file_at(old_path).vertices.empty());

    // A save the same tick another Mesh is pointed at that file does not write over it.
    Mesh& other = rig.mesh("Other");
    const std::shared_ptr<MeshEditor> third = rig.open(other);
    add_box(*third);
    rig.frames(1);
    REQUIRE_FALSE(other.set_path("meshes/gen2.amesh"));
    rig.editors().write_all(rig.game, false);
    REQUIRE(rig.file_at("meshes/gen2.amesh").vertices.size() == sphere.vertices.size());
    REQUIRE_FALSE(rig.editors().usable(rig.game, *third));
    REQUIRE(other.session_geometry().data == nullptr);
}

TEST_CASE("R8 playing, a Path change ends the editor and the Mesh draws its new file", "[mesh-editors]") {
    EditorRig rig;
    Mesh& cube = rig.mesh("Cube");
    add_box(*rig.open(cube));
    Mesh& rock = rig.mesh("Rock");
    rig.open(rock)->add_shape([](Data& data) { engine_core::add_sphere(data, 1.f, 8, Vec3{}); });
    rig.editors().write_all(rig.game, true);

    rig.game.start_simulation();
    const std::shared_ptr<MeshEditor> editor = rig.open(cube);
    editor->clear();
    rig.frames(1);
    REQUIRE(cube.session_geometry().data != nullptr);
    REQUIRE_FALSE(cube.set_path(rock.path()));
    rig.frames(1);
    REQUIRE_FALSE(rig.editors().usable(rig.game, *editor));
    REQUIRE(cube.session_geometry().data == nullptr);
    rig.game.stop_simulation();
}
