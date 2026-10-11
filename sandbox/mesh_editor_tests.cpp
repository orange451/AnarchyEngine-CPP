// MeshEditor: a mesh's vertices, triangles, and bones under ids that survive
// removals, and what build() makes of them
// (docs/superpowers/specs/2026-10-11-mesh-editor-design.md, section 1).

#include "MeshEditor.hpp"
#include "MeshShapes.hpp"
#include "Skeleton.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace {

using engine_core::Matrix4;
using engine_core::MeshEditError;
using engine_core::MeshEditor;
using engine_core::Vec2;
using engine_core::Vec3;
using Id = MeshEditor::Id;
using anarchy::amesh::Data;

bool near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }
bool near_vec(Vec3 a, Vec3 b, float tolerance = 1e-4f) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

// The message a call is refused with, or "" when it is not.
template <typename Fn>
std::string refusal(Fn fn) {
    try {
        fn();
    } catch (const MeshEditError& refused) {
        return refused.what();
    }
    return {};
}

// Two triangles facing +Y over the unit square at y = 0: (a, b, c) and (b, d, c).
struct Square {
    MeshEditor editor;
    Id a, b, c, d, t1, t2;
    Square() {
        a = editor.add_vertex(Vec3{0.f, 0.f, 0.f});
        b = editor.add_vertex(Vec3{1.f, 0.f, 0.f});
        c = editor.add_vertex(Vec3{0.f, 0.f, -1.f});
        d = editor.add_vertex(Vec3{1.f, 0.f, -1.f});
        t1 = editor.add_triangle(a, b, c);
        t2 = editor.add_triangle(b, d, c);
    }
};

Vec3 normal_of(const Data& data, std::size_t vertex) {
    const float* n = data.vertices[vertex].n;
    return Vec3{n[0], n[1], n[2]};
}

}  // namespace

TEST_CASE("ME1 ids survive removals, and build compacts what is live in id order", "[mesh-editor]") {
    Square square;
    MeshEditor& editor = square.editor;
    REQUIRE(square.a == 0);
    REQUIRE(square.d == 3);
    editor.remove_vertex(square.a);
    REQUIRE(editor.vertices() == std::vector<Id>{square.b, square.c, square.d});
    // Removing a vertex removes the triangles that use it.
    REQUIRE(editor.triangles() == std::vector<Id>{square.t2});
    REQUIRE(editor.triangle_vertices(square.t2) == std::array<Id, 3>{square.b, square.d, square.c});
    REQUIRE(near_vec(editor.position(square.d), Vec3{1.f, 0.f, -1.f}));
    REQUIRE(refusal([&] { editor.position(square.a); }) == "no vertex 0");

    const Data built = editor.build();
    REQUIRE(built.vertices.size() == 3);
    // b, c, d become 0, 1, 2.
    REQUIRE(built.indices == std::vector<std::uint32_t>{0, 2, 1});
    REQUIRE(near(built.vertices[2].p[0], 1.f));

    // A new vertex never takes a removed one's id.
    REQUIRE(editor.add_vertex(Vec3{}) == 4);
}

TEST_CASE("ME2 removing a triangle keeps its vertices, and bad triangles are refused", "[mesh-editor]") {
    Square square;
    MeshEditor& editor = square.editor;
    editor.remove_triangle(square.t1);
    REQUIRE(editor.vertices().size() == 4);
    REQUIRE(editor.triangles() == std::vector<Id>{square.t2});
    REQUIRE(refusal([&] { editor.remove_triangle(square.t1); }) == "no triangle 0");
    REQUIRE(refusal([&] { editor.add_triangle(square.a, square.a, square.b); }) ==
            "a triangle needs three different vertices");
    REQUIRE(refusal([&] { editor.add_triangle(square.a, square.b, 99); }) == "no vertex 99");
    // Removing a vertex of an already removed triangle leaves the other one alone.
    editor.remove_vertex(square.a);
    REQUIRE(editor.triangles() == std::vector<Id>{square.t2});
}

