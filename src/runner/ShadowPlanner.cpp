#include "ShadowPlanner.hpp"

#include "ParallelBlocks.hpp"
#include "RenderMath.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace runner {
namespace {

using engine_core::Matrix4;
using engine_core::Vec3;

// How much wider than its slice a sun cascade is fitted, as a part of the
// slice's radius: its texels that much coarser, unseen, for a box that holds
// the slice while the camera moves that far, so the map is not drawn again.
constexpr float kCascadeSlack = 1.f / 16.f;

// A 64-bit hash of what is added, eight bytes at a step. Each step is a
// bijection of the running value for a given word, so two inputs that differ
// in one word never hash alike. Only ever compared with itself, frame to frame.
class Hasher {
public:
    template <typename T>
    void add(const T& value) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        std::size_t i = 0;
        for (; i + 8 <= sizeof(T); i += 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, bytes + i, 8);
            mix(word);
        }
        for (; i < sizeof(T); ++i) {
            mix(bytes[i]);
        }
    }
    std::uint64_t value() const { return value_; }

private:
    void mix(std::uint64_t word) {
        value_ = (value_ ^ word) * 0x9e3779b97f4a7c15ull;
        value_ ^= value_ >> 29;
    }

    std::uint64_t value_ = 1469598103934665603ull;
};

// Each caster's own fingerprint: which geometry, which upload of it, how it is
// posed, and where. A light or cascade folds in these, not their bytes. Worked
// out only for the casters a light or a cascade reaches: with hundreds of
// thousands of casters, hashing them all every frame cost more than the shadows.
std::uint64_t CasterHash(const ShadowCaster& caster) {
    Hasher hash;
    hash.add(caster.mesh);
    hash.add(caster.revision);
    hash.add(caster.pose);
    hash.add(caster.model);
    if (caster.cutout != 0) {
        hash.add(caster.cutout);
    }
    return hash.value();
}

int TileCount(ShadowKind kind) {
    return kind == ShadowKind::Spot ? 1 : 6;
}

std::int64_t Texels(int tiles, int size) {
    return static_cast<std::int64_t>(tiles) * size * size;
}

// Shrinks sizes (each light's tile size, tiles[i] tiles each) until they
// fit capacity texels: one light's size halved at a time, from the lowest
// priority in order up and round again, so the lights that look biggest
// keep their size longest. One at minTile is passed over; only once every
// light is there do the lowest go to 0, no shadow. None is bigger than a page.
void FitTiles(std::vector<int>& sizes, const std::vector<int>& tiles, const std::vector<int>& order,
              std::int64_t capacity, int pageSize, int minTile) {
    std::int64_t demand = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        while (sizes[i] > pageSize) {
            sizes[i] /= 2;
        }
        demand += Texels(tiles[i], sizes[i]);
    }
    bool halved = true;
    while (demand > capacity && halved) {
        halved = false;
        for (auto it = order.rbegin(); it != order.rend() && demand > capacity; ++it) {
            const auto i = static_cast<std::size_t>(*it);
            if (sizes[i] > minTile) {
                demand -= Texels(tiles[i], sizes[i]) - Texels(tiles[i], sizes[i] / 2);
                sizes[i] /= 2;
                halved = true;
            }
        }
    }
    for (auto it = order.rbegin(); it != order.rend() && demand > capacity; ++it) {
        const auto i = static_cast<std::size_t>(*it);
        demand -= Texels(tiles[i], sizes[i]);
        sizes[i] = 0;
    }
}

