#include "MeshEditor.hpp"

#include <algorithm>
#include <cmath>

namespace engine_core {
namespace {

using anarchy::amesh::Data;
using anarchy::amesh::kNoBone;

std::string text(MeshEditor::Id id) { return std::to_string(id); }

bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

Vec3 minus(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }

Vec3 cross(Vec3 a, Vec3 b) { return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

float length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::lround(std::min(1.f, std::max(0.f, value)) * 255.f));
}

// Where an AMESH bone record rests, as a Matrix4: m is row-major, as make_skeleton reads it.
Matrix4 rest_of(const anarchy::amesh::Bone& bone) {
    Matrix4 rest = matrix4_identity();
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            rest.m[column * 4 + row] = bone.m[row][column];
        }
    }
    rest.m[12] = bone.t[0];
    rest.m[13] = bone.t[1];
    rest.m[14] = bone.t[2];
    return rest;
}

void check_rest(const Matrix4& rest) {
    for (const float value : rest.m) {
        if (!std::isfinite(value)) {
            throw MeshEditError("a bone's rest transform must be finite");
        }
    }
    // The upper 3x3's determinant, as column 0 dotted with column 1 cross column 2.
    const float* m = rest.m;
    const double det = double(m[0]) * (double(m[5]) * m[10] - double(m[6]) * m[9]) +
                       double(m[1]) * (double(m[6]) * m[8] - double(m[4]) * m[10]) +
                       double(m[2]) * (double(m[4]) * m[9] - double(m[5]) * m[8]);
    if (!(std::fabs(det) > 1e-12)) {
        throw MeshEditError("a bone's rest transform must have an inverse");
    }
}

// Weights summing to 1, or none when they sum to nothing.
void renormalize(std::vector<MeshEditor::Influence>& skin) {
    float total = 0.f;
    for (const MeshEditor::Influence& influence : skin) {
        total += influence.second;
    }
    if (!(total > 0.f)) {
        skin.clear();
        return;
    }
    for (MeshEditor::Influence& influence : skin) {
        influence.second /= total;
    }
}

}  // namespace

MeshEditor::VertexSlot MeshEditor::slot_of(const anarchy::amesh::Vertex& vertex, std::size_t bone_count) {
    VertexSlot slot;
    slot.p = Vec3{vertex.p[0], vertex.p[1], vertex.p[2]};
    slot.normal_set = true;
    slot.n = Vec3{vertex.n[0], vertex.n[1], vertex.n[2]};
    slot.uv = Vec2{vertex.uv[0], vertex.uv[1]};
    std::copy(std::begin(vertex.rgba), std::end(vertex.rgba), slot.rgba.begin());
    for (int k = 0; k < 4; ++k) {
        if (vertex.bone[k] != kNoBone && vertex.bone[k] < bone_count && vertex.weight[k] > 0.f) {
            slot.skin.emplace_back(vertex.bone[k], vertex.weight[k]);
        }
    }
    return slot;
}

MeshEditor::MeshEditor(const Data& data) {
    vertices_.reserve(data.vertices.size());
    for (const anarchy::amesh::Vertex& vertex : data.vertices) {
        vertices_.push_back(slot_of(vertex, data.bones.size()));
    }
    live_vertices_ = vertices_.size();
    triangles_.reserve(data.indices.size() / 3);
    for (std::size_t t = 0; t + 2 < data.indices.size(); t += 3) {
        push_triangle({{data.indices[t], data.indices[t + 1], data.indices[t + 2]}});
    }
    live_triangles_ = triangles_.size();
    bones_.reserve(data.bones.size());
    for (const anarchy::amesh::Bone& bone : data.bones) {
        BoneSlot slot;
        slot.name = bone.name;
        if (bone.parent != kNoBone) {
            slot.parent = bone.parent;
        }
        slot.rest = rest_of(bone);
        if (!slot.name.empty()) {
            bone_names_.emplace(slot.name, static_cast<Id>(bones_.size()));
        }
        bones_.push_back(std::move(slot));
    }
    live_bones_ = bones_.size();
}

