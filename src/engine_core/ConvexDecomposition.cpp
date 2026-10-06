#include "ConvexDecomposition.hpp"

#pragma warning(push, 0)
#define ENABLE_VHACD_IMPLEMENTATION 1
#include "VHACD.h"
#pragma warning(pop)

#include <utility>

namespace engine_core {

std::vector<anarchy::amesh::ConvexPiece> decompose(const std::vector<Vec3>& points,
                                                   const std::vector<std::uint32_t>& triangles) {
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

}  // namespace engine_core
