#include "TerrainSelection.hpp"

#include "RenderMath.hpp"
#include "ShadowMath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace runner {

using engine_core::Matrix4;
using engine_core::TerrainNodeView;
using engine_core::Vec3;
using engine_core::terrain::NodeKey;
using PerTerrain = TerrainFadeState::PerTerrain;
using Fade = TerrainFadeState::Fade;

namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

std::size_t SlotOf(const NodeKey& key, std::size_t mask) {
    std::uint64_t h = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.x)) << 32 |
                       static_cast<std::uint32_t>(key.z)) *
                      0x9e3779b97f4a7c15ull;
    h ^= (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.y)) << 8 |
          static_cast<std::uint32_t>(key.level)) *
         0xc2b2ae3d27d4eb4full;
    h ^= h >> 31;
    return static_cast<std::size_t>(h) & mask;
}

// parent_of, inline: the index walks up from every node.
int Half(int value) { return value >= 0 ? value / 2 : -((1 - value) / 2); }
NodeKey Parent(const NodeKey& key) { return NodeKey{key.level + 1, Half(key.x), Half(key.y), Half(key.z)}; }

// key's index in the node set terrain is indexed for, or kNone.
std::size_t Find(const PerTerrain& terrain, const NodeKey& key) {
    if (terrain.slots.empty()) {
        return kNone;
    }
    const std::size_t mask = terrain.slots.size() - 1;
    for (std::size_t slot = SlotOf(key, mask);; slot = (slot + 1) & mask) {
        const std::uint32_t entry = terrain.slots[slot];
        if (entry == 0) {
            return kNone;
        }
        if (terrain.keys[entry - 1] == key) {
            return entry - 1;
        }
    }
}

// The table and roots for nodes, a new node set. The same keys and masks as
// the last (only meshes changed, as an edit re-meshing nodes does) keep both.
void Index(PerTerrain& terrain, const std::vector<TerrainNodeView>& nodes, int levelLimit) {
    if (terrain.keys.size() == nodes.size() && !terrain.slots.empty()) {
        bool same = true;
        for (std::size_t index = 0; index < nodes.size() && same; ++index) {
            same = nodes[index].key == terrain.keys[index] && nodes[index].child_mask == terrain.masks[index];
        }
        if (same) {
            return;
        }
    }
    terrain.keys.resize(nodes.size());
    terrain.masks.resize(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        terrain.keys[index] = nodes[index].key;
        terrain.masks[index] = nodes[index].child_mask;
    }
    std::size_t size = 16;
    while (size < nodes.size() * 2) {
        size *= 2;
    }
    terrain.slots.assign(size, 0u);
    const std::size_t mask = size - 1;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        std::size_t slot = SlotOf(terrain.keys[index], mask);
        while (terrain.slots[slot] != 0) {
            slot = (slot + 1) & mask;
        }
        terrain.slots[slot] = static_cast<std::uint32_t>(index + 1);
    }
    // A root has no published ancestor covering it: none at all, or the
    // nearest one's child_mask leaves out the child on the way down (a
    // child not built yet, new since that ancestor's build: R24), so
    // selection would never reach it from there. Nearly every node's parent
    // is published, one lookup.
    terrain.roots.clear();
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        NodeKey key = terrain.keys[index];
        bool root = true;
        while (key.level < levelLimit) {
            const NodeKey child = key;
            key = Parent(key);
            const std::size_t above = Find(terrain, key);
            if (above != kNone) {
                const int bit = (child.x - key.x * 2) + 2 * (child.y - key.y * 2) + 4 * (child.z - key.z * 2);
                root = (terrain.masks[above] & (1u << bit)) == 0;
                break;
            }
        }
        if (root) {
            terrain.roots.push_back(index);
        }
    }
}

