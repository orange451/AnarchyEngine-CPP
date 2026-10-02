#include "ShadowPlanner.hpp"

#include "RenderMath.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace runner {
namespace {

using engine_core::Matrix4;
using engine_core::Vec3;

// 64-bit FNV-1a over the bytes of what is added.
class Hasher {
public:
    template <typename T>
    void add(const T& value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            value_ ^= bytes[i];
            value_ *= 1099511628211ull;
        }
    }
    std::uint64_t value() const { return value_; }

private:
    std::uint64_t value_ = 1469598103934665603ull;
};

// Each caster's own fingerprint, worked out once a call: which geometry, which
// upload of it, and where. A light or cascade folds in these, not their bytes.
void HashCasters(const std::vector<ShadowCaster>& casters, std::vector<std::uint64_t>& out) {
    out.resize(casters.size());
    for (std::size_t c = 0; c < casters.size(); ++c) {
        Hasher hash;
        hash.add(casters[c].mesh);
        hash.add(casters[c].revision);
        hash.add(casters[c].model);
        out[c] = hash.value();
    }
}

}  // namespace

int TileSizeFor(float reach, int previous, int paneHeight, const ShadowSettings& settings) {
    const float h = settings.hysteresis;
    if (!(reach >= settings.minReach * (previous > 0 ? 1.f - h : 1.f))) {
        return 0;
    }
    const float ideal = reach * static_cast<float>(paneHeight) * settings.texelsPerPixel;
    if (previous > 0) {
        const int kept = std::clamp(previous, settings.minTile, settings.maxTile);
        if (ideal > static_cast<float>(kept) * 0.5f * (1.f - h) && ideal <= static_cast<float>(kept) * (1.f + h)) {
            return kept;
        }
    }
    int size = settings.minTile;
    while (static_cast<float>(size) < ideal && size < settings.maxTile) {
        size *= 2;
    }
    return size;
}

void ShadowPlanner::releaseTiles(Record& record) {
    for (int t = 0; t < record.tileCount; ++t) {
        atlas_.release(record.tiles[t]);
        record.tiles[t] = {};
    }
    record.tileCount = 0;
    record.size = 0;
    record.ready = false;
    record.waited = 0;
    std::fill(record.matches, record.matches + 6, false);
}

void ShadowPlanner::resetAtlas(int size, int minTile) {
    atlas_.reset(size, minTile);
    for (auto& [key, record] : records_) {
        for (AtlasTile& tile : record.tiles) {
            tile = {};
        }
        record.tileCount = 0;
        record.size = 0;
        record.ready = false;
        record.waited = 0;
        std::fill(record.matches, record.matches + 6, false);
    }
}

void ShadowPlanner::clear() {
    records_.clear();
    pending_.clear();
    atlas_.reset(0, 0);
    cascadeReady_ = false;
    cascadePending_ = false;
}

bool ShadowPlanner::allocate(const std::vector<ShadowRequest>& requests, const std::vector<int>& order,
                             const std::vector<int>& wanted, const ShadowSettings& settings) {
    // Each light whose wish changed gives its tiles back first, so the others can use them.
    for (std::size_t i = 0; i < requests.size(); ++i) {
        Record& record = records_.at(requests[i].key);
        if (record.wanted != wanted[i]) {
            releaseTiles(record);
            record.wanted = wanted[i];
        }
    }
    bool all = true;
    for (const int i : order) {
        Record& record = records_.at(requests[static_cast<std::size_t>(i)].key);
        // Kept, even below its wish: retrying every frame would redraw it every frame.
        if (record.size > 0 || record.wanted == 0) {
            all = all && record.size >= record.wanted;
            continue;
        }
        const int tileCount = record.kind == ShadowKind::Spot ? 1 : 6;
        for (int size = record.wanted; size >= settings.minTile && record.size == 0; size /= 2) {
            int got = 0;
            for (; got < tileCount; ++got) {
                record.tiles[got] = atlas_.allocate(size);
                if (record.tiles[got].size == 0) {
                    break;
                }
            }
            if (got == tileCount) {
                record.size = size;
                record.tileCount = tileCount;
            } else {
                for (int t = 0; t < got; ++t) {
                    atlas_.release(record.tiles[t]);
                    record.tiles[t] = {};
                }
            }
        }
        std::fill(record.dirty, record.dirty + 6, true);
        std::fill(record.matches, record.matches + 6, false);
        record.ready = false;
        record.waited = 0;
        all = all && record.size >= record.wanted;
    }
    return all;
}