TEST_CASE("ME3 a normal is computed from its triangles until one is set", "[mesh-editor]") {
    Square square;
    MeshEditor& editor = square.editor;
    REQUIRE(near_vec(editor.normal(square.b), Vec3{0.f, 1.f, 0.f}));
    const Id lone = editor.add_vertex(Vec3{5.f, 5.f, 5.f});
    REQUIRE(near_vec(editor.normal(lone), Vec3{0.f, 1.f, 0.f}));

    editor.set_normal(square.a, Vec3{0.f, 0.f, 2.f});
    REQUIRE(near_vec(editor.normal(square.a), Vec3{0.f, 0.f, 1.f}));
    Data built = editor.build();
    REQUIRE(near_vec(normal_of(built, 0), Vec3{0.f, 0.f, 1.f}));
    REQUIRE(near_vec(normal_of(built, 1), Vec3{0.f, 1.f, 0.f}));

    editor.set_normal(square.a, std::nullopt);
    REQUIRE(near_vec(editor.normal(square.a), Vec3{0.f, 1.f, 0.f}));
    built = editor.build();
    REQUIRE(near_vec(normal_of(built, 0), Vec3{0.f, 1.f, 0.f}));
    REQUIRE_FALSE(refusal([&] { editor.set_normal(square.a, Vec3{}); }).empty());
}

TEST_CASE("ME4 a shape returns its new vertices, and keeps the normals it made", "[mesh-editor]") {
    MeshEditor editor;
    editor.add_vertex(Vec3{});
    const std::vector<Id> box = editor.add_shape(
        [](Data& data) { engine_core::add_box(data, Vec3{2.f, 2.f, 2.f}, Vec3{0.f, 5.f, 0.f}); });
    REQUIRE(box.size() == 24);
    REQUIRE(box.front() == 1);
    REQUIRE(box.back() == 24);
    REQUIRE(editor.triangles().size() == 12);

    Data expected;
    engine_core::add_box(expected, Vec3{2.f, 2.f, 2.f}, Vec3{0.f, 5.f, 0.f});
    const Data built = editor.build();
    for (std::size_t i = 0; i < expected.vertices.size(); ++i) {
        REQUIRE(near_vec(normal_of(built, i + 1), normal_of(expected, i)));
        REQUIRE(near(built.vertices[i + 1].p[1], expected.vertices[i].p[1]));
    }
}

TEST_CASE("ME5 a mesh opened and built with no edits comes back the same", "[mesh-editor]") {
    Data data;
    engine_core::add_box(data, Vec3{1.f, 2.f, 3.f}, Vec3{});
    engine_core::add_sphere(data, 1.f, 12, Vec3{4.f, 0.f, 0.f});
    data.vertices[3].rgba[0] = 10;
    data.vertices[3].uv[0] = 0.25f;
    const Data built = MeshEditor(data).build();
    REQUIRE(built.vertices.size() == data.vertices.size());
    REQUIRE(built.indices == data.indices);
    for (std::size_t i = 0; i < data.vertices.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            REQUIRE(built.vertices[i].p[k] == data.vertices[i].p[k]);
            REQUIRE(near(built.vertices[i].n[k], data.vertices[i].n[k]));
        }
        REQUIRE(built.vertices[i].uv[0] == data.vertices[i].uv[0]);
        REQUIRE(built.vertices[i].rgba[0] == data.vertices[i].rgba[0]);
    }
    REQUIRE(built.bones.empty());
}

TEST_CASE("ME6 Clear removes everything, and ids carry on", "[mesh-editor]") {
    Square square;
    MeshEditor& editor = square.editor;
    editor.add_bone("Root", std::nullopt, engine_core::matrix4_identity());
    editor.clear();
    REQUIRE(editor.vertices().empty());
    REQUIRE(editor.triangles().empty());
    REQUIRE(editor.bones().empty());
    REQUIRE(refusal([&] { editor.position(square.b); }) == "no vertex 1");
    REQUIRE(editor.add_vertex(Vec3{}) == 4);
    REQUIRE(editor.add_bone("Root", std::nullopt, engine_core::matrix4_identity()) == 1);
    REQUIRE(editor.build().vertices.size() == 1);
}