// A node's Terrain-local box as a world box: center and half extents.
struct WorldBox {
    Vec3 center;
    Vec3 half;
};
WorldBox ToWorld(const Matrix4& m, Vec3 min, Vec3 max) {
    const Vec3 c{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
    const Vec3 h{(max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f, (max.z - min.z) * 0.5f};
    WorldBox out;
    out.center = engine_core::matrix4_point(m, c);
    // The box around the turned box: each world axis takes |M| times the half extents.
    out.half.x = std::abs(m.m[0]) * h.x + std::abs(m.m[4]) * h.y + std::abs(m.m[8]) * h.z;
    out.half.y = std::abs(m.m[1]) * h.x + std::abs(m.m[5]) * h.y + std::abs(m.m[9]) * h.z;
    out.half.z = std::abs(m.m[2]) * h.x + std::abs(m.m[6]) * h.y + std::abs(m.m[10]) * h.z;
    return out;
}

float DistanceTo(const WorldBox& box, Vec3 point) {
    const float dx = std::max(std::abs(point.x - box.center.x) - box.half.x, 0.f);
    const float dy = std::max(std::abs(point.y - box.center.y) - box.half.y, 0.f);
    const float dz = std::max(std::abs(point.z - box.center.z) - box.half.z, 0.f);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Whether box reaches inside every plane of frustum. Conservative: a box
// just past a corner may count as in view.
bool InView(const Frustum& frustum, const WorldBox& box) {
    for (int p = 0; p < 6; ++p) {
        const float* plane = frustum.planes[p];
        const float d = plane[0] * box.center.x + plane[1] * box.center.y + plane[2] * box.center.z + plane[3];
        const float r = std::abs(plane[0]) * box.half.x + std::abs(plane[1]) * box.half.y +
                        std::abs(plane[2]) * box.half.z;
        if (d + r < 0.f) {
            return false;
        }
    }
    return true;
}

// Whether any strict ancestor of key, up to level limit, is in set.
template <typename Set>
bool AncestorIn(const Set& set, NodeKey key, int limit) {
    if (set.empty()) {
        return false;
    }
    while (key.level < limit) {
        key = engine_core::terrain::parent_of(key);
        if (set.count(key) != 0) {
            return true;
        }
    }
    return false;
}

// Adds every strict ancestor of key, up to level limit, to set; stops at one already there.
void AddAncestors(NodeKeySet& set, NodeKey key, int limit) {
    while (key.level < limit) {
        key = engine_core::terrain::parent_of(key);
        if (!set.insert(key).second) {
            return;
        }
    }
}

// Whether key is in set, or an ancestor or descendant of a key in it (ancestors: set's strict ancestors).
bool Related(const NodeKeySet& set, const NodeKeySet& ancestors, const NodeKey& key, int limit) {
    return !set.empty() && (set.count(key) != 0 || ancestors.count(key) != 0 || AncestorIn(set, key, limit));
}

// What one Terrain's nodes look like from one camera.
struct Sight {
    const std::vector<TerrainNodeView>& nodes;
    const PerTerrain& terrain;
    Vec3 eye;
    bool cull = false;
    Frustum frustum;
    Matrix4 m;
    float errorScale = 1.f;
    float fov = 60.f;
    int paneHeight = 1;

    WorldBox box(const TerrainNodeView& node) const { return ToWorld(m, node.bounds_min, node.bounds_max); }
    bool inView(const WorldBox& world) const { return !cull || InView(frustum, world); }
    float pixelError(const TerrainNodeView& node, const WorldBox& world) const {
        return NodePixelError(node.error * errorScale, DistanceTo(world, eye), fov, paneHeight);
    }
};

Sight Look(const engine_core::TerrainView& view, const TerrainCamera& camera, const PerTerrain& terrain) {
    Sight sight{*view.nodes, terrain, engine_core::matrix4_position(camera.world)};
    sight.cull = camera.pane_width > 0 && camera.pane_height > 0 && camera.fov_y_degrees > 0.f &&
                 camera.fov_y_degrees < 180.f;
    if (sight.cull) {
        const Matrix4 viewMatrix = engine_core::matrix4_inverse(engine_core::matrix4_orthonormalize(camera.world));
        const Matrix4 projection =
            Perspective(camera.fov_y_degrees,
                        static_cast<float>(camera.pane_width) / static_cast<float>(camera.pane_height), kSceneNear,
                        std::max(camera.far_z, kSceneNear * 2.f));
        sight.frustum = MakeFrustum(engine_core::matrix4_multiply(projection, viewMatrix));
    }
    // A scaled Terrain's errors grow with it: by its largest axis's scale.
    sight.m = view.transform;
    const Matrix4& m = view.transform;
    sight.errorScale = std::max({std::sqrt(m.m[0] * m.m[0] + m.m[1] * m.m[1] + m.m[2] * m.m[2]),
                                 std::sqrt(m.m[4] * m.m[4] + m.m[5] * m.m[5] + m.m[6] * m.m[6]),
                                 std::sqrt(m.m[8] * m.m[8] + m.m[9] * m.m[9] + m.m[10] * m.m[10])});
    sight.fov = camera.fov_y_degrees;
    sight.paneHeight = camera.pane_height;
    return sight;
}

// node's children in its child_mask, into found (count of them); false if
// any is not published (R4: then node draws instead).
bool PublishedChildren(const PerTerrain& terrain, const TerrainNodeView& node, std::size_t (&found)[8], int& count) {
    const std::array<NodeKey, 8> children = engine_core::terrain::children_of(node.key);
    count = 0;
    for (int i = 0; i < 8; ++i) {
        if ((node.child_mask & (1u << i)) == 0) {
            continue;
        }
        const std::size_t child = Find(terrain, children[static_cast<std::size_t>(i)]);
        if (child == kNone) {
            return false;
        }
        found[count++] = child;
    }
    return true;
}

// From the roots: what to draw with no fades, culled when cull is set. A
// node in held draws without its error tested; one in holdPath (an ancestor
// of a held node) splits whatever its error, if it can. A stale node (R26)
// whose children are all published splits whatever its error or hold, and
// goes into forced (when given).
void Traverse(const Sight& sight, bool cull, const NodeKeySet& held, const NodeKeySet& holdPath,
              std::vector<std::size_t>& stack, std::vector<std::size_t>& selected, NodeKeySet* forced) {
    const std::vector<TerrainNodeView>& nodes = sight.nodes;
    const bool holding = !held.empty();
    selected.clear();
    stack.assign(sight.terrain.roots.begin(), sight.terrain.roots.end());
    std::size_t found[8];
    int count = 0;
    while (!stack.empty()) {
        const std::size_t index = stack.back();
        stack.pop_back();
        const TerrainNodeView& node = nodes[index];
        const WorldBox box = sight.box(node);
        if (cull && !sight.inView(box)) {
            continue;
        }
        if (node.key.level == 0 || node.child_mask == 0) {
            selected.push_back(index);
            continue;
        }
        // R26: a stale node shows the terrain from before an edit; its
        // published children show it since. Below the pixel budget if need be.
        if (node.stale && PublishedChildren(sight.terrain, node, found, count)) {
            if (forced != nullptr) {
                forced->insert(node.key);
            }
            stack.insert(stack.end(), found, found + count);
            continue;
        }
        if (holding && held.count(node.key) != 0) {
            selected.push_back(index);
            continue;
        }
        const bool split = holding && holdPath.count(node.key) != 0;
        if (!split && sight.pixelError(node, box) < kTerrainPixelError) {
            selected.push_back(index);
            continue;
        }
        // R4: descend only when every child with surface is published.
        if (!PublishedChildren(sight.terrain, node, found, count)) {
            selected.push_back(index);
            continue;
        }
        stack.insert(stack.end(), found, found + count);
    }
}

// What a node drawn last frame is now.
enum Drawn : std::uint8_t {
    kSteady = 1,       // fading in no longer: whole
    kFadingIn = 2,     // mid fade-in
    kFadingOut = 4,    // mid fade-out, with a value
    kMixedOut = 8,     // more than one fade-out value
};
std::uint8_t Classify(const Fade& fade, double now, float& value) {
    value = fade.at(now);
    if (fade.incoming) {
        return value >= 1.f ? kSteady : kFadingIn;
    }
    return value > 0.f ? kFadingOut : 0;
}
// Folds one drawn node (bits, value) into what is known of a key's relatives; false when it adds nothing.
bool Merge(std::pair<std::uint8_t, float>& into, std::uint8_t bits, float value) {
    const std::pair<std::uint8_t, float> before = into;
    if ((bits & kFadingOut) != 0) {
        if ((into.first & kFadingOut) == 0) {
            into.second = value;
        } else if (std::abs(into.second - value) > 1e-4f) {
            into.first = static_cast<std::uint8_t>(into.first | kMixedOut);
        }
    }
    into.first = static_cast<std::uint8_t>(into.first | bits);
    return into.first != before.first || into.second != before.second;
}

// Whether last frame drew all of key's region: key itself, or (where it drew
// something under key, per drawnBelow) every child in key's child_mask,
// each in turn. A region part culled last frame is not covered.
bool CoveredBelow(const PerTerrain& terrain, const std::vector<TerrainNodeView>& nodes,
                  const std::unordered_map<NodeKey, std::pair<std::uint8_t, float>, engine_core::terrain::NodeKeyHash>&
                      drawnBelow,
                  const NodeKey& key, double now) {
    const auto drawn = terrain.fades.find(key);
    if (drawn != terrain.fades.end()) {
        float value = 0.f;
        if (Classify(drawn->second, now, value) != 0) {
            return true;
        }
    }
    if (drawnBelow.find(key) == drawnBelow.end()) {
        return false;
    }
    const std::size_t index = Find(terrain, key);
    if (index == kNone || key.level == 0 || nodes[index].child_mask == 0) {
        return false;
    }
    const std::array<NodeKey, 8> children = engine_core::terrain::children_of(key);
    for (int i = 0; i < 8; ++i) {
        if ((nodes[index].child_mask & (1u << i)) != 0 &&
            !CoveredBelow(terrain, nodes, drawnBelow, children[static_cast<std::size_t>(i)], now)) {
            return false;
        }
    }
    return true;
}

}  // namespace

float TerrainFadeState::Fade::at(double now) const {
    const float moved = static_cast<float>((now - since) / kTerrainFadeSeconds);
    return std::clamp(incoming ? from + moved : from - moved, 0.f, 1.f);
}

void TerrainFadeState::sweep() {
    for (auto it = terrains.begin(); it != terrains.end();) {
        if (!it->second.seen) {
            it = terrains.erase(it);
        } else {
            it->second.seen = false;
            ++it;
        }
    }
}

float NodePixelError(float error, float distance, float fov_y_degrees, int pane_height) {
    if (!(distance > 0.f)) {
        return error > 0.f ? std::numeric_limits<float>::infinity() : 0.f;
    }
    const float scale = static_cast<float>(pane_height) / (2.f * std::tan(fov_y_degrees * kDegree * 0.5f));
    return error * scale / distance;
}

void SelectTerrainNodes(const engine_core::TerrainView& view, const TerrainCamera& camera, double now_seconds,
                        TerrainFadeState& state, std::vector<NodeChoice>& out) {
    out.clear();
    state.held.clear();
    state.holdPath.clear();
    state.chosen.clear();
    PerTerrain& terrain = state.terrains[view.terrain];
    terrain.seen = true;
    if (view.nodes == nullptr || view.nodes->empty()) {
        terrain.fades.clear();
        terrain.roots.clear();
        terrain.slots.clear();
        terrain.indexFor = nullptr;
        terrain.forced.clear();
        terrain.forcedAncestors.clear();
        return;
    }
    const std::vector<TerrainNodeView>& nodes = *view.nodes;
    const int levelLimit = std::max(view.top_level, nodes.back().key.level);
    if (terrain.indexFor != view.nodes.get() || terrain.indexRevision != view.nodes_revision) {
        terrain.indexFor = view.nodes.get();
        terrain.indexRevision = view.nodes_revision;
        Index(terrain, nodes, levelLimit);
    }
    int fadeLimit = levelLimit;
    for (const auto& [key, fade] : terrain.fades) {
        fadeLimit = std::max(fadeLimit, key.level);
    }

    // R18: what is fading in stays chosen until its fade is done.
    for (const auto& [key, fade] : terrain.fades) {
        if (fade.incoming && fade.at(now_seconds) < 1.f) {
            state.held.insert(key);
            AddAncestors(state.holdPath, key, fadeLimit);
        }
    }
    const Sight sight = Look(view, camera, terrain);
    std::vector<std::size_t>& selected = state.selected;
    NodeKeySet& forced = state.forced;
    forced.clear();
    Traverse(sight, sight.cull, state.held, state.holdPath, state.stack, selected, &forced);

    // Fades: what last frame drew (terrain.fades) against what this one chose.
    auto& chosen = state.chosen;
    auto& next = state.nextFades;
    auto& snapped = state.snapped;
    next.clear();
    snapped.clear();
    state.chosenAncestors.clear();
    state.snappedAncestors.clear();
    state.vanished.clear();
    state.vanishedAncestors.clear();
    for (const std::size_t index : selected) {
        chosen.insert(nodes[index].key);
    }
    bool belowReady = false;
    for (const std::size_t index : selected) {
        const NodeKey& key = nodes[index].key;
        Fade fade;
        const auto last = terrain.fades.find(key);
        // (One done fading out counts as new.)
        if (last != terrain.fades.end() && (last->second.incoming || last->second.at(now_seconds) > 0.f)) {
            fade = last->second;
            if (!fade.incoming) {
                // Chosen again mid fade-out (what replaced it went): whole at once.
                fade = Fade{1.f, now_seconds, true};
                snapped.insert(key);
            }
        } else if (AncestorIn(forced, key, fadeLimit) || Related(terrain.forced, terrain.forcedAncestors, key, fadeLimit)) {
            // An edit's swap (R24, R26): reached under a stale node this
            // frame, or where last frame descended through one (now rebuilt).
            // Whole at once, and what it replaces stops.
            fade = Fade{1.f, now_seconds, true};
            snapped.insert(key);
        } else {
            // New: what of its ancestors and descendants did last frame draw?
            if (!belowReady) {
                state.drawnBelow.clear();
                for (const auto& [drawn, drawnFade] : terrain.fades) {
                    float value = 0.f;
                    const std::uint8_t bits = Classify(drawnFade, now_seconds, value);
                    if (bits == 0) {
                        continue;
                    }
                    NodeKey up = drawn;
                    while (up.level < fadeLimit) {
                        up = engine_core::terrain::parent_of(up);
                        if (!Merge(state.drawnBelow[up], bits, value)) {
                            break;
                        }
                    }
                }
                belowReady = true;
            }
            std::pair<std::uint8_t, float> relatives{std::uint8_t{0}, 0.f};
            const auto below = state.drawnBelow.find(key);
            if (below != state.drawnBelow.end()) {
                relatives = below->second;
            }
            NodeKey up = key;
            bool aboveDrawn = false;
            while (up.level < fadeLimit) {
                up = engine_core::terrain::parent_of(up);
                const auto above = terrain.fades.find(up);
                if (above != terrain.fades.end()) {
                    float value = 0.f;
                    const std::uint8_t bits = Classify(above->second, now_seconds, value);
                    Merge(relatives, bits, value);
                    aboveDrawn = aboveDrawn || bits != 0;
                }
            }
            if ((relatives.first == kSteady || relatives.first == kFadingOut) && !aboveDrawn &&
                !CoveredBelow(terrain, nodes, state.drawnBelow, key, now_seconds)) {
                // Replacing descendants that drew only part of its region (the
                // rest was out of view): fading in, it would leave the rest
                // part-drawn. Whole at once, and those descendants stop.
                fade = Fade{1.f, now_seconds, true};
                snapped.insert(key);
            } else if (relatives.first == 0) {
                // Nothing related drawn: first sight, or turning into view.
                fade = Fade{1.f, now_seconds, true};
            } else if (relatives.first == kSteady) {
                // Replacing whole nodes: in from 0 while they go out.
                fade = Fade{0.f, now_seconds, true};
            } else if (relatives.first == kFadingOut) {
                // Coming into view (or reached) where its relatives fade out: the other half of their fade.
                fade = Fade{1.f - relatives.second, now_seconds, true};
            } else {
                // A fade it cannot join exactly: whole at once, and its relatives stop.
                fade = Fade{1.f, now_seconds, true};
                snapped.insert(key);
            }
        }
        out.push_back(NodeChoice{index, fade.at(now_seconds), true});
        next.emplace(key, fade);
    }
    for (const std::size_t index : selected) {
        AddAncestors(state.chosenAncestors, nodes[index].key, fadeLimit);
    }
    // What fades out: last frame's nodes not chosen, while what replaced them draws.
    const std::size_t outgoing = out.size();
    auto& dropped = state.vanished;
    for (const auto& [key, last] : terrain.fades) {
        if (chosen.count(key) != 0 || !Related(chosen, state.chosenAncestors, key, fadeLimit)) {
            continue;
        }
        float value = 0.f;
        const std::uint8_t bits = Classify(last, now_seconds, value);
        if (bits == 0) {
            continue;   // done fading out
        }
        const std::size_t index = bits == kFadingIn ? kNone : Find(terrain, key);
        if (index == kNone) {
            // Mid fade-in but no longer chosen (its region's fade stopped), or
            // gone from the set: what was fading against it must draw whole.
            dropped.insert(key);
            continue;
        }
        // R23: far over the pixel budget now (the camera came on faster than
        // the fade), it goes at once, and what replaces it draws whole.
        if (sight.pixelError(nodes[index], sight.box(nodes[index])) > kTerrainFadeOutPixelError) {
            dropped.insert(key);
            continue;
        }
        // Out of view, it stays in the fade (kept in next, left out of the
        // draws below) so that it draws its part again should it come back
        // into view before the fade is done.
        const Fade fade = bits == kSteady ? Fade{1.f, now_seconds, false} : last;
        out.push_back(NodeChoice{index, fade.at(now_seconds), false});
        next.emplace(key, fade);
    }
    // A node drawn whole at once (snapped) stops the fades against it; a node
    // fading in against one that stopped draws whole too; until nothing changes.
    std::size_t snappedBefore = 0;
    for (;;) {
        if (snapped.size() != snappedBefore) {
            snappedBefore = snapped.size();
            state.snappedAncestors.clear();
            for (const NodeKey& key : snapped) {
                AddAncestors(state.snappedAncestors, key, fadeLimit);
            }
            const auto stop = std::remove_if(out.begin() + static_cast<std::ptrdiff_t>(outgoing), out.end(),
                                             [&](const NodeChoice& choice) {
                                                 const NodeKey& key = nodes[choice.index].key;
                                                 if (!Related(snapped, state.snappedAncestors, key, fadeLimit)) {
                                                     return false;
                                                 }
                                                 next.erase(key);
                                                 dropped.insert(key);
                                                 return true;
                                             });
            out.erase(stop, out.end());
        }
        if (dropped.empty()) {
            break;
        }
        state.vanishedAncestors.clear();
        for (const NodeKey& key : dropped) {
            AddAncestors(state.vanishedAncestors, key, fadeLimit);
        }
        for (std::size_t i = 0; i < outgoing; ++i) {
            const NodeKey& key = nodes[out[i].index].key;
            if (out[i].fade < 1.f && Related(dropped, state.vanishedAncestors, key, fadeLimit)) {
                out[i].fade = 1.f;
                next[key] = Fade{1.f, now_seconds, true};
                snapped.insert(key);
            }
        }
        if (snapped.size() == snappedBefore) {
            break;
        }
    }
    // What fades out out of view draws nothing (it stays in next).
    out.erase(std::remove_if(out.begin() + static_cast<std::ptrdiff_t>(outgoing), out.end(),
                             [&](const NodeChoice& choice) { return !sight.inView(sight.box(nodes[choice.index])); }),
              out.end());
    terrain.fades.swap(next);
    terrain.forced.swap(forced);
    terrain.forcedAncestors.clear();
    for (const NodeKey& key : terrain.forced) {
        AddAncestors(terrain.forcedAncestors, key, fadeLimit);
    }
}

void SelectTerrainCasters(const engine_core::TerrainView& view, const TerrainCamera& camera,
                          TerrainFadeState& state, std::vector<std::size_t>& out) {
    out.clear();
    const auto found = state.terrains.find(view.terrain);
    if (view.nodes == nullptr || view.nodes->empty() || found == state.terrains.end() ||
        found->second.indexFor != view.nodes.get()) {
        return;
    }
    const Sight sight = Look(view, camera, found->second);
    if (!sight.cull) {
        // Nothing is out of view: every caster is drawn.
        return;
    }
    // The same selection as the frame's, holds and all, but not culled: in
    // view it is what was chosen; the rest is out of view.
    Traverse(sight, false, state.held, state.holdPath, state.stack, out, nullptr);
    const std::vector<TerrainNodeView>& nodes = *view.nodes;
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](std::size_t index) { return state.chosen.count(nodes[index].key) != 0; }),
              out.end());
    // Nearest first, so the few a frame AppendTerrainDraws uploads are the nearest.
    auto& nearest = state.nearest;
    nearest.clear();
    for (const std::size_t index : out) {
        nearest.emplace_back(DistanceTo(sight.box(nodes[index]), sight.eye), index);
    }
    std::sort(nearest.begin(), nearest.end());
    for (std::size_t i = 0; i < nearest.size(); ++i) {
        out[i] = nearest[i].second;
    }
}