ShadowPlan ShadowPlanner::plan(const std::vector<ShadowRequest>& requests, const std::vector<ShadowCaster>& casters,
                               const CameraView& camera, const ShadowSettings& settings) {
    ShadowPlan out;
    pending_.clear();
    if (atlas_.atlasSize() == 0) {
        resetAtlas(settings.atlasMinSize, settings.minTile);
        out.atlasResized = true;
    }

    // How big each light looks, and the tile that earns it.
    const Vec3 eye = engine_core::matrix4_position(camera.world);
    const std::size_t count = requests.size();
    std::vector<float> reach(count);
    std::vector<int> wanted(count);
    for (auto& [key, record] : records_) {
        record.seen = false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const ShadowRequest& request = requests[i];
        Record& record = records_[request.key];
        record.seen = true;
        if (record.kind != request.kind) {
            releaseTiles(record);
            record.kind = request.kind;
            record.wanted = 0;
        }
        reach[i] = ProjectedReach(request.position, request.radius, eye, camera.fovYDegrees);
        wanted[i] = TileSizeFor(reach[i], record.wanted, camera.paneHeight, settings);
    }
    // Lights gone since last frame give their tiles back.
    for (auto it = records_.begin(); it != records_.end();) {
        if (!it->second.seen) {
            releaseTiles(it->second);
            it = records_.erase(it);
        } else {
            ++it;
        }
    }

    std::vector<int> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return reach[a] > reach[b]; });
    if (!allocate(requests, order, wanted, settings) && atlas_.atlasSize() < settings.atlasMaxSize) {
        resetAtlas(atlas_.atlasSize() * 2, settings.minTile);
        out.atlasResized = true;
        allocate(requests, order, wanted, settings);
    }

    // What each map should be drawn from: the light, its tile, and every
    // caster in its reach. A PointLight's cube is world-aligned, so where it
    // points and its cone do not change its map.
    HashCasters(casters, casterHashes_);
    for (std::size_t i = 0; i < count; ++i) {
        const ShadowRequest& request = requests[i];
        Record& record = records_.at(request.key);
        if (record.size == 0) {
            continue;
        }
        Hasher hash;
        hash.add(request.kind);
        hash.add(request.position);
        if (request.kind == ShadowKind::Spot) {
            hash.add(request.direction);
            hash.add(request.outerFovDegrees);
        }
        hash.add(request.radius);
        hash.add(record.size);
        const Sphere reachSphere{request.position, request.radius};
        for (std::size_t c = 0; c < casters.size(); ++c) {
            const ShadowCaster& caster = casters[c];
            if ((request.owner != 0 && caster.owner == request.owner) || !SpheresTouch(reachSphere, caster.bounds)) {
                continue;
            }
            hash.add(casterHashes_[c]);
        }
        record.wantHash = hash.value();
        if (!request.cached || record.wantHash != record.hash) {
            std::fill(record.dirty, record.dirty + 6, true);
        }
    }

    // Whether the camera can see a kind's face at position, radius: the same
    // test used both to decide what must draw and what is due to draw.
    const Frustum view = MakeFrustum(camera.viewProjection);
    auto faceVisible = [&](ShadowKind kind, Vec3 position, float radius, int face) {
        return kind == ShadowKind::Spot ? SphereInFrustum(view, Sphere{position, radius})
                                         : CubeFaceVisible(view, position, radius, face);
    };
    // Not ready, or a face the camera can see was never actually drawn from
    // the map it has now: reading that face would be a misread, so it must
    // draw before anything else, cap or no cap. Computed once per light here
    // (not per comparison, and not again in the loop below).
    std::vector<bool> must(count);
    std::vector<int> tier(count);
    for (std::size_t i = 0; i < count; ++i) {
        const ShadowRequest& request = requests[i];
        const Record& record = records_.at(request.key);
        bool mustDraw = !record.ready;
        if (!mustDraw) {
            const int faceLimit = request.kind == ShadowKind::Spot ? 1 : 6;
            for (int face = 0; face < faceLimit && !mustDraw; ++face) {
                mustDraw = !record.matches[face] &&
                           faceVisible(request.kind, request.position, request.radius, face);
            }
        }
        must[i] = mustDraw;
        tier[i] = mustDraw ? 0 : (record.waited >= kMaxShadowWait ? 1 : 2);
    }

    // Lights that must draw first, then ones that have lost the cap
    // kMaxShadowWait frames running, then the rest; biggest look first
    // within each (order is already sorted that way, and stable_sort keeps it).
    std::vector<int> drawOrder = order;
    std::stable_sort(drawOrder.begin(), drawOrder.end(), [&](int a, int b) {
        return tier[static_cast<std::size_t>(a)] < tier[static_cast<std::size_t>(b)];
    });
    std::int64_t spent = 0;
    for (const int i : drawOrder) {
        const ShadowRequest& request = requests[static_cast<std::size_t>(i)];
        Record& record = records_.at(request.key);
        if (record.size == 0) {
            continue;
        }
        // Unreadable until this light's own commit: a scheduled draw can
        // still fail to reach commit() (the frame's draw calls may fail, or
        // the plan may simply never be committed), and until then a visible
        // face it must draw has not actually been drawn from drawn. A light
        // that is not cached is unreadable the same way: its key is only its
        // place in this frame's list, so last frame's map at that key may be
        // another light's. It keeps its place in the order, though, so an
        // aged light still gets past the cap ahead of it.
        record.readable = !must[static_cast<std::size_t>(i)] && request.cached;

        // The faces due that the camera can see. One it cannot is never read, so it waits.
        int faces[6];
        int faceCount = 0;
        const int faceLimit = request.kind == ShadowKind::Spot ? 1 : 6;
        for (int face = 0; face < faceLimit; ++face) {
            if (record.dirty[face] && faceVisible(request.kind, request.position, request.radius, face)) {
                faces[faceCount++] = face;
            }
        }
        if (faceCount == 0) {
            continue;
        }
        const std::int64_t cost = static_cast<std::int64_t>(faceCount) * record.size * record.size;
        if (spent > 0 && spent + cost > settings.maxTexelsPerFrame) {
            ++record.waited;
            continue;
        }
        spent += cost;

        LocalShadow shadow;
        shadow.kind = request.kind;
        std::copy(record.tiles, record.tiles + 6, shadow.tiles);
        shadow.position = request.position;
        shadow.nearZ = ShadowNear(request.radius);
        shadow.farZ = request.radius;
        Matrix4 matrices[6];
        if (request.kind == ShadowKind::Spot) {
            const SpotShadow spot =
                SpotShadowFor(request.position, request.direction, request.outerFovDegrees, request.radius, record.size);
            shadow.viewProjection = spot.viewProjection;
            shadow.texelPerDistance = spot.texelPerDistance;
            matrices[0] = spot.viewProjection;
        } else {
            shadow.faceScale = static_cast<float>(record.size - 2 * kTileGuard) / static_cast<float>(record.size);
            const auto cube = CubeFaceViewProjections(request.position, request.radius, shadow.faceScale);
            std::copy(cube.begin(), cube.end(), matrices);
            shadow.texelPerDistance = 2.f / (shadow.faceScale * static_cast<float>(record.size));
        }
        Pending pending;
        pending.key = request.key;
        pending.shadow = shadow;
        pending.hash = record.wantHash;
        for (int k = 0; k < faceCount; ++k) {
            const int face = faces[k];
            TileDraw draw;
            draw.key = request.key;
            draw.face = face;
            draw.tile = record.tiles[face];
            draw.viewProjection = matrices[face];
            const Frustum tileView = MakeFrustum(matrices[face]);
            for (std::size_t c = 0; c < casters.size(); ++c) {
                if (request.owner != 0 && casters[c].owner == request.owner) {
                    continue;
                }
                if (SphereInFrustum(tileView, casters[c].bounds)) {
                    draw.casters.push_back(static_cast<int>(c));
                }
            }
            out.draws.push_back(std::move(draw));
            pending.faces[pending.faceCount++] = face;
        }
        pending_.push_back(pending);
    }
    return out;
}

