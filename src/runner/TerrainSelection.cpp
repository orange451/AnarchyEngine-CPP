#include "TerrainSelection.hpp"

#include "RenderMath.hpp"
#include "ShadowMath.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace runner {

using engine_core::Matrix4;
using engine_core::TerrainNodeView;
using engine_core::Vec3;
using engine_core::terrain::NodeKey;

namespace {

bool KeyLess(const NodeKey& a, const NodeKey& b) {
    return std::tie(a.level, a.x, a.y, a.z) < std::tie(b.level, b.x, b.y, b.z);
}

// key's index in nodes (sorted by level, x, y, z), or npos.
constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
std::size_t Find(const std::vector<TerrainNodeView>& nodes, const NodeKey& key) {
    const auto it = std::lower_bound(nodes.begin(), nodes.end(), key,
                                     [](const TerrainNodeView& node, const NodeKey& k) { return KeyLess(node.key, k); });
    return it != nodes.end() && it->key == key ? static_cast<std::size_t>(it - nodes.begin()) : kNone;
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
    while (key.level < limit) {
        key = engine_core::terrain::parent_of(key);
        if (set.count(key) != 0) {
            return true;
        }
    }
    return false;
}

// Adds every strict ancestor of key, up to level limit, to set; stops at one already there.
template <typename Set>
void AddAncestors(Set& set, NodeKey key, int limit) {
    while (key.level < limit) {
        key = engine_core::terrain::parent_of(key);
        if (!set.insert(key).second) {
            return;
        }
    }
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
    TerrainFadeState::PerTerrain& terrain = state.terrains[view.terrain];
    terrain.seen = true;
    if (view.nodes == nullptr || view.nodes->empty()) {
        terrain.fades.clear();
        terrain.roots.clear();
        terrain.rootsFor = nullptr;
        return;
    }
    const std::vector<TerrainNodeView>& nodes = *view.nodes;
    const int levelLimit = std::max(view.top_level, nodes.back().key.level);

    // The roots: published nodes with no published ancestor. Once a node set.
    if (terrain.rootsFor != view.nodes.get() || terrain.rootsRevision != view.nodes_revision) {
        terrain.rootsFor = view.nodes.get();
        terrain.rootsRevision = view.nodes_revision;
        terrain.roots.clear();
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            NodeKey key = nodes[index].key;
            bool root = true;
            while (key.level < levelLimit) {
                key = engine_core::terrain::parent_of(key);
                if (Find(nodes, key) != kNone) {
                    root = false;
                    break;
                }
            }
            if (root) {
                terrain.roots.push_back(index);
            }
        }
    }

    // The camera, in world space.
    const Vec3 eye = engine_core::matrix4_position(camera.world);
    const bool cull = camera.pane_width > 0 && camera.pane_height > 0 && camera.fov_y_degrees > 0.f &&
                      camera.fov_y_degrees < 180.f;
    Frustum frustum;
    if (cull) {
        const Matrix4 viewMatrix = engine_core::matrix4_inverse(engine_core::matrix4_orthonormalize(camera.world));
        const Matrix4 projection =
            Perspective(camera.fov_y_degrees,
                        static_cast<float>(camera.pane_width) / static_cast<float>(camera.pane_height), kSceneNear,
                        std::max(camera.far_z, kSceneNear * 2.f));
        frustum = MakeFrustum(engine_core::matrix4_multiply(projection, viewMatrix));
    }
    // A scaled Terrain's errors grow with it: by its largest axis's scale.
    const Matrix4& m = view.transform;
    const float errorScale = std::max({std::sqrt(m.m[0] * m.m[0] + m.m[1] * m.m[1] + m.m[2] * m.m[2]),
                                       std::sqrt(m.m[4] * m.m[4] + m.m[5] * m.m[5] + m.m[6] * m.m[6]),
                                       std::sqrt(m.m[8] * m.m[8] + m.m[9] * m.m[9] + m.m[10] * m.m[10])});
    const auto inView = [&](const TerrainNodeView& node) {
        return !cull || InView(frustum, ToWorld(m, node.bounds_min, node.bounds_max));
    };

