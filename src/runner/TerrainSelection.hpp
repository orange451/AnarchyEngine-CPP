#pragma once

// Which of a Terrain's published LOD nodes (TerrainView::nodes) to draw this
// frame, with no GL: each node's error projected to the screen decides
// whether it draws or its children are tested instead, nodes outside the
// camera's view are skipped with their children, and a node replaced by its
// children (or the reverse) keeps drawing for kTerrainFadeSeconds while the
// other fades in, the two dithered so together they cover each pixel once.
// TerrainDraws turns the choices into MeshDraws; sandbox tests drive this alone.

#include "Matrix4.hpp"
#include "RenderMath.hpp"
#include "TerrainWorld.hpp"
#include "terrain/LodNode.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace runner {

// A node draws when its error, projected, is under this many pixels.
constexpr float kTerrainPixelError = 1.f;
// How long a switch between levels cross-fades.
constexpr double kTerrainFadeSeconds = 0.25;

// Where the Terrain is seen from: a Camera's Transform (looking down its -Z),
// its vertical angle, and the pane it draws into, in pixels. pane_width sets
// the view's aspect for culling; 0 (or less) culls nothing. far_z is the
// far plane: the Scene View draws nothing beyond kSceneFar.
struct TerrainCamera {
    engine_core::Matrix4 world = engine_core::matrix4_identity();
    float fov_y_degrees = 60.f;
    int pane_height = 1;
    int pane_width = 0;
    float far_z = kSceneFar;
};

// One node to draw: an index into the TerrainView's nodes, its fade (1 is
// fully drawn), and whether it is fading in (or steady) rather than out.
struct NodeChoice {
    std::size_t index = 0;
    float fade = 1.f;
    bool incoming = true;
};

// What selection keeps between frames, per Terrain: each node drawn last
// frame and its fade, and the Terrain's roots for its current node set.
struct TerrainFadeState {
    struct Fade {
        float from = 1.f;     // the fade at since
        double since = 0.0;   // seconds, SelectTerrainNodes's clock
        bool incoming = true;
        float at(double now) const;
    };
    struct PerTerrain {
        std::unordered_map<engine_core::terrain::NodeKey, Fade, engine_core::terrain::NodeKeyHash> fades;
        // Roots of the node set whose nodes_revision is rootsRevision.
        std::uint64_t rootsRevision = 0;
        const void* rootsFor = nullptr;
        std::vector<std::size_t> roots;
        bool seen = false;
    };
    std::unordered_map<engine_core::InstanceId, PerTerrain> terrains;

    // Forgets the Terrains no SelectTerrainNodes call saw since the last sweep.
    void sweep();

    // Per-frame scratch, kept so a frame allocates only while it grows.
    std::vector<std::size_t> stack;
    std::vector<std::size_t> selected;
    std::unordered_set<engine_core::terrain::NodeKey, engine_core::terrain::NodeKeyHash> chosen, chosenAncestors,
        drawnAncestors;
    std::unordered_map<engine_core::terrain::NodeKey, Fade, engine_core::terrain::NodeKeyHash> nextFades;
    // AppendTerrainDraws's, for one Terrain's choices at a time.
    std::vector<NodeChoice> choices;
};

// A node's error (studs) as pixels on a pane pane_height pixels tall, seen
// with a vertical angle of fov_y_degrees from distance studs away:
// error * pane_height / (2 tan(fov / 2)) / distance. Infinite at distance 0
// (inside the node) unless error is 0.
float NodePixelError(float error, float distance, float fov_y_degrees, int pane_height);

// Fills out with view's nodes to draw this frame from camera, at now_seconds
// (any origin, never decreasing). Pure but for state, which carries fades
// between frames, keyed by Terrain and node.
//
// The roots are the published nodes with no published ancestor (the nodes at
// top_level once those are built). From each, a node outside camera's view
// is skipped with its children; a node whose pixel error is under
// kTerrainPixelError, or at level 0, or with any child in its child_mask
// missing from the set, is drawn; otherwise each child is tested in turn.
// A node newly chosen where last frame drew an ancestor or descendant of it
// fades in from 0 over kTerrainFadeSeconds, while that one fades out; a node
// newly chosen with nothing related drawn before (first sight, or turning
// into view) draws at once.
void SelectTerrainNodes(const engine_core::TerrainView& view, const TerrainCamera& camera, double now_seconds,
                        TerrainFadeState& state, std::vector<NodeChoice>& out);

}  // namespace runner