const MeshEditor::VertexSlot& MeshEditor::vertex_slot(Id vertex) const {
    if (vertex >= vertices_.size() || !vertices_[vertex].live) {
        throw MeshEditError("no vertex " + text(vertex));
    }
    return vertices_[vertex];
}

MeshEditor::VertexSlot& MeshEditor::vertex_slot(Id vertex) {
    return const_cast<VertexSlot&>(static_cast<const MeshEditor&>(*this).vertex_slot(vertex));
}

const MeshEditor::TriangleSlot& MeshEditor::triangle_slot(Id triangle) const {
    if (triangle >= triangles_.size() || !triangles_[triangle].live) {
        throw MeshEditError("no triangle " + text(triangle));
    }
    return triangles_[triangle];
}

const MeshEditor::BoneSlot& MeshEditor::bone_slot(Id bone) const {
    if (bone >= bones_.size() || !bones_[bone].live) {
        throw MeshEditError("no bone " + text(bone));
    }
    return bones_[bone];
}

MeshEditor::BoneSlot& MeshEditor::bone_slot(Id bone) {
    return const_cast<BoneSlot&>(static_cast<const MeshEditor&>(*this).bone_slot(bone));
}

MeshEditor::Id MeshEditor::push_triangle(const std::array<Id, 3>& corners) {
    const Id id = static_cast<Id>(triangles_.size());
    TriangleSlot slot;
    slot.corners = corners;
    triangles_.push_back(slot);
    for (const Id corner : corners) {
        if (corner < vertices_.size()) {
            vertices_[corner].triangles.push_back(id);
        }
    }
    return id;
}

void MeshEditor::drop_triangle(Id triangle) {
    TriangleSlot& slot = triangles_[triangle];
    slot.live = false;
    for (const Id corner : slot.corners) {
        if (corner < vertices_.size()) {
            std::vector<Id>& using_it = vertices_[corner].triangles;
            using_it.erase(std::remove(using_it.begin(), using_it.end(), triangle), using_it.end());
        }
    }
    --live_triangles_;
}

MeshEditor::Id MeshEditor::add_vertex(Vec3 position) {
    if (!finite(position)) {
        throw MeshEditError("position must be finite");
    }
    if (live_vertices_ >= anarchy::amesh::kMaxVertices) {
        throw MeshEditError("a mesh holds at most " + std::to_string(anarchy::amesh::kMaxVertices) + " vertices");
    }
    VertexSlot slot;
    slot.p = position;
    vertices_.push_back(std::move(slot));
    ++live_vertices_;
    ++edits_;
    return static_cast<Id>(vertices_.size() - 1);
}

void MeshEditor::remove_vertex(Id vertex) {
    VertexSlot& slot = vertex_slot(vertex);
    // A copy: dropping a triangle edits the list.
    const std::vector<Id> using_it = slot.triangles;
    for (const Id triangle : using_it) {
        if (triangles_[triangle].live) {
            drop_triangle(triangle);
        }
    }
    slot = VertexSlot{};
    slot.live = false;
    --live_vertices_;
    ++edits_;
}

std::vector<MeshEditor::Id> MeshEditor::vertices() const {
    std::vector<Id> out;
    out.reserve(live_vertices_);
    for (std::size_t id = 0; id < vertices_.size(); ++id) {
        if (vertices_[id].live) {
            out.push_back(static_cast<Id>(id));
        }
    }
    return out;
}

Vec3 MeshEditor::position(Id vertex) const { return vertex_slot(vertex).p; }

void MeshEditor::set_position(Id vertex, Vec3 position) {
    VertexSlot& slot = vertex_slot(vertex);
    if (!finite(position)) {
        throw MeshEditError("position must be finite");
    }
    slot.p = position;
    ++edits_;
}