    // What this frame would draw with no fades.
    std::vector<std::size_t>& selected = state.selected;
    std::vector<std::size_t>& stack = state.stack;
    selected.clear();
    stack.assign(terrain.roots.begin(), terrain.roots.end());
    while (!stack.empty()) {
        const std::size_t index = stack.back();
        stack.pop_back();
        const TerrainNodeView& node = nodes[index];
        const WorldBox box = ToWorld(m, node.bounds_min, node.bounds_max);
        if (cull && !InView(frustum, box)) {
            continue;
        }
        if (node.key.level == 0 || node.child_mask == 0 ||
            NodePixelError(node.error * errorScale, DistanceTo(box, eye), camera.fov_y_degrees,
                           camera.pane_height) < kTerrainPixelError) {
            selected.push_back(index);
            continue;
        }
        // R4: descend only when every child with surface is published.
        const std::array<NodeKey, 8> children = engine_core::terrain::children_of(node.key);
        std::size_t found[8];
        int count = 0;
        bool complete = true;
        for (int i = 0; i < 8; ++i) {
            if ((node.child_mask & (1u << i)) == 0) {
                continue;
            }
            const std::size_t child = Find(nodes, children[static_cast<std::size_t>(i)]);
            if (child == kNone) {
                complete = false;
                break;
            }
            found[count++] = child;
        }
        if (!complete) {
            selected.push_back(index);
            continue;
        }
        stack.insert(stack.end(), found, found + count);
    }

    // Fades: what last frame drew (terrain.fades) against what this one chose.
    auto& chosen = state.chosen;
    auto& chosenAncestors = state.chosenAncestors;
    auto& drawnAncestors = state.drawnAncestors;
    auto& next = state.nextFades;
    chosen.clear();
    chosenAncestors.clear();
    drawnAncestors.clear();
    next.clear();
    bool drawnAncestorsReady = false;
    int fadeLimit = levelLimit;
    for (const auto& [key, fade] : terrain.fades) {
        fadeLimit = std::max(fadeLimit, key.level);
    }
    for (const std::size_t index : selected) {
        chosen.insert(nodes[index].key);
    }
    for (const std::size_t index : selected) {
        const NodeKey& key = nodes[index].key;
        TerrainFadeState::Fade fade;
        const auto last = terrain.fades.find(key);
        if (last != terrain.fades.end()) {
            fade = last->second;
            if (!fade.incoming) {
                // Chosen again mid fade-out: back in from where it is.
                fade = TerrainFadeState::Fade{fade.at(now_seconds), now_seconds, true};
            }
        } else {
            // New: it replaces what last frame drew of its ancestors or descendants, if any.
            if (!drawnAncestorsReady) {
                for (const auto& [drawn, unused] : terrain.fades) {
                    AddAncestors(drawnAncestors, drawn, fadeLimit);
                }
                drawnAncestorsReady = true;
            }
            const bool replaces = drawnAncestors.count(key) != 0 || AncestorIn(terrain.fades, key, fadeLimit);
            fade = TerrainFadeState::Fade{replaces ? 0.f : 1.f, now_seconds, true};
        }
        out.push_back(NodeChoice{index, fade.at(now_seconds), true});
        next.emplace(key, fade);
    }
    bool chosenAncestorsReady = false;
    for (const auto& [key, last] : terrain.fades) {
        if (chosen.count(key) != 0) {
            continue;
        }
        // Not chosen: it fades out only while what replaced it draws.
        if (!chosenAncestorsReady) {
            for (const std::size_t index : selected) {
                AddAncestors(chosenAncestors, nodes[index].key, fadeLimit);
            }
            chosenAncestorsReady = true;
        }
        if (chosenAncestors.count(key) == 0 && !AncestorIn(chosen, key, fadeLimit)) {
            continue;
        }
        TerrainFadeState::Fade fade = last;
        if (fade.incoming) {
            fade = TerrainFadeState::Fade{fade.at(now_seconds), now_seconds, false};
        }
        const float value = fade.at(now_seconds);
        if (!(value > 0.f)) {
            continue;
        }
        const std::size_t index = Find(nodes, key);
        if (index == kNone || !inView(nodes[index])) {
            continue;
        }
        out.push_back(NodeChoice{index, value, false});
        next.emplace(key, fade);
    }
    terrain.fades.swap(next);
}

}  // namespace runner
