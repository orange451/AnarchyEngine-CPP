#include "BrushVisuals.hpp"

#include "AssetInstances.hpp"
#include "Brush.hpp"
#include "DataModel.hpp"
#include "Matrix4.hpp"
#include "SnapshotPump.hpp"
#include "brush/BrushGeometry.hpp"
#include "profiler/Profiler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine_core {

namespace {

using anarchy::amesh::Data;
using anarchy::amesh::LodRange;
using anarchy::amesh::Vertex;

// FNV-1a over raw bytes: a change detector, not a key anyone stores.
struct Hash {
    std::uint64_t value = 1469598103934665603ull;
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            value = (value ^ p[i]) * 1099511628211ull;
        }
    }
    template <class T>
    void add(const T& item) {
        bytes(&item, sizeof(item));
    }
};

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.f, 1.f) * 255.f));
}

float unit(double value) { return static_cast<float>(std::clamp(value, 0.0, 1.0)); }

// One mesh in the making: triangles grouped by Material, then flattened with
// LOD 0 the whole mesh and LOD i + 1 Material i's triangles.
struct Builder {
    std::vector<Vertex> vertices;
    std::vector<std::string> materials;
    std::vector<std::vector<std::uint32_t>> indices;

    std::vector<std::uint32_t>& group(const std::string& guid) {
        for (std::size_t i = 0; i < materials.size(); ++i) {
            if (materials[i] == guid) {
                return indices[i];
            }
        }
        materials.push_back(guid);
        indices.emplace_back();
        return indices.back();
    }

    // Appends a Brush's mesh, through world (identity for its own space).
    // scales[range] divides that range's UVs (the Material's TextureScale).
    void append(const brush::Mesh& mesh, const Matrix4* world, ColorRgb color, const std::vector<float>& scales) {
        const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
        Matrix4 normal_matrix = matrix4_identity();
        bool mirrored = false;
        if (world != nullptr) {
            const Matrix4 inverse = matrix4_inverse(*world);
            // The inverse's transpose, 3x3: normals stay normal under scale.
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    normal_matrix.m[c * 4 + r] = inverse.m[r * 4 + c];
                }
            }
            const float* m = world->m;
            const float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                              m[8] * (m[1] * m[6] - m[5] * m[2]);
            mirrored = det < 0.f;
        }
        const std::uint8_t rgba[4] = {to_byte(color.r), to_byte(color.g), to_byte(color.b), 255};
        // Which range each vertex is in, for its UV scale.
        std::vector<float> vertex_scale(mesh.vertices.size(), 1.f);
        for (std::size_t r = 0; r < mesh.ranges.size(); ++r) {
            const brush::MeshRange& range = mesh.ranges[r];
            for (std::uint32_t i = range.first_index; i < range.first_index + range.index_count; ++i) {
                vertex_scale[mesh.indices[i]] = r < scales.size() ? scales[r] : 1.f;
            }
        }
        vertices.reserve(vertices.size() + mesh.vertices.size());
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const brush::MeshVertex& in = mesh.vertices[i];
            Vertex out;
            Vec3 p{in.position[0], in.position[1], in.position[2]};
            Vec3 n{in.normal[0], in.normal[1], in.normal[2]};
            Vec3 t{in.tangent[0], in.tangent[1], in.tangent[2]};
            float w = in.tangent[3];
            if (world != nullptr) {
                p = matrix4_point(*world, p);
                n = matrix4_vector(normal_matrix, n);
                t = matrix4_vector(*world, t);
                const float nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                const float tl = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
                if (nl > 0.f) {
                    n = {n.x / nl, n.y / nl, n.z / nl};
                }
                if (tl > 0.f) {
                    t = {t.x / tl, t.y / tl, t.z / tl};
                }
                if (mirrored) {
                    w = -w;
                }
            }
            const float scale = vertex_scale[i] > 0.f ? vertex_scale[i] : 1.f;
            out.p[0] = p.x;
            out.p[1] = p.y;
            out.p[2] = p.z;
            out.n[0] = n.x;
            out.n[1] = n.y;
            out.n[2] = n.z;
            out.uv[0] = in.uv[0] / scale;
            out.uv[1] = in.uv[1] / scale;
            out.t[0] = t.x;
            out.t[1] = t.y;
            out.t[2] = t.z;
            out.t[3] = w;
            std::memcpy(out.rgba, rgba, 4);
            vertices.push_back(out);
        }
        for (const brush::MeshRange& range : mesh.ranges) {
            std::vector<std::uint32_t>& out = group(range.material);
            for (std::uint32_t i = range.first_index; i < range.first_index + range.index_count; i += 3) {
                // A mirroring transform turns triangles inside out; swap two corners.
                out.push_back(base + mesh.indices[i]);
                out.push_back(base + mesh.indices[mirrored ? i + 2 : i + 1]);
                out.push_back(base + mesh.indices[mirrored ? i + 1 : i + 2]);
            }
        }
    }

    std::shared_ptr<const Data> finish() {
        auto data = std::make_shared<Data>();
        data->vertices = std::move(vertices);
        std::uint32_t total = 0;
        for (const auto& group : indices) {
            total += static_cast<std::uint32_t>(group.size());
        }
        data->indices.reserve(total);
        data->lods.push_back(LodRange{0, total / 3});
        for (const auto& group : indices) {
            data->lods.push_back(LodRange{static_cast<std::uint32_t>(data->indices.size() / 3),
                                          static_cast<std::uint32_t>(group.size() / 3)});
            data->indices.insert(data->indices.end(), group.begin(), group.end());
        }
        anarchy::amesh::compute_aabb(*data);
        return data;
    }
};

}  // namespace