// Whether caster c casts nothing this frame: no fingerprint, or hidden.
bool CastsNothing(const CasterView& casters, std::size_t c) {
    return casters.fingerprints[c] == 0 || (casters.hidden != nullptr && casters.hidden[c] != 0);
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

void ShadowPlanner::resetAtlas(int pageSize, int pages, int minTile) {
    atlas_.reset(pageSize, pages, minTile);
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

void ShadowPlanner::forgetMaps() {
    for (auto& [key, record] : records_) {
        record.ready = false;
        record.readable = false;
        record.waited = 0;
        std::fill(record.dirty, record.dirty + 6, true);
        std::fill(record.matches, record.matches + 6, false);
    }
    pending_.clear();
}

void ShadowPlanner::clear() {
    records_.clear();
    pending_.clear();
    atlas_.reset(0, 0, 0);
    capSize_ = 0;
    capPages_ = 0;
    cascadeReady_ = false;
    cascadePending_ = false;
    heldReady_ = false;
    forgetCascadeCache();
}

bool ShadowPlanner::takeTiles(int size, int count, AtlasTile* tiles) {
    for (int got = 0; got < count; ++got) {
        tiles[got] = atlas_.allocate(size);
        if (tiles[got].size == 0) {
            for (int t = 0; t < got; ++t) {
                atlas_.release(tiles[t]);
            }
            return false;
        }
    }
    return true;
}

void ShadowPlanner::place(Record& record, int minTile) {
    // Kept: no change, no redraw.
    if (record.size == record.fitted) {
        return;
    }
    // A light with none takes the biggest size there is room for; one short
    // of its size moves only to a bigger one, so a frame with no more room
    // than the last changes nothing.
    const int tileCount = TileCount(record.kind);
    const int floor = record.size > 0 ? record.size * 2 : minTile;
    AtlasTile tiles[6];
    int size = record.fitted;
    while (size >= floor && !takeTiles(size, tileCount, tiles)) {
        size /= 2;
    }
    if (size < floor) {
        return;
    }
    for (int t = 0; t < record.tileCount; ++t) {
        atlas_.release(record.tiles[t]);
    }
    std::copy(tiles, tiles + 6, record.tiles);
    record.size = size;
    record.tileCount = tileCount;
    std::fill(record.dirty, record.dirty + 6, true);
    std::fill(record.matches, record.matches + 6, false);
    record.ready = false;
    record.waited = 0;
}

void ShadowPlanner::allocate(const std::vector<ShadowRequest>& requests, const std::vector<int>& order,
                             const std::vector<int>& fitted, const ShadowSettings& settings) {
    // Each light whose fitted size changed gives its tiles back first, so
    // the others can use them: unless it is a short light fitted down to
    // just what it holds.
    for (std::size_t i = 0; i < requests.size(); ++i) {
        Record& record = records_.at(requests[i].key);
        if (record.fitted != fitted[i]) {
            if (record.size != fitted[i]) {
                releaseTiles(record);
            }
            record.fitted = fitted[i];
        }
    }
    // Biggest tiles first: power-of-two squares taken largest to smallest
    // leave the quadtree no gaps, so sizes that fit the atlas's texels all
    // get their tiles. Lights that kept theirs can still break the free
    // blocks up; a light short of its size then takes a smaller one.
    std::vector<int> packing = order;
    std::stable_sort(packing.begin(), packing.end(), [&](int a, int b) {
        return fitted[static_cast<std::size_t>(a)] > fitted[static_cast<std::size_t>(b)];
    });
    const auto recordAt = [&](int i) -> Record& { return records_.at(requests[static_cast<std::size_t>(i)].key); };
    bool repack = false;
    for (const int i : packing) {
        Record& record = recordAt(i);
        place(record, settings.minTile);
        record.shortFor = record.size < record.fitted ? record.shortFor + 1 : 0;
        repack = repack || record.shortFor >= kShortFramesBeforeRepack;
    }
    if (!repack) {
        return;
    }
    // Held short long enough: the first short light in packing order and
    // every light after it give their tiles back. Every tile kept is then
    // at least as big as any given back, so the free space is whole blocks
    // of that size, and packing the rest again biggest first fits them all.
    // Not every frame, so a moving camera cannot make the atlas thrash.
    const auto first = std::find_if(packing.begin(), packing.end(), [&](int i) {
        const Record& record = recordAt(i);
        return record.size < record.fitted;
    });
    for (auto it = first; it != packing.end(); ++it) {
        releaseTiles(recordAt(*it));
    }
    for (auto it = first; it != packing.end(); ++it) {
        Record& record = recordAt(*it);
        place(record, settings.minTile);
        record.shortFor = 0;
    }
}

std::uint64_t ShadowCasterFingerprint(const ShadowCaster& caster) { return CasterHash(caster) | 1u; }

const CasterView& ShadowPlanner::viewOf(const std::vector<ShadowCaster>& casters) {
    viewBounds_.resize(casters.size());
    viewOwners_.resize(casters.size());
    viewFingerprints_.resize(casters.size());
    for (std::size_t c = 0; c < casters.size(); ++c) {
        viewBounds_[c] = casters[c].bounds;
        viewOwners_[c] = casters[c].owner;
        viewFingerprints_[c] = ShadowCasterFingerprint(casters[c]);
    }
    view_ = CasterView{casters.size(), viewBounds_.data(), viewOwners_.data(), viewFingerprints_.data()};
    return view_;
}

ShadowPlan ShadowPlanner::plan(const std::vector<ShadowRequest>& requests, const std::vector<ShadowCaster>& casters,
                               const CameraView& camera, const ShadowSettings& settings) {
    return planView(requests, viewOf(casters), camera, settings);
}

std::vector<CascadeDraw> ShadowPlanner::planCascades(const SunRequest* sun, const std::vector<ShadowCaster>& casters,
                                                     const CameraView& camera, const ShadowSettings& settings,
                                                     std::size_t grain) {
    return planCascadesView(sun, viewOf(casters), camera, settings, grain);
}

ShadowPlan ShadowPlanner::planView(const std::vector<ShadowRequest>& requests, const CasterView& casters,
                                   const CameraView& camera, const ShadowSettings& settings) {
    ShadowPlan out;
    pending_.clear();
    if (atlas_.pageSize() == 0) {
        resetAtlas(settings.atlasMinSize, 1, settings.minTile);
        out.atlasResized = true;
    }

    // How big each light looks, and the tile that earns it.
    const Vec3 eye = engine_core::matrix4_position(camera.world);
    const std::size_t count = requests.size();
    std::vector<float> reach(count);
    std::vector<int> wanted(count);
    std::vector<int> tiles(count);
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
            record.fitted = 0;
        }
        reach[i] = ProjectedReach(request.position, request.radius, eye, camera.fovYDegrees);
        wanted[i] = TileSizeFor(reach[i], record.wanted, camera.paneHeight, settings);
        record.wanted = wanted[i];
        tiles[i] = TileCount(request.kind);
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

    // Wanting more texels than the atlas has grows it, in one step, to the
    // smallest page and then the fewest pages that hold them, up to the
    // settings and any cap. Every map is drawn again after. Then, grown or
    // not, the sizes are fitted to it.
    std::int64_t demand = 0;
    for (std::size_t i = 0; i < count; ++i) {
        demand += Texels(tiles[i], wanted[i]);
    }
    const int maxSize = capSize_ > 0 ? std::min(settings.atlasMaxSize, capSize_) : settings.atlasMaxSize;
    const int maxPages = capPages_ > 0 ? std::min(settings.atlasMaxPages, capPages_) : settings.atlasMaxPages;
    int pageSize = atlas_.pageSize();
    int pages = atlas_.pageCount();
    if (pageSize > maxSize || pages > maxPages) {
        // Capped below what it has.
        pageSize = std::min(pageSize, maxSize);
        pages = std::min(pages, maxPages);
    } else {
        while (demand > Texels(pages, pageSize) && pageSize < maxSize) {
            pageSize *= 2;
        }
        while (demand > Texels(pages, pageSize) && pages < maxPages) {
            ++pages;
        }
    }
    if (pageSize != atlas_.pageSize() || pages != atlas_.pageCount()) {
        resetAtlas(pageSize, pages, settings.minTile);
        out.atlasResized = true;
    }
    std::vector<int> fitted = wanted;
    FitTiles(fitted, tiles, order, Texels(atlas_.pageCount(), atlas_.pageSize()), atlas_.pageSize(), settings.minTile);
    allocate(requests, order, fitted, settings);

    // What each map should be drawn from: the light, its tile, and every
    // caster in its reach. A PointLight's cube is world-aligned, so where it
    // points and its cone do not change its map.
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
        for (std::size_t k = 0, span = CasterSpan(casters); k < span; ++k) {
            const std::size_t c = CasterAt(casters, k);
            if (CastsNothing(casters, c) || !SpheresTouch(reachSphere, casters.bounds[c]) ||
                (request.owner != 0 && casters.owners[c] == request.owner)) {
                continue;
            }
            hash.add(casters.fingerprints[c]);
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
            for (std::size_t k = 0, span = CasterSpan(casters); k < span; ++k) {
                const std::size_t c = CasterAt(casters, k);
                if (CastsNothing(casters, c) || (request.owner != 0 && casters.owners[c] == request.owner)) {
                    continue;
                }
                if (SphereInFrustum(tileView, casters.bounds[c])) {
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

std::vector<CascadeDraw> ShadowPlanner::planCascadesView(const SunRequest* sun, const CasterView& casters,
                                                         const CameraView& camera, const ShadowSettings& settings,
                                                         std::size_t grain) {
    std::vector<CascadeDraw> draws;
    cascadePending_ = false;
    if (sun == nullptr || !(sun->shadowDistance > camera.nearZ)) {
        cascadeReady_ = false;
        return draws;
    }
    // Nothing it is made from has changed: the cascades drawn still hold.
    // Zeroed first and kept by copying bytes, so the padding compares too.
    CascadeInputs inputs;
    std::memset(&inputs, 0, sizeof(CascadeInputs));
    inputs.world = camera.world;
    inputs.fovYDegrees = camera.fovYDegrees;
    inputs.aspect = camera.aspect;
    inputs.nearZ = camera.nearZ;
    inputs.shine = sun->shine;
    inputs.shadowDistance = sun->shadowDistance;
    inputs.owner = sun->owner;
    inputs.cascadeCount = settings.cascadeCount;
    inputs.cascadeLambda = settings.cascadeLambda;
    inputs.cascadeSize = settings.cascadeSize;
    inputs.cascadePullLimit = settings.cascadePullLimit;
    inputs.casterCount = casters.count;
    inputs.casterRevision = casters.revision;
    if (cascadeReady_ && cascadeInputsKept_ && casters.revision != 0 &&
        std::memcmp(&inputs, &cascadeInputs_, sizeof(CascadeInputs)) == 0) {
        return draws;
    }
    std::memcpy(&cascadeInputs_, &inputs, sizeof(CascadeInputs));
    cascadeInputsKept_ = true;
    const int count = std::clamp(settings.cascadeCount, 1, kMaxCascades);
    float splits[kMaxCascades + 1];
    CascadeSplits(camera.nearZ, sun->shadowDistance, count, settings.cascadeLambda, splits);
    const float pullLimit = sun->shadowDistance * settings.cascadePullLimit;
    CascadeShadow next;
    next.count = count;
    // Every cascade sees the sun from the same turn (CascadeLightView), and
    // its frustum is a box in that view. So each caster is turned once, and
    // testing it against a cascade is comparing it with the box's six sides:
    // the same test as the frustum's six planes, without them.
    const engine_core::Matrix4 lightView = CascadeLightView(sun->shine);
    // Only the casters that can cast, when the view knows them: the same plan,
    // as the rest add nothing to a pull, a box, or the hash.
    const std::size_t casterCount = CasterSpan(casters);
    lit_.resize(casterCount);
    // Blocks of casters on worker threads, each pass joined in block order,
    // so the lists and the hash come out as one loop would make them.
    cascadeBlocks_.resize(BlockCount(casterCount, grain));
    ParallelBlocks(casterCount, grain, [&](std::size_t, std::size_t begin, std::size_t end) {
        for (std::size_t k = begin; k < end; ++k) {
            const std::size_t c = CasterAt(casters, k);
            const engine_core::Vec3 at = engine_core::matrix4_point(lightView, casters.bounds[c].center);
            lit_[k] = LitCaster{at.x, at.y, -at.z, casters.bounds[c].radius};
        }
    });
    const auto in_box = [](const LitCaster& lit, const CascadeFit& box, bool ignoreNear) {
        return SphereInCascadeBox(box, lit.x, lit.y, lit.depth, lit.radius, ignoreNear);
    };
    // The sun's own meshes cast nothing for it, nor does what casts nothing at all.
    const auto own = [&](std::size_t c) {
        return CastsNothing(casters, c) || (sun->owner != 0 && casters.owners[c] == sun->owner);
    };
    Sphere slices[kMaxCascades];
    CascadeFit boxes[kMaxCascades];
    for (int i = 0; i < count; ++i) {
        slices[i] = FrustumSliceSphere(camera.world, camera.fovYDegrees, camera.aspect, splits[i], splits[i + 1]);
        boxes[i] = FitCascade(slices[i], sun->shine, settings.cascadeSize);
    }
    // How far toward the sun each cascade reaches, so a caster outside its
    // slice that shades it is still drawn: the most any caster in it asks.
    ParallelBlocks(casterCount, grain, [&](std::size_t block, std::size_t begin, std::size_t end) {
        float* pull = cascadeBlocks_[block].pull;
        std::fill(pull, pull + kMaxCascades, 0.f);
        for (std::size_t k = begin; k < end; ++k) {
            if (own(CasterAt(casters, k))) {
                continue;
            }
            const LitCaster& lit = lit_[k];
            const float nearest = lit.depth - lit.radius;
            for (int i = 0; i < count; ++i) {
                if (in_box(lit, boxes[i], true)) {
                    pull[i] = std::max(pull[i], boxes[i].nearDepth - nearest);
                }
            }
        }
    });
    CascadeFit fits[kMaxCascades];
    for (int i = 0; i < count; ++i) {
        float pull = 0.f;
        for (const CascadeBlock& block : cascadeBlocks_) {
            pull = std::max(pull, block.pull[i]);
        }
        pull = std::min(pull, pullLimit);
        // Fitted with slack, so the box drawn last keeps holding the slice
        // while the camera moves a little, and need not be drawn again.
        Sphere padded = slices[i];
        padded.radius *= 1.f + kCascadeSlack;
        fits[i] = FitCascade(padded, sun->shine, settings.cascadeSize, pull);
        const CascadeFit& held = heldFits_[i];
        if (heldReady_ && i < cascade_.count && held.texelWorld == fits[i].texelWorld &&
            std::equal(held.lightView.m, held.lightView.m + 16, fits[i].lightView.m)) {
            const engine_core::Vec3 at = engine_core::matrix4_point(held.lightView, slices[i].center);
            const float r = slices[i].radius;
            const float depth = -at.z;
            if (at.x - r >= held.left && at.x + r <= held.right && at.y - r >= held.bottom && at.y + r <= held.top &&
                depth - r - pull >= held.nearPlane && depth + r <= held.farPlane) {
                fits[i] = held;
            }
        }
    }
    ParallelBlocks(casterCount, grain, [&](std::size_t block, std::size_t begin, std::size_t end) {
        CascadeBlock& mine = cascadeBlocks_[block];
        for (int i = 0; i < count; ++i) {
            mine.inside[i].clear();
            mine.hashes[i].clear();
            mine.moving[i].clear();
            mine.movingHashes[i].clear();
        }
        for (std::size_t k = begin; k < end; ++k) {
            const std::size_t c = CasterAt(casters, k);
            if (own(c)) {
                continue;
            }
            const bool moves = casters.moving != nullptr && casters.moving[c] != 0;
            for (int i = 0; i < count; ++i) {
                if (!in_box(lit_[k], fits[i], false)) {
                    continue;
                }
                (moves ? mine.moving[i] : mine.inside[i]).push_back(static_cast<int>(c));
                (moves ? mine.movingHashes[i] : mine.hashes[i]).push_back(casters.fingerprints[c]);
            }
        }
    });
    std::uint64_t hashes[kMaxCascades] = {};
    std::uint64_t stillHashes[kMaxCascades] = {};
    for (int i = 0; i < count; ++i) {
        Hasher still;
        still.add(fits[i].viewProjection);
        Hasher moving;
        // Kept from frame to frame, so a frame that changes nothing allocates nothing.
        std::vector<int>& inside = cascadeCasters_[i];
        std::vector<int>& movers = cascadeMoving_[i];
        inside.clear();
        movers.clear();
        for (const CascadeBlock& block : cascadeBlocks_) {
            inside.insert(inside.end(), block.inside[i].begin(), block.inside[i].end());
            movers.insert(movers.end(), block.moving[i].begin(), block.moving[i].end());
            for (const std::uint64_t fingerprint : block.hashes[i]) {
                still.add(fingerprint);
            }
            for (const std::uint64_t fingerprint : block.movingHashes[i]) {
                moving.add(fingerprint);
            }
        }
        stillHashes[i] = still.value();
        // With nothing moving, the hash is the still one alone, as it always was.
        if (movers.empty()) {
            hashes[i] = stillHashes[i];
        } else {
            Hasher both;
            both.add(stillHashes[i]);
            both.add(moving.value());
            hashes[i] = both.value();
        }
        next.viewProjection[i] = fits[i].viewProjection;
        next.texelWorld[i] = fits[i].texelWorld;
    }
    // Only the cascades whose matrix or casters changed are drawn again: a
    // car or a character moving by a still camera redraws the near ones, not
    // the far one that holds most of a forest's casters.
    const bool whole = !cascadeReady_ || cascade_.count != count;
    std::copy(cacheHashes_, cacheHashes_ + kMaxCascades, pendingCacheHashes_);
    std::copy(cacheKept_, cacheKept_ + kMaxCascades, pendingCacheKept_);
    for (int i = 0; i < count; ++i) {
        if (!whole && hashes[i] == cascadeHashes_[i]) {
            continue;
        }
        CascadeDraw draw;
        draw.layer = i;
        draw.viewProjection = next.viewProjection[i];
        draw.texelWorld = next.texelWorld[i];
        if (cascadeMoving_[i].empty()) {
            draw.casters = cascadeCasters_[i];
        } else if (!whole && cacheKept_[i] && cacheHashes_[i] == stillHashes[i]) {
            // Only what moves changed: the still casters kept are copied under it.
            draw.kind = CascadeDraw::Kind::Moving;
            draw.moving = cascadeMoving_[i];
        } else {
            draw.kind = CascadeDraw::Kind::Still;
            draw.casters = cascadeCasters_[i];
            draw.moving = cascadeMoving_[i];
            pendingCacheHashes_[i] = stillHashes[i];
            pendingCacheKept_[i] = true;
        }
        draws.push_back(std::move(draw));
    }
    if (draws.empty()) {
        return draws;
    }
    pendingCascade_ = next;
    std::copy(hashes, hashes + kMaxCascades, pendingCascadeHashes_);
    std::copy(fits, fits + count, pendingFits_);
    cascadePending_ = true;
    return draws;
}

void ShadowPlanner::commitCascades() {
    if (!cascadePending_) {
        return;
    }
    cascade_ = pendingCascade_;
    std::copy(pendingCascadeHashes_, pendingCascadeHashes_ + kMaxCascades, cascadeHashes_);
    std::copy(pendingFits_, pendingFits_ + kMaxCascades, heldFits_);
    std::copy(pendingCacheHashes_, pendingCacheHashes_ + kMaxCascades, cacheHashes_);
    std::copy(pendingCacheKept_, pendingCacheKept_ + kMaxCascades, cacheKept_);
    heldReady_ = true;
    cascadeReady_ = true;
    cascadePending_ = false;
}

}  // namespace runner