Vec3 MeshEditor::normal(Id vertex) const {
    const VertexSlot& slot = vertex_slot(vertex);
    if (slot.normal_set) {
        return slot.n;
    }
    Vec3 sum{};
    for (const Id triangle : slot.triangles) {
        const std::array<Id, 3>& c = triangles_[triangle].corners;
        const Vec3 face = cross(minus(vertices_[c[1]].p, vertices_[c[0]].p), minus(vertices_[c[2]].p, vertices_[c[0]].p));
        sum = Vec3{sum.x + face.x, sum.y + face.y, sum.z + face.z};
    }
    const float size = length(sum);
    if (!(size > 0.f) || !std::isfinite(size)) {
        return Vec3{0.f, 1.f, 0.f};
    }
    return Vec3{sum.x / size, sum.y / size, sum.z / size};
}

void MeshEditor::set_normal(Id vertex, std::optional<Vec3> normal) {
    VertexSlot& slot = vertex_slot(vertex);
    if (!normal) {
        slot.normal_set = false;
        ++edits_;
        return;
    }
    const float size = length(*normal);
    if (!finite(*normal) || !(size > 0.f) || !std::isfinite(size)) {
        throw MeshEditError("normal must be a finite Vector3 that is not zero");
    }
    slot.n = Vec3{normal->x / size, normal->y / size, normal->z / size};
    slot.normal_set = true;
    ++edits_;
}

Vec2 MeshEditor::uv(Id vertex) const { return vertex_slot(vertex).uv; }

void MeshEditor::set_uv(Id vertex, Vec2 uv) {
    VertexSlot& slot = vertex_slot(vertex);
    if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
        throw MeshEditError("uv must be finite");
    }
    slot.uv = uv;
    ++edits_;
}

std::array<float, 4> MeshEditor::color(Id vertex) const {
    const VertexSlot& slot = vertex_slot(vertex);
    return {{slot.rgba[0] / 255.f, slot.rgba[1] / 255.f, slot.rgba[2] / 255.f, slot.rgba[3] / 255.f}};
}

void MeshEditor::set_color(Id vertex, const std::array<float, 4>& rgba) {
    VertexSlot& slot = vertex_slot(vertex);
    for (const float channel : rgba) {
        if (!std::isfinite(channel)) {
            throw MeshEditError("color must be finite");
        }
    }
    for (std::size_t k = 0; k < 4; ++k) {
        slot.rgba[k] = to_byte(rgba[k]);
    }
    ++edits_;
}

MeshEditor::Id MeshEditor::add_triangle(Id a, Id b, Id c) {
    vertex_slot(a);
    vertex_slot(b);
    vertex_slot(c);
    if (a == b || b == c || a == c) {
        throw MeshEditError("a triangle needs three different vertices");
    }
    if (live_triangles_ >= anarchy::amesh::kMaxTriangles) {
        throw MeshEditError("a mesh holds at most " + std::to_string(anarchy::amesh::kMaxTriangles) + " triangles");
    }
    const Id id = push_triangle({{a, b, c}});
    ++live_triangles_;
    ++edits_;
    return id;
}

void MeshEditor::remove_triangle(Id triangle) {
    triangle_slot(triangle);
    drop_triangle(triangle);
    ++edits_;
}

std::vector<MeshEditor::Id> MeshEditor::triangles() const {
    std::vector<Id> out;
    out.reserve(live_triangles_);
    for (std::size_t id = 0; id < triangles_.size(); ++id) {
        if (triangles_[id].live) {
            out.push_back(static_cast<Id>(id));
        }
    }
    return out;
}

std::array<MeshEditor::Id, 3> MeshEditor::triangle_vertices(Id triangle) const {
    return triangle_slot(triangle).corners;
}

