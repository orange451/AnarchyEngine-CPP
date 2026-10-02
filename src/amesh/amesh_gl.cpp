#include "amesh.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef AE_MESH_NO_GL
#include "runner/gl.hpp"
#endif

// GpuMesh on the engine's own GL loader (runner/gl.hpp, a 3.3 core context).
// With AE_MESH_NO_GL every call is a no-op, and nothing here touches GL.

namespace anarchy::amesh {

GpuMesh::~GpuMesh() {
    destroy();
}

GpuMesh::GpuMesh(GpuMesh&& other) noexcept
    : vao_(std::exchange(other.vao_, 0)),
      vbo_(std::exchange(other.vbo_, 0)),
      ebo_(std::exchange(other.ebo_, 0)),
      lods_(std::move(other.lods_)),
      subsets_(std::move(other.subsets_)) {
    for (int i = 0; i < 3; ++i) {
        bounds_min_[i] = other.bounds_min_[i];
        bounds_max_[i] = other.bounds_max_[i];
    }
}

GpuMesh& GpuMesh::operator=(GpuMesh&& other) noexcept {
    if (this != &other) {
        destroy();
        vao_ = std::exchange(other.vao_, 0);
        vbo_ = std::exchange(other.vbo_, 0);
        ebo_ = std::exchange(other.ebo_, 0);
        lods_ = std::move(other.lods_);
        subsets_ = std::move(other.subsets_);
        for (int i = 0; i < 3; ++i) {
            bounds_min_[i] = other.bounds_min_[i];
            bounds_max_[i] = other.bounds_max_[i];
        }
    }
    return *this;
}

void GpuMesh::keep_bounds(const Data& data) {
    for (int axis = 0; axis < 3; ++axis) {
        bounds_min_[axis] = data.vertices.empty() ? 0.f : data.vertices[0].p[axis];
        bounds_max_[axis] = bounds_min_[axis];
    }
    for (const Vertex& vertex : data.vertices) {
        for (int axis = 0; axis < 3; ++axis) {
            bounds_min_[axis] = std::min(bounds_min_[axis], vertex.p[axis]);
            bounds_max_[axis] = std::max(bounds_max_[axis], vertex.p[axis]);
        }
    }
}

bool GpuMesh::valid() const {
    return vao_ != 0;
}

void GpuMesh::forget() {
    vao_ = vbo_ = ebo_ = 0;
    lods_.clear();
    subsets_.clear();
}

void GpuMesh::draw(int lod) const {
    if (!valid()) {
        return;
    }
    if (lod < 0 || static_cast<std::size_t>(lod) >= lods_.size()) {
        throw std::out_of_range("GpuMesh::draw: LOD " + std::to_string(lod) + " of " + std::to_string(lods_.size()));
    }
    draw_range(lods_[static_cast<std::size_t>(lod)].tri_begin, lods_[static_cast<std::size_t>(lod)].tri_count);
}

void GpuMesh::draw_subset(std::size_t subset) const {
    if (!valid()) {
        return;
    }
    if (subset >= subsets_.size()) {
        throw std::out_of_range("GpuMesh::draw_subset: subset " + std::to_string(subset) + " of " +
                                std::to_string(subsets_.size()));
    }
    draw_range(subsets_[subset].tri_begin, subsets_[subset].tri_count);
}

#ifdef AE_MESH_NO_GL

void GpuMesh::upload(const Data& data, bool) { keep_bounds(data); }
void GpuMesh::bind() const {}
void GpuMesh::draw_range(std::uint32_t, std::uint32_t) const {}
void GpuMesh::destroy() {
    lods_.clear();
    subsets_.clear();
}

#else

// The loader's GL types and constants are in namespace runner.
using namespace ::runner;

void GpuMesh::upload(const Data& data, bool dynamic) {
    // Everything is checked before GL is touched, so a throw leaves the old buffers intact.
    const std::size_t vertex_count = data.vertices.size();
    if (data.indices.size() % 3 != 0) {
        throw std::invalid_argument("GpuMesh::upload: the index count is not a multiple of 3");
    }
    const std::uint64_t triangle_count = data.indices.size() / 3;
    for (const std::uint32_t index : data.indices) {
        if (index >= vertex_count) {
            throw std::invalid_argument("GpuMesh::upload: index " + std::to_string(index) + " of " +
                                        std::to_string(vertex_count) + " vertices");
        }
    }
    std::vector<LodRange> lods = data.lods;
    if (lods.empty()) {
        lods.push_back({0, static_cast<std::uint32_t>(triangle_count)});
    }
    std::vector<LodRange> subsets;
    for (const Subset& subset : data.subsets) {
        subsets.push_back({subset.tri_begin, subset.tri_count});
    }
    for (const auto* ranges : {&lods, &subsets}) {
        for (const LodRange& range : *ranges) {
            if (std::uint64_t{range.tri_begin} + range.tri_count > triangle_count) {
                throw std::invalid_argument("GpuMesh::upload: a LOD or subset runs past the last triangle");
            }
        }
    }

    keep_bounds(data);

    // A static mesh still fills the bone and weight attributes, with no influences.
    const bool skinned = !data.bones.empty();
    std::vector<GpuVertex> vertices(vertex_count);
    for (std::size_t i = 0; i < vertex_count; ++i) {
        const Vertex& in = data.vertices[i];
        GpuVertex& out = vertices[i];
        for (int k = 0; k < 3; ++k) {
            out.position[k] = in.p[k];
            out.normal[k] = in.n[k];
        }
        out.uv[0] = in.uv[0];
        out.uv[1] = in.uv[1];
        for (int k = 0; k < 4; ++k) {
            out.tangent[k] = in.t[k];
            out.color[k] = in.rgba[k];
            out.bone[k] = skinned ? in.bone[k] : kNoBone;
            out.weight[k] = skinned ? in.weight[k] : 0.0f;
        }
    }

    if (vao_ == 0) {
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &ebo_);
    }
    const GLenum usage = dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GpuVertex)), vertices.data(), usage);
    // The element buffer binding is VAO state, so it is bound with the VAO current.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.indices.size() * sizeof(std::uint32_t)),
                 data.indices.data(), usage);

    constexpr GLsizei stride = sizeof(GpuVertex);
    const auto at = [](std::size_t offset) { return reinterpret_cast<const void*>(offset); };
    for (const GLuint location : {kAttribPosition, kAttribNormal, kAttribUv, kAttribTangent, kAttribColor,
                                  kAttribBone, kAttribWeight}) {
        glEnableVertexAttribArray(location);
    }
    glVertexAttribPointer(kAttribPosition, 3, GL_FLOAT, GL_FALSE, stride, at(offsetof(GpuVertex, position)));
    glVertexAttribPointer(kAttribNormal, 3, GL_FLOAT, GL_FALSE, stride, at(offsetof(GpuVertex, normal)));
    glVertexAttribPointer(kAttribUv, 2, GL_FLOAT, GL_FALSE, stride, at(offsetof(GpuVertex, uv)));
    glVertexAttribPointer(kAttribTangent, 4, GL_FLOAT, GL_FALSE, stride, at(offsetof(GpuVertex, tangent)));
    glVertexAttribPointer(kAttribColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, at(offsetof(GpuVertex, color)));
    glVertexAttribIPointer(kAttribBone, 4, GL_UNSIGNED_SHORT, stride, at(offsetof(GpuVertex, bone)));
    glVertexAttribPointer(kAttribWeight, 4, GL_FLOAT, GL_FALSE, stride, at(offsetof(GpuVertex, weight)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    lods_ = std::move(lods);
    subsets_ = std::move(subsets);
}

void GpuMesh::bind() const {
    glBindVertexArray(vao_);
}

void GpuMesh::draw_range(std::uint32_t tri_begin, std::uint32_t tri_count) const {
    if (tri_count == 0) {
        return;
    }
    const std::size_t first_index = std::size_t{tri_begin} * 3;
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(std::size_t{tri_count} * 3), GL_UNSIGNED_INT,
                   reinterpret_cast<const void*>(first_index * sizeof(std::uint32_t)));
}

void GpuMesh::destroy() {
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        glDeleteBuffers(1, &vbo_);
        glDeleteBuffers(1, &ebo_);
        vao_ = vbo_ = ebo_ = 0;
    }
    lods_.clear();
    subsets_.clear();
}

#endif

}  // namespace anarchy::amesh
