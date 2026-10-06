#include "ConvexDecomposition.hpp"

#include "AssetInstances.hpp"

#pragma warning(push, 0)
#define ENABLE_VHACD_IMPLEMENTATION 1
#include "VHACD.h"
#pragma warning(pop)

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

namespace engine_core {

namespace {

constexpr std::size_t kCachedMeshes = 64;

std::atomic<std::uint64_t> decompositions{0};

struct Cache {
    std::mutex mutex;
    std::deque<std::pair<std::uint64_t, std::vector<anarchy::amesh::ConvexPiece>>> entries;
};

Cache& cache() {
    static Cache instance;
    return instance;
}

// FNV-1a over the points, the triangles, and the recipe.
std::uint64_t geometry_key(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        }
    };
    for (const Vec3& p : points) {
        const float xyz[3] = {p.x, p.y, p.z};
        mix(xyz, sizeof(xyz));
    }
    mix(triangles.data(), triangles.size() * sizeof(std::uint32_t));
    mix(&kRecipe, sizeof(kRecipe));
    return hash;
}

}  // namespace

std::vector<anarchy::amesh::ConvexPiece> decompose(const std::vector<Vec3>& points,
                                                   const std::vector<std::uint32_t>& triangles) {
    ++decompositions;
    std::vector<anarchy::amesh::ConvexPiece> pieces;
    if (points.empty() || triangles.size() < 3) {
        return pieces;
    }
    std::vector<float> flat;
    flat.reserve(points.size() * 3);
    for (const Vec3& p : points) {
        flat.push_back(p.x);
        flat.push_back(p.y);
        flat.push_back(p.z);
    }
    // The settings kRecipe names.
    VHACD::IVHACD::Parameters parameters;
    parameters.m_maxConvexHulls = 32;
    parameters.m_resolution = 100000;
    parameters.m_maxNumVerticesPerCH = 64;
    parameters.m_minimumVolumePercentErrorAllowed = 1;
    parameters.m_fillMode = VHACD::FillMode::FLOOD_FILL;
    parameters.m_shrinkWrap = true;
    // The caller picks the thread.
    parameters.m_asyncACD = false;

    VHACD::IVHACD* vhacd = VHACD::CreateVHACD();
    if (vhacd->Compute(flat.data(), static_cast<std::uint32_t>(points.size()), triangles.data(),
                       static_cast<std::uint32_t>(triangles.size() / 3), parameters)) {
        for (std::uint32_t index = 0; index < vhacd->GetNConvexHulls(); ++index) {
            VHACD::IVHACD::ConvexHull hull;
            if (!vhacd->GetConvexHull(index, hull) || hull.m_points.size() < anarchy::amesh::kMinPiecePoints) {
                continue;
            }
            anarchy::amesh::ConvexPiece piece;
            for (const VHACD::Vertex& v : hull.m_points) {
                if (piece.points.size() == anarchy::amesh::kMaxPiecePoints) {
                    break;
                }
                piece.points.push_back(
                    {static_cast<float>(v.mX), static_cast<float>(v.mY), static_cast<float>(v.mZ)});
            }
            pieces.push_back(std::move(piece));
            if (pieces.size() == anarchy::amesh::kMaxPieces) {
                break;
            }
        }
    }
    vhacd->Clean();
    vhacd->Release();
    return pieces;
}

void remember_pieces(const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                     std::vector<anarchy::amesh::ConvexPiece> pieces) {
    const std::uint64_t key = geometry_key(points, triangles);
    Cache& kept = cache();
    std::lock_guard<std::mutex> lock(kept.mutex);
    for (auto& entry : kept.entries) {
        if (entry.first == key) {
            entry.second = std::move(pieces);
            return;
        }
    }
    kept.entries.emplace_back(key, std::move(pieces));
    if (kept.entries.size() > kCachedMeshes) {
        kept.entries.pop_front();
    }
}

bool known_pieces(const Mesh& mesh, const std::vector<Vec3>& points, const std::vector<std::uint32_t>& triangles,
                  std::vector<anarchy::amesh::ConvexPiece>& out) {
    if (mesh.file_pieces(kRecipe, out)) {
        return true;
    }
    const std::uint64_t key = geometry_key(points, triangles);
    Cache& kept = cache();
    std::lock_guard<std::mutex> lock(kept.mutex);
    for (const auto& entry : kept.entries) {
        if (entry.first == key) {
            out = entry.second;
            return true;
        }
    }
    out.clear();
    return false;
}

std::vector<anarchy::amesh::ConvexPiece> pieces_for(const Mesh& mesh, const std::vector<Vec3>& points,
                                                    const std::vector<std::uint32_t>& triangles) {
    std::vector<anarchy::amesh::ConvexPiece> pieces;
    if (known_pieces(mesh, points, triangles, pieces)) {
        return pieces;
    }
    pieces = decompose(points, triangles);
    remember_pieces(points, triangles, pieces);
    return pieces;
}

std::uint64_t decompose_count() { return decompositions.load(); }

void clear_piece_cache() {
    Cache& kept = cache();
    std::lock_guard<std::mutex> lock(kept.mutex);
    kept.entries.clear();
}

}  // namespace engine_core