std::vector<MeshEditor::Id> MeshEditor::add_shape(const std::function<void(Data&)>& make) {
    Data made;
    make(made);
    const std::size_t new_triangles = made.indices.size() / 3;
    if (live_vertices_ + made.vertices.size() > anarchy::amesh::kMaxVertices ||
        live_triangles_ + new_triangles > anarchy::amesh::kMaxTriangles) {
        throw MeshEditError("the shape is more than one mesh can hold");
    }
    const Id first = static_cast<Id>(vertices_.size());
    std::vector<Id> ids;
    ids.reserve(made.vertices.size());
    for (const anarchy::amesh::Vertex& vertex : made.vertices) {
        vertices_.push_back(slot_of(vertex, 0));
        ids.push_back(static_cast<Id>(vertices_.size() - 1));
    }
    for (std::size_t t = 0; t < new_triangles; ++t) {
        push_triangle({{first + made.indices[t * 3], first + made.indices[t * 3 + 1], first + made.indices[t * 3 + 2]}});
    }
    live_vertices_ += made.vertices.size();
    live_triangles_ += new_triangles;
    ++edits_;
    return ids;
}

void MeshEditor::clear() {
    for (VertexSlot& slot : vertices_) {
        slot = VertexSlot{};
        slot.live = false;
    }
    for (TriangleSlot& slot : triangles_) {
        slot.live = false;
    }
    for (BoneSlot& slot : bones_) {
        slot = BoneSlot{};
        slot.live = false;
    }
    bone_names_.clear();
    live_vertices_ = 0;
    live_triangles_ = 0;
    live_bones_ = 0;
    ++edits_;
}

void MeshEditor::check_bone_name(const std::string& name, std::optional<Id> self) const {
    if (name.empty()) {
        throw MeshEditError("a bone's name must not be empty");
    }
    const auto found = bone_names_.find(name);
    if (found != bone_names_.end() && (!self || found->second != *self)) {
        throw MeshEditError("a bone named " + name + " already exists");
    }
}

MeshEditor::Id MeshEditor::add_bone(const std::string& name, std::optional<Id> parent, const Matrix4& rest) {
    check_bone_name(name, std::nullopt);
    if (parent) {
        bone_slot(*parent);
    }
    check_rest(rest);
    if (live_bones_ >= anarchy::amesh::kMaxBones) {
        throw MeshEditError("a mesh holds at most " + std::to_string(anarchy::amesh::kMaxBones) + " bones");
    }
    BoneSlot slot;
    slot.name = name;
    slot.parent = parent;
    slot.rest = rest;
    const Id id = static_cast<Id>(bones_.size());
    bones_.push_back(std::move(slot));
    bone_names_[name] = id;
    ++live_bones_;
    ++edits_;
    return id;
}

void MeshEditor::remove_bone(Id bone) {
    BoneSlot& gone = bone_slot(bone);
    const std::optional<Id> up = gone.parent;
    for (BoneSlot& other : bones_) {
        if (other.live && other.parent == bone) {
            other.parent = up;
        }
    }
    for (VertexSlot& slot : vertices_) {
        if (!slot.live) {
            continue;
        }
        const std::size_t before = slot.skin.size();
        slot.skin.erase(std::remove_if(slot.skin.begin(), slot.skin.end(),
                                       [bone](const Influence& influence) { return influence.first == bone; }),
                        slot.skin.end());
        if (slot.skin.size() != before) {
            renormalize(slot.skin);
        }
    }
    const auto named = bone_names_.find(gone.name);
    if (named != bone_names_.end() && named->second == bone) {
        bone_names_.erase(named);
    }
    gone = BoneSlot{};
    gone.live = false;
    --live_bones_;
    ++edits_;
}

std::vector<MeshEditor::Id> MeshEditor::bones() const {
    std::vector<Id> out;
    out.reserve(live_bones_);
    for (std::size_t id = 0; id < bones_.size(); ++id) {
        if (bones_[id].live) {
            out.push_back(static_cast<Id>(id));
        }
    }
    return out;
}