void ShadowPlanner::commit() {
    for (const Pending& pending : pending_) {
        const auto it = records_.find(pending.key);
        if (it == records_.end()) {
            continue;
        }
        Record& record = it->second;
        record.drawn = pending.shadow;
        record.hash = pending.hash;
        record.ready = true;
        record.readable = true;
        record.waited = 0;
        bool drawnFace[6] = {};
        for (int k = 0; k < pending.faceCount; ++k) {
            drawnFace[pending.faces[k]] = true;
            record.dirty[pending.faces[k]] = false;
        }
        // Drawn faces now match the map just committed; a face left dirty
        // and undrawn does not (drawn's state moved on without it); a face
        // that was neither drawn nor dirty was already fine, and stays so.
        const int faceLimit = record.kind == ShadowKind::Spot ? 1 : 6;
        for (int face = 0; face < faceLimit; ++face) {
            if (drawnFace[face]) {
                record.matches[face] = true;
            } else if (record.dirty[face]) {
                record.matches[face] = false;
            }
        }
    }
    pending_.clear();
}

const LocalShadow* ShadowPlanner::find(std::uint64_t key) const {
    const auto it = records_.find(key);
    return it != records_.end() && it->second.ready && it->second.size > 0 && it->second.readable ? &it->second.drawn
                                                                                                    : nullptr;
}