TEST_CASE("ME7 UVs and colors round-trip, and a position must be finite", "[mesh-editor]") {
    MeshEditor editor;
    const Id v = editor.add_vertex(Vec3{});
    editor.set_uv(v, Vec2{0.5f, 2.f});
    REQUIRE(editor.uv(v).x == 0.5f);
    REQUIRE(editor.uv(v).y == 2.f);
    editor.set_color(v, {1.f, 0.5f, 0.f, 0.25f});
    const std::array<float, 4> color = editor.color(v);
    REQUIRE(near(color[1], 0.5f, 1.f / 255.f));
    REQUIRE(near(color[3], 0.25f, 1.f / 255.f));
    const Data built = editor.build();
    REQUIRE(built.vertices[0].rgba[2] == 0);
    REQUIRE((built.flags & anarchy::amesh::FLAG_VERTEX_COLOR) != 0);
    REQUIRE_FALSE(refusal([&] { editor.add_vertex(Vec3{std::nanf(""), 0.f, 0.f}); }).empty());
    REQUIRE_FALSE(refusal([&] { editor.set_position(v, Vec3{INFINITY, 0.f, 0.f}); }).empty());
}

TEST_CASE("MB1 bones come out parents first, whatever order they were made in", "[mesh-editor]") {
    MeshEditor editor;
    const Id a = editor.add_bone("A", std::nullopt, engine_core::matrix4_identity());
    const Id b = editor.add_bone("B", a, engine_core::matrix4_translation(0.f, 1.f, 0.f));
    const Id c = editor.add_bone("C", std::nullopt, engine_core::matrix4_translation(0.f, -1.f, 0.f));
    editor.set_bone_parent(a, c);
    REQUIRE(editor.bone_parent(a) == c);
    const Data built = editor.build();
    REQUIRE(built.bones.size() == 3);
    REQUIRE(built.bones[0].name == "C");
    REQUIRE(built.bones[1].name == "A");
    REQUIRE(built.bones[2].name == "B");
    REQUIRE(built.bones[0].parent == anarchy::amesh::kNoBone);
    REQUIRE(built.bones[1].parent == 0);
    REQUIRE(built.bones[2].parent == 1);
    REQUIRE(built.bones[0].t[1] == -1.f);
    REQUIRE(editor.bones() == std::vector<Id>{a, b, c});
}

TEST_CASE("MB2 removing a bone moves its children up and renormalizes its vertices", "[mesh-editor]") {
    MeshEditor editor;
    const Id root = editor.add_bone("Root", std::nullopt, engine_core::matrix4_identity());
    const Id mid = editor.add_bone("Mid", root, engine_core::matrix4_translation(0.f, 1.f, 0.f));
    const Id tip = editor.add_bone("Tip", mid, engine_core::matrix4_translation(0.f, 2.f, 0.f));
    const Id shared = editor.add_vertex(Vec3{0.f, 1.5f, 0.f});
    const Id only_mid = editor.add_vertex(Vec3{0.f, 1.f, 0.f});
    const std::vector<float> weights{0.25f, 0.75f};
    editor.set_vertex_bones(shared, {mid, tip}, &weights);
    editor.set_vertex_bones(only_mid, {mid}, nullptr);

    editor.remove_bone(mid);
    REQUIRE(editor.bone_parent(tip) == root);
    // Its rest stays where it was, in mesh space.
    REQUIRE(engine_core::matrix4_position(editor.bone_transform(tip)).y == 2.f);
    REQUIRE(editor.vertex_bones(shared).size() == 1);
    REQUIRE(editor.vertex_bones(shared)[0].first == tip);
    REQUIRE(near(editor.vertex_bones(shared)[0].second, 1.f));
    REQUIRE(editor.vertex_bones(only_mid).empty());
    REQUIRE(refusal([&] { editor.bone_name(mid); }) == "no bone 1");
    REQUIRE_FALSE(editor.find_bone("Mid"));

    const Data built = editor.build();
    REQUIRE(built.bones.size() == 2);
    // The vertex left with no bones is the root's.
    REQUIRE(built.vertices[1].bone[0] == 0);
    REQUIRE(built.vertices[1].weight[0] == 1.f);
    REQUIRE(built.vertices[0].bone[0] == 1);
}