std::optional<MeshEditor::Id> MeshEditor::find_bone(const std::string& name) const {
    const auto found = bone_names_.find(name);
    if (found == bone_names_.end()) {
        return std::nullopt;
    }
    return found->second;
}

const std::string& MeshEditor::bone_name(Id bone) const { return bone_slot(bone).name; }

void MeshEditor::set_bone_name(Id bone, const std::string& name) {
    BoneSlot& slot = bone_slot(bone);
    check_bone_name(name, bone);
    const auto named = bone_names_.find(slot.name);
    if (named != bone_names_.end() && named->second == bone) {
        bone_names_.erase(named);
    }
    slot.name = name;
    bone_names_[name] = bone;
    ++edits_;
}

std::optional<MeshEditor::Id> MeshEditor::bone_parent(Id bone) const { return bone_slot(bone).parent; }

void MeshEditor::set_bone_parent(Id bone, std::optional<Id> parent) {
    BoneSlot& slot = bone_slot(bone);
    for (std::optional<Id> at = parent; at; at = bone_slot(*at).parent) {
        if (*at == bone) {
            throw MeshEditError("bone " + text(bone) + " cannot be its own ancestor");
        }
    }
    slot.parent = parent;
    ++edits_;
}

Matrix4 MeshEditor::bone_transform(Id bone) const { return bone_slot(bone).rest; }

void MeshEditor::set_bone_transform(Id bone, const Matrix4& rest) {
    BoneSlot& slot = bone_slot(bone);
    check_rest(rest);
    slot.rest = rest;
    ++edits_;
}

void MeshEditor::set_vertex_bones(Id vertex, const std::vector<Id>& bones, const std::vector<float>* weights) {
    VertexSlot& slot = vertex_slot(vertex);
    if (weights != nullptr && weights->size() != bones.size()) {
        throw MeshEditError("bones and weights must be the same length");
    }
    std::vector<Influence> merged;
    float total = 0.f;
    for (std::size_t i = 0; i < bones.size(); ++i) {
        bone_slot(bones[i]);
        const float weight = weights != nullptr ? (*weights)[i] : 1.f;
        if (!std::isfinite(weight) || weight < 0.f) {
            throw MeshEditError("a weight must be a finite number of 0 or more");
        }
        total += weight;
        const Id bone = bones[i];
        const auto same =
            std::find_if(merged.begin(), merged.end(), [bone](const Influence& influence) { return influence.first == bone; });
        if (same != merged.end()) {
            same->second += weight;
        } else {
            merged.emplace_back(bone, weight);
        }
    }
    if (!bones.empty()) {
        if (!(total > 0.f)) {
            throw MeshEditError("the weights must not all be 0");
        }
        merged.erase(std::remove_if(merged.begin(), merged.end(),
                                    [](const Influence& influence) { return !(influence.second > 0.f); }),
                     merged.end());
        std::stable_sort(merged.begin(), merged.end(),
                         [](const Influence& a, const Influence& b) { return a.second > b.second; });
        if (merged.size() > anarchy::amesh::kMaxInfluences) {
            merged.resize(anarchy::amesh::kMaxInfluences);
        }
        renormalize(merged);
    }
    slot.skin = std::move(merged);
    ++edits_;
}

const std::vector<MeshEditor::Influence>& MeshEditor::vertex_bones(Id vertex) const { return vertex_slot(vertex).skin; }