std::vector<CascadeDraw> ShadowPlanner::planCascades(const SunRequest* sun, const std::vector<ShadowCaster>& casters,
                                                     const CameraView& camera, const ShadowSettings& settings) {
    std::vector<CascadeDraw> draws;
    cascadePending_ = false;
    if (sun == nullptr || !(sun->shadowDistance > camera.nearZ)) {
        cascadeReady_ = false;
        return draws;
    }
    const int count = std::clamp(settings.cascadeCount, 1, kMaxCascades);
    float splits[kMaxCascades + 1];
    CascadeSplits(camera.nearZ, sun->shadowDistance, count, settings.cascadeLambda, splits);
    const float pullLimit = sun->shadowDistance * settings.cascadePullLimit;
    CascadeShadow next;
    next.count = count;
    Hasher hash;
    HashCasters(casters, casterHashes_);
    for (int i = 0; i < count; ++i) {
        const Sphere slice =
            FrustumSliceSphere(camera.world, camera.fovYDegrees, camera.aspect, splits[i], splits[i + 1]);
        const CascadeFit box = FitCascade(slice, sun->shine, settings.cascadeSize);
        const Frustum boxView = MakeFrustum(box.viewProjection);
        // How far toward the sun to reach, so a caster outside the slice that
        // shades it is still drawn. The sun's own meshes cast nothing for it.
        float pull = 0.f;
        for (const ShadowCaster& caster : casters) {
            if ((sun->owner != 0 && caster.owner == sun->owner) || !SphereInFrustum(boxView, caster.bounds, true)) {
                continue;
            }
            const float nearest =
                -engine_core::matrix4_point(box.lightView, caster.bounds.center).z - caster.bounds.radius;
            pull = std::max(pull, box.nearDepth - nearest);
        }
        const CascadeFit fit = FitCascade(slice, sun->shine, settings.cascadeSize, std::min(pull, pullLimit));
        const Frustum fitView = MakeFrustum(fit.viewProjection);
        hash.add(fit.viewProjection);
        // Kept from frame to frame, so a frame that changes nothing allocates nothing.
        std::vector<int>& inside = cascadeCasters_[i];
        inside.clear();
        for (std::size_t c = 0; c < casters.size(); ++c) {
            if ((sun->owner == 0 || casters[c].owner != sun->owner) && SphereInFrustum(fitView, casters[c].bounds)) {
                inside.push_back(static_cast<int>(c));
                hash.add(casterHashes_[c]);
            }
        }
        next.viewProjection[i] = fit.viewProjection;
        next.texelWorld[i] = fit.texelWorld;
    }
    if (cascadeReady_ && hash.value() == cascadeHash_) {
        return draws;
    }
    for (int i = 0; i < count; ++i) {
        CascadeDraw draw;
        draw.layer = i;
        draw.viewProjection = next.viewProjection[i];
        draw.casters = cascadeCasters_[i];
        draws.push_back(std::move(draw));
    }
    pendingCascade_ = next;
    pendingCascadeHash_ = hash.value();
    cascadePending_ = true;
    return draws;
}

void ShadowPlanner::commitCascades() {
    if (!cascadePending_) {
        return;
    }
    cascade_ = pendingCascade_;
    cascadeHash_ = pendingCascadeHash_;
    cascadeReady_ = true;
    cascadePending_ = false;
}

}  // namespace runner
