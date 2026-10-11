#pragma once

// A Mesh's geometry while scripts edit it: vertices, triangles, and bones in
// slots whose ids never change and are never reused while the editor lives,
// whatever else is removed. build() compacts what is live into AMESH data.
// It knows nothing of Lua or the DataModel: MeshEditors (engine_core) decides
// when a build reaches the Mesh, and MeshEditorBindings.cpp is its Lua face.
// See docs/superpowers/specs/2026-10-11-mesh-editor-design.md.

#include "Matrix4.hpp"
#include "Vector2.hpp"
#include "Vector3.hpp"
#include "amesh.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine_core {

// A refused edit: a missing id, a broken rule, or a limit. what() is what a script sees.
class MeshEditError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class MeshEditor {
public:
    using Id = std::uint32_t;
    // A bone, and how much of a vertex it moves.
    using Influence = std::pair<Id, float>;

    // Which Mesh this edits, and the world and mode it was opened in. MeshEditors sets it.
    struct Owner {
        InstanceId mesh = 0;
        std::uint32_t generation = 0;
        bool playing = false;
    };

    // Starts from data: its vertex, triangle, and bone indices are the first
    // ids, and its normals count as set. Its LODs, subsets, and pieces are not kept.
    explicit MeshEditor(const anarchy::amesh::Data& data = {});

    // A new vertex has no normal set, UV (0, 0), white, opaque, and no bones.
    Id add_vertex(Vec3 position);
    // Removes it and every triangle that uses it.
    void remove_vertex(Id vertex);
    std::vector<Id> vertices() const;
    Vec3 position(Id vertex) const;
    void set_position(Id vertex, Vec3 position);
    // The set normal, or else what its triangles give, area weighted, as
    // amesh::compute_normals makes it: (0, 1, 0) when they give none.
    Vec3 normal(Id vertex) const;
    // Normalized. nullopt goes back to the computed one. A zero length is refused.
    void set_normal(Id vertex, std::optional<Vec3> normal);
    Vec2 uv(Id vertex) const;
    void set_uv(Id vertex, Vec2 uv);
    // Red, green, blue, and alpha from 0 to 1, kept as bytes.
    std::array<float, 4> color(Id vertex) const;
    void set_color(Id vertex, const std::array<float, 4>& rgba);

    // Counter-clockwise front faces.
    Id add_triangle(Id a, Id b, Id c);
    // Its vertices stay.
    void remove_triangle(Id triangle);
    std::vector<Id> triangles() const;
    std::array<Id, 3> triangle_vertices(Id triangle) const;

    // Appends what make adds to an empty Data (MeshShapes' builders) and
    // returns its vertices' new ids. Their normals count as set.
    std::vector<Id> add_shape(const std::function<void(anarchy::amesh::Data&)>& make);
    // Removes every vertex, triangle, and bone. Ids made before stay dead;
    // new ones carry on from where they were.
    void clear();

    // rest is where the bone rests, in mesh space.
    Id add_bone(const std::string& name, std::optional<Id> parent, const Matrix4& rest);
    // Its children move to its parent, keeping their rest. Its weights are
    // dropped and each vertex's others renormalized.
    void remove_bone(Id bone);
    std::vector<Id> bones() const;
    std::optional<Id> find_bone(const std::string& name) const;
    const std::string& bone_name(Id bone) const;
    void set_bone_name(Id bone, const std::string& name);
    std::optional<Id> bone_parent(Id bone) const;
    void set_bone_parent(Id bone, std::optional<Id> parent);
    Matrix4 bone_transform(Id bone) const;
    void set_bone_transform(Id bone, const Matrix4& rest);

    // weights null: equal shares. More than four keeps the four largest, and
    // the kept ones are renormalized to sum to 1. No bones clears them.
    void set_vertex_bones(Id vertex, const std::vector<Id>& bones, const std::vector<float>* weights);
    // Largest first. Empty for a vertex build() binds to the first root.
    const std::vector<Influence>& vertex_bones(Id vertex) const;

    // What is live, compacted in id order; bones parents first. A vertex with
    // no bones in a mesh with some is the first root's (the lowest id with no
    // parent) at weight 1. Unset normals, tangents, cull radii, and the box are computed.
    anarchy::amesh::Data build() const;

    // Counts every change, so a caller can tell when there is something new to build.
    std::uint64_t edits() const { return edits_; }
    // The vertex, triangle, and bone slots held: live, or removed since the last clear.
    std::size_t slot_count() const { return vertices_.size() + triangles_.size() + bones_.size(); }

    Owner owner;
    bool ended() const { return ended_; }
    void end() { ended_ = true; }

private:
    struct VertexSlot {
        bool live = true;
        Vec3 p{};
        bool normal_set = false;
        Vec3 n{0.f, 1.f, 0.f};
        Vec2 uv{};
        std::array<std::uint8_t, 4> rgba{{255, 255, 255, 255}};
        std::vector<Influence> skin;
        // The live triangles that use it.
        std::vector<Id> triangles;
    };
    struct TriangleSlot {
        bool live = true;
        std::array<Id, 3> corners{};
    };
    struct BoneSlot {
        bool live = true;
        std::string name;
        std::optional<Id> parent;
        Matrix4 rest = matrix4_identity();
    };

    static VertexSlot slot_of(const anarchy::amesh::Vertex& vertex, std::size_t bone_count);
    const VertexSlot& vertex_slot(Id vertex) const;
    VertexSlot& vertex_slot(Id vertex);
    const TriangleSlot& triangle_slot(Id triangle) const;
    const BoneSlot& bone_slot(Id bone) const;
    BoneSlot& bone_slot(Id bone);
    // Appends a triangle and links it to its corners. No checks, no counts.
    Id push_triangle(const std::array<Id, 3>& corners);
    void drop_triangle(Id triangle);
    void check_bone_name(const std::string& name, std::optional<Id> self) const;

    // Slot i holds id base + i. clear() lets go of every slot by moving each
    // base past them, so ids carry on while the memory does not grow.
    std::vector<VertexSlot> vertices_;
    std::vector<TriangleSlot> triangles_;
    std::vector<BoneSlot> bones_;
    Id vertex_base_ = 0;
    Id triangle_base_ = 0;
    Id bone_base_ = 0;
    std::unordered_map<std::string, Id> bone_names_;
    std::size_t live_vertices_ = 0;
    std::size_t live_triangles_ = 0;
    std::size_t live_bones_ = 0;
    std::uint64_t edits_ = 0;
    bool ended_ = false;
};

}  // namespace engine_core