Data MeshEditor::build() const {
    Data out;

    // Bones: each one's unplaced ancestors first, root down, otherwise in id order.
    std::vector<std::uint16_t> bone_index(bones_.size(), kNoBone);
    std::vector<Id> order;
    order.reserve(live_bones_);
    std::vector<Id> chain;
    for (std::size_t id = 0; id < bones_.size(); ++id) {
        if (!bones_[id].live || bone_index[id] != kNoBone) {
            continue;
        }
        chain.clear();
        for (std::optional<Id> at = static_cast<Id>(id); at && bone_index[*at] == kNoBone; at = bones_[*at].parent) {
            chain.push_back(*at);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            bone_index[*it] = static_cast<std::uint16_t>(order.size());
            order.push_back(*it);
        }
    }
    std::uint16_t root = kNoBone;
    for (std::size_t id = 0; id < bones_.size(); ++id) {
        if (bones_[id].live && !bones_[id].parent) {
            root = bone_index[id];
            break;
        }
    }
    out.bones.reserve(order.size());
    for (const Id id : order) {
        const BoneSlot& slot = bones_[id];
        anarchy::amesh::Bone bone;
        bone.name = slot.name;
        bone.parent = slot.parent ? bone_index[*slot.parent] : kNoBone;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                bone.m[row][column] = slot.rest.m[column * 4 + row];
            }
        }
        bone.t[0] = slot.rest.m[12];
        bone.t[1] = slot.rest.m[13];
        bone.t[2] = slot.rest.m[14];
        out.bones.push_back(bone);
    }

    // Vertices in id order.
    std::vector<std::uint32_t> index_of(vertices_.size(), 0);
    out.vertices.reserve(live_vertices_);
    bool any_color = false;
    for (std::size_t id = 0; id < vertices_.size(); ++id) {
        const VertexSlot& slot = vertices_[id];
        if (!slot.live) {
            continue;
        }
        index_of[id] = static_cast<std::uint32_t>(out.vertices.size());
        anarchy::amesh::Vertex vertex;
        vertex.p[0] = slot.p.x;
        vertex.p[1] = slot.p.y;
        vertex.p[2] = slot.p.z;
        vertex.uv[0] = slot.uv.x;
        vertex.uv[1] = slot.uv.y;
        std::copy(slot.rgba.begin(), slot.rgba.end(), vertex.rgba);
        any_color = any_color || slot.rgba[0] != 255 || slot.rgba[1] != 255 || slot.rgba[2] != 255 || slot.rgba[3] != 255;
        if (!order.empty()) {
            if (slot.skin.empty()) {
                vertex.bone[0] = root;
                vertex.weight[0] = 1.f;
            } else {
                for (std::size_t k = 0; k < slot.skin.size(); ++k) {
                    vertex.bone[k] = bone_index[slot.skin[k].first];
                    vertex.weight[k] = slot.skin[k].second;
                }
            }
            for (int k = 0; k < 4; ++k) {
                if (vertex.bone[k] == kNoBone || !(vertex.weight[k] > 0.f)) {
                    continue;
                }
                anarchy::amesh::Bone& bone = out.bones[vertex.bone[k]];
                const float reach = length(minus(slot.p, Vec3{bone.t[0], bone.t[1], bone.t[2]}));
                bone.cull_radius = std::max(bone.cull_radius, reach);
            }
        }
        out.vertices.push_back(vertex);
    }

    out.indices.reserve(live_triangles_ * 3);
    for (const TriangleSlot& triangle : triangles_) {
        if (triangle.live) {
            for (const Id corner : triangle.corners) {
                out.indices.push_back(index_of[corner]);
            }
        }
    }

    anarchy::amesh::compute_normals(out);
    for (std::size_t id = 0; id < vertices_.size(); ++id) {
        const VertexSlot& slot = vertices_[id];
        if (slot.live && slot.normal_set) {
            float* n = out.vertices[index_of[id]].n;
            n[0] = slot.n.x;
            n[1] = slot.n.y;
            n[2] = slot.n.z;
        }
    }
    anarchy::amesh::compute_tangents(out);
    anarchy::amesh::compute_aabb(out);
    if (any_color) {
        out.flags = static_cast<std::uint16_t>(out.flags | anarchy::amesh::FLAG_VERTEX_COLOR);
    }
    if (!order.empty()) {
        out.flags = static_cast<std::uint16_t>(out.flags | anarchy::amesh::FLAG_SKINNED);
    }
    return out;
}

}  // namespace engine_core