void SelectTerrainPrefetch(const engine_core::TerrainView& view, const TerrainCamera& camera,
                           const std::vector<NodeChoice>& choices, TerrainFadeState& state,
                           const std::function<bool(std::size_t index)>& uploaded, std::vector<std::size_t>& keep,
                           std::vector<std::size_t>& upload) {
    keep.clear();
    upload.clear();
    const auto found = state.terrains.find(view.terrain);
    if (view.nodes == nullptr || view.nodes->empty() || found == state.terrains.end() ||
        found->second.indexFor != view.nodes.get()) {
        return;
    }
    const PerTerrain& terrain = found->second;
    const std::vector<TerrainNodeView>& nodes = *view.nodes;
    const Sight sight = Look(view, camera, terrain);
    auto& waiting = state.nearest;
    waiting.clear();
    for (const NodeChoice& choice : choices) {
        const TerrainNodeView& node = nodes[choice.index];
        if (!choice.incoming || node.key.level == 0 || node.child_mask == 0 ||
            sight.pixelError(node, sight.box(node)) < kTerrainPrefetchPixelError) {
            continue;
        }
        const std::array<NodeKey, 8> children = engine_core::terrain::children_of(node.key);
        for (int i = 0; i < 8; ++i) {
            if ((node.child_mask & (1u << i)) == 0) {
                continue;
            }
            const std::size_t child = Find(terrain, children[static_cast<std::size_t>(i)]);
            if (child == kNone || state.chosen.count(nodes[child].key) != 0) {
                continue;
            }
            if (uploaded(child)) {
                keep.push_back(child);
            } else {
                waiting.emplace_back(DistanceTo(sight.box(nodes[child]), sight.eye), child);
            }
        }
    }
    const std::size_t count = std::min(waiting.size(), static_cast<std::size_t>(kTerrainPrefetchUploads));
    std::partial_sort(waiting.begin(), waiting.begin() + static_cast<std::ptrdiff_t>(count), waiting.end());
    for (std::size_t i = 0; i < count; ++i) {
        upload.push_back(waiting[i].second);
    }
}

}  // namespace runner