TEST_CASE("MB3 weights: equal by default, four at most, renormalized, and checked", "[mesh-editor]") {
    MeshEditor editor;
    std::vector<Id> bones;
    for (int i = 0; i < 6; ++i) {
        bones.push_back(editor.add_bone("B" + std::to_string(i), std::nullopt, engine_core::matrix4_identity()));
    }
    const Id v = editor.add_vertex(Vec3{});

    editor.set_vertex_bones(v, {bones[0], bones[1]}, nullptr);
    REQUIRE(near(editor.vertex_bones(v)[0].second, 0.5f));
    REQUIRE(near(editor.vertex_bones(v)[1].second, 0.5f));

    const std::vector<float> six{0.1f, 0.6f, 0.05f, 0.3f, 0.4f, 0.2f};
    editor.set_vertex_bones(v, bones, &six);
    const auto& kept = editor.vertex_bones(v);
    REQUIRE(kept.size() == 4);
    REQUIRE(kept[0].first == bones[1]);
    REQUIRE(kept[1].first == bones[4]);
    REQUIRE(kept[2].first == bones[3]);
    REQUIRE(kept[3].first == bones[5]);
    REQUIRE(near(kept[0].second + kept[1].second + kept[2].second + kept[3].second, 1.f));
    REQUIRE(near(kept[0].second, 0.6f / 1.5f));

    // The same bone twice is one influence.
    const std::vector<float> twice{1.f, 3.f};
    editor.set_vertex_bones(v, {bones[2], bones[2]}, &twice);
    REQUIRE(editor.vertex_bones(v).size() == 1);
    REQUIRE(near(editor.vertex_bones(v)[0].second, 1.f));

    const std::vector<float> one{1.f};
    REQUIRE(refusal([&] { editor.set_vertex_bones(v, {bones[0], bones[1]}, &one); }) ==
            "bones and weights must be the same length");
    const std::vector<float> negative{-1.f, 2.f};
    REQUIRE(refusal([&] { editor.set_vertex_bones(v, {bones[0], bones[1]}, &negative); }) ==
            "a weight must be a finite number of 0 or more");
    const std::vector<float> zeros{0.f, 0.f};
    REQUIRE(refusal([&] { editor.set_vertex_bones(v, {bones[0], bones[1]}, &zeros); }) ==
            "the weights must not all be 0");
    REQUIRE(refusal([&] { editor.set_vertex_bones(v, {99}, nullptr); }) == "no bone 99");
    // A refused call changes nothing.
    REQUIRE(editor.vertex_bones(v).size() == 1);

    editor.set_vertex_bones(v, {}, nullptr);
    REQUIRE(editor.vertex_bones(v).empty());
}

TEST_CASE("MB4 an unweighted vertex is bound to the first root at build, and still reads as unweighted", "[mesh-editor]") {
    MeshEditor editor;
    editor.add_bone("Child", std::nullopt, engine_core::matrix4_identity());
    const Id root = editor.add_bone("Root", std::nullopt, engine_core::matrix4_identity());
    editor.set_bone_parent(0, root);
    const Id v = editor.add_vertex(Vec3{});
    const Data built = editor.build();
    REQUIRE(editor.vertex_bones(v).empty());
    // Root (id 1) is the only root, and it comes first.
    REQUIRE(built.bones[0].name == "Root");
    REQUIRE(built.vertices[0].bone[0] == 0);
    REQUIRE(built.vertices[0].weight[0] == 1.f);
    REQUIRE((built.flags & anarchy::amesh::FLAG_SKINNED) != 0);
}

TEST_CASE("MB5 a bone's cull radius reaches its farthest weighted vertex", "[mesh-editor]") {
    MeshEditor editor;
    const Id root = editor.add_bone("Root", std::nullopt, engine_core::matrix4_identity());
    const Id tip = editor.add_bone("Tip", root, engine_core::matrix4_translation(0.f, 2.f, 0.f));
    const Id far = editor.add_vertex(Vec3{0.f, 5.f, 0.f});
    const Id near_tip = editor.add_vertex(Vec3{0.f, 2.5f, 0.f});
    editor.set_vertex_bones(far, {tip}, nullptr);
    editor.set_vertex_bones(near_tip, {tip, root}, nullptr);
    const Data built = editor.build();
    REQUIRE(near(built.bones[1].cull_radius, 3.f));
    REQUIRE(near(built.bones[0].cull_radius, 2.5f));
}