const Material* BrushVisuals::material(const DataModel& game, const std::string& guid) {
    if (guid.empty()) {
        return nullptr;
    }
    auto found = materials_.find(guid);
    if (found != materials_.end() && game.alive(found->second) && game.has_guid(found->second, guid)) {
        return dynamic_cast<const Material*>(game.instance(found->second));
    }
    const std::optional<InstanceId> id = game.find_guid(guid);
    materials_[guid] = id ? *id : 0;
    return id ? dynamic_cast<const Material*>(game.instance(*id)) : nullptr;
}

void BrushVisuals::update(DataModel& game, std::vector<VisualBrushDraw>& out) {
    PROFILE_SCOPE("Brush visuals", profiler::Group::Engine);
    out.clear();
    ids_.clear();
    game.physics_bodies(ids_);
    for (auto& [key, members] : members_) {
        members.clear();
    }
    for (auto& [id, single] : singles_) {
        single.seen = false;
    }
    std::vector<float> scales;
    const auto texture_scales = [&](const brush::Mesh& mesh) {
        scales.clear();
        for (const brush::MeshRange& range : mesh.ranges) {
            const Material* look = material(game, range.material);
            scales.push_back(static_cast<float>(look != nullptr ? look->texture_scale() : Material::kDefaultTextureScale));
        }
    };
    const auto opaque = [&](const Brush& brush) {
        if (brush.transparency() > 0.0) {
            return false;
        }
        for (const brush::Face& face : brush.faces()) {
            const Material* look = material(game, face.material);
            if (look != nullptr && look->transparency() > 0.0) {
                return false;
            }
        }
        return true;
    };

    // Sort the brushes into cells and singles.
    for (InstanceId id : ids_) {
        const auto* brush = dynamic_cast<const Brush*>(game.instance(id));
        if (brush == nullptr) {
            continue;
        }
        if (brush->anchored() && opaque(*brush)) {
            const brush::Shape& shape = brush->shape();
            const Vec3 middle = matrix4_point(brush->transform(),
                                              Vec3{static_cast<float>((shape.min.x + shape.max.x) * 0.5),
                                                   static_cast<float>((shape.min.y + shape.max.y) * 0.5),
                                                   static_cast<float>((shape.min.z + shape.max.z) * 0.5)});
            const CellKey key{static_cast<std::int64_t>(std::floor(middle.x / kCellSize)),
                              static_cast<std::int64_t>(std::floor(middle.y / kCellSize)),
                              static_cast<std::int64_t>(std::floor(middle.z / kCellSize))};
            members_[key].push_back(id);
            continue;
        }
        Baked& single = singles_[id];
        single.seen = true;
        std::shared_ptr<const brush::Mesh> mesh = brush->mesh();
        texture_scales(*mesh);
        Hash hash;
        hash.add(brush->revision());
        hash.bytes(scales.data(), scales.size() * sizeof(float));
        if (single.mesh == nullptr || single.signature != hash.value) {
            Builder builder;
            builder.append(*mesh, nullptr, brush->color(), scales);
            single.materials = builder.materials;
            single.mesh = builder.finish();
            single.signature = hash.value;
            single.revision = brush::next_revision();
            ++bakes_;
        }
        for (std::size_t i = 0; i < single.materials.size(); ++i) {
            VisualBrushDraw& draw = out.emplace_back();
            fill_visual_material(game, material(game, single.materials[i]), draw.look);
            draw.look.session = single.mesh;
            draw.look.revision = single.revision;
            draw.lod = static_cast<std::uint32_t>(i + 1);
            draw.world = brush->transform();
            draw.transparency = unit(brush->transparency());
            draw.casts_shadow = i == 0;
            draw.owner = id;
        }
    }
    for (auto it = singles_.begin(); it != singles_.end();) {
        it = it->second.seen ? std::next(it) : singles_.erase(it);
    }

    // Bake the cells whose brushes changed.
    for (auto it = members_.begin(); it != members_.end();) {
        std::vector<InstanceId>& members = it->second;
        if (members.empty()) {
            cells_.erase(it->first);
            it = members_.erase(it);
            continue;
        }
        std::sort(members.begin(), members.end());
        Hash hash;
        for (InstanceId id : members) {
            const auto* brush = static_cast<const Brush*>(game.instance(id));
            hash.add(id);
            hash.add(brush->revision());
            hash.bytes(brush->transform().m, sizeof(float) * 16);
            for (const brush::Face& face : brush->faces()) {
                if (const Material* look = material(game, face.material)) {
                    hash.add(look->texture_scale());
                }
            }
        }
        Baked& cell = cells_[it->first];
        if (cell.mesh == nullptr || cell.signature != hash.value) {
            Builder builder;
            for (InstanceId id : members) {
                const auto* brush = static_cast<const Brush*>(game.instance(id));
                std::shared_ptr<const brush::Mesh> mesh = brush->mesh();
                texture_scales(*mesh);
                const Matrix4 world = brush->transform();
                builder.append(*mesh, &world, brush->color(), scales);
            }
            cell.materials = builder.materials;
            cell.mesh = builder.finish();
            cell.signature = hash.value;
            cell.revision = brush::next_revision();
            ++bakes_;
        }
        for (std::size_t i = 0; i < cell.materials.size(); ++i) {
            VisualBrushDraw& draw = out.emplace_back();
            fill_visual_material(game, material(game, cell.materials[i]), draw.look);
            draw.look.session = cell.mesh;
            draw.look.revision = cell.revision;
            draw.lod = static_cast<std::uint32_t>(i + 1);
            draw.casts_shadow = i == 0;
        }
        ++it;
    }
}

}  // namespace engine_core