TEST_CASE("MB6 bone names, parents, and rest transforms are checked", "[mesh-editor]") {
    MeshEditor editor;
    const Id a = editor.add_bone("A", std::nullopt, engine_core::matrix4_identity());
    const Id b = editor.add_bone("B", a, engine_core::matrix4_identity());
    REQUIRE(refusal([&] { editor.add_bone("A", std::nullopt, engine_core::matrix4_identity()); }) ==
            "a bone named A already exists");
    REQUIRE(refusal([&] { editor.add_bone("", std::nullopt, engine_core::matrix4_identity()); }) ==
            "a bone's name must not be empty");
    REQUIRE_FALSE(refusal([&] { editor.set_bone_parent(a, b); }).empty());
    REQUIRE(refusal([&] { editor.set_bone_parent(a, a); }) == refusal([&] { editor.set_bone_parent(a, b); }));
    Matrix4 flat = engine_core::matrix4_identity();
    flat.m[5] = 0.f;
    REQUIRE(refusal([&] { editor.add_bone("C", std::nullopt, flat); }) ==
            "a bone's rest transform must have an inverse");
    REQUIRE(refusal([&] { editor.set_bone_transform(a, flat); }) == "a bone's rest transform must have an inverse");

    editor.set_bone_name(a, "Renamed");
    REQUIRE(editor.find_bone("Renamed") == a);
    REQUIRE_FALSE(editor.find_bone("A"));
    REQUIRE(editor.add_bone("A", std::nullopt, engine_core::matrix4_identity()) == 2);
    REQUIRE(refusal([&] { editor.set_bone_name(b, "Renamed"); }) == "a bone named Renamed already exists");
    // A bone may keep its own name.
    editor.set_bone_name(b, "B");
}

namespace {

// A skinned build: two bones, a box with blended weights.
Data skinned_box() {
    MeshEditor editor;
    const Id root = editor.add_bone("Root", std::nullopt, engine_core::matrix4_identity());
    const Matrix4 turned =
        engine_core::matrix4_multiply(engine_core::matrix4_translation(0.f, 2.f, 0.f),
                                      engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, 0.5));
    const Id tip = editor.add_bone("Tip", root, turned);
    for (const Id v : editor.add_shape([](Data& data) {
             engine_core::add_box(data, Vec3{1.f, 4.f, 1.f}, Vec3{0.f, 2.f, 0.f});
         })) {
        const float t = editor.position(v).y / 4.f;
        const std::vector<float> weights{1.f - t, t};
        editor.set_vertex_bones(v, {root, tip}, &weights);
    }
    return editor.build();
}

}  // namespace

TEST_CASE("MB7 a skinned mesh opened again keeps its bones and weights", "[mesh-editor]") {
    const Data first = skinned_box();
    const Data read = anarchy::amesh::read(anarchy::amesh::write(first));
    const Data again = anarchy::amesh::read(anarchy::amesh::write(MeshEditor(read).build()));
    REQUIRE(again.bones.size() == 2);
    REQUIRE(again.bones[1].name == "Tip");
    REQUIRE(again.bones[1].parent == 0);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            REQUIRE(near(again.bones[1].m[row][column], first.bones[1].m[row][column]));
        }
    }
    REQUIRE(again.vertices.size() == first.vertices.size());
    for (std::size_t i = 0; i < first.vertices.size(); ++i) {
        for (int k = 0; k < 4; ++k) {
            REQUIRE(again.vertices[i].bone[k] == read.vertices[i].bone[k]);
            REQUIRE(near(again.vertices[i].weight[k], read.vertices[i].weight[k], 1.f / 255.f));
        }
    }
}

TEST_CASE("MB8 a skinned build is AMESH, and its skeleton rests where the bones were put", "[mesh-editor]") {
    const Data built = skinned_box();
    const Data read = anarchy::amesh::read(anarchy::amesh::write(built));
    const std::shared_ptr<const engine_core::Skeleton> skeleton = engine_core::make_skeleton(read.bones);
    REQUIRE(skeleton != nullptr);
    const Matrix4 turned =
        engine_core::matrix4_multiply(engine_core::matrix4_translation(0.f, 2.f, 0.f),
                                      engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, 0.5));
    for (int i = 0; i < 16; ++i) {
        REQUIRE(near(skeleton->bones[1].rest.m[i], turned.m[i]));
    }
    REQUIRE(skeleton->find("Tip") == 1);
}
