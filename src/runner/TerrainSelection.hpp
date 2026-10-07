#pragma once

// Which of a Terrain's published LOD nodes (TerrainView::nodes) to draw this
// frame, with no GL: each node's error projected to the screen decides
// whether it draws or its children are tested instead, nodes outside the
// camera's view are skipped with their children, and a node replaced by its
// children (or the reverse) keeps drawing for kTerrainFadeSeconds while the
// other fades in, the two dithered so together they cover each pixel once.
// Also which nodes cast shadows without being drawn (out of view), and which
// finer nodes to upload ahead of a switch. TerrainDraws turns the choices into
// MeshDraws; sandbox tests drive this alone.

#include "Matrix4.hpp"
#include "RenderMath.hpp"
#include "TerrainWorld.hpp"
#include "terrain/LodNode.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace runner {

// A node draws when its error, projected, is under this many pixels.
constexpr float kTerrainPixelError = 1.f;
// How long a switch between levels cross-fades.
constexpr double kTerrainFadeSeconds = 0.25;
// A drawn node this close to switching (its pixel error at least this) has
// its children uploaded ahead, nearest first, at most kTerrainPrefetchUploads a frame.
constexpr float kTerrainPrefetchPixelError = 0.5f;
constexpr int kTerrainPrefetchUploads = 8;
// Out-of-view shadow casters not uploaded yet upload at most this many a frame.
constexpr int kTerrainCasterUploads = 16;

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

using NodeKeySet = std::unordered_set<engine_core::terrain::NodeKey, engine_core::terrain::NodeKeyHash>;

// What selection keeps between frames, per Terrain: each node drawn last
// frame and its fade, and an index of the Terrain's current node set.
struct TerrainFadeState {
    struct Fade {
        float from = 1.f;     // the fade at since
        double since = 0.0;   // seconds, SelectTerrainNodes's clock
        bool incoming = true;
        float at(double now) const;
    };
    struct PerTerrain {
        std::unordered_map<engine_core::terrain::NodeKey, Fade, engine_core::terrain::NodeKeyHash> fades;
        // Of the node set whose nodes_revision is indexRevision: an open-addressed
        // table of node index + 1 by key (0 empty; its size a power of two), and
        // the roots, the nodes with no published ancestor.
        std::uint64_t indexRevision = 0;
        const void* indexFor = nullptr;
        std::vector<std::uint32_t> slots;
        std::vector<engine_core::terrain::NodeKey> keys;   // the set's, in its order
        std::vector<std::size_t> roots;
        bool seen = false;
    };
    std::unordered_map<engine_core::InstanceId, PerTerrain> terrains;

    // Forgets the Terrains no SelectTerrainNodes call saw since the last sweep.
    void sweep();

    // Per-frame scratch, kept so a frame allocates only while it grows. The
    // held sets and chosen stay from a SelectTerrainNodes call for
    // SelectTerrainCasters and SelectTerrainPrefetch after it.
    std::vector<std::size_t> stack;
    std::vector<std::size_t> selected;
    NodeKeySet chosen, chosenAncestors, held, holdPath, snapped, snappedAncestors, vanished, vanishedAncestors;
    // Per key, what last frame drew under it: Relatives bits, and the fade of what fades out there.
    std::unordered_map<engine_core::terrain::NodeKey, std::pair<std::uint8_t, float>,
                       engine_core::terrain::NodeKeyHash>
        drawnBelow;
    std::unordered_map<engine_core::terrain::NodeKey, Fade, engine_core::terrain::NodeKeyHash> nextFades;
    std::vector<std::pair<float, std::size_t>> nearest;
    // AppendTerrainDraws's, for one Terrain's choices at a time.
    std::vector<NodeChoice> choices;
    std::vector<std::size_t> casters, keep, upload;
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
//
// A node newly chosen where last frame drew an ancestor or descendant of it
// fades in from 0 over kTerrainFadeSeconds, while those fade out; one newly
// chosen with nothing related drawn before (first sight, or turning into
// view) draws at once, and one coming into view where an ancestor or
// descendant is already fading out takes up the other half of that fade. A
// region mid-fade does not switch again until its fade is done: the nodes
// fading in there stay chosen (their ancestors are tested no further) for
// up to kTerrainFadeSeconds. Should that fail (a node of the fade gone from
// the set, or a parent needed to split it missing a child), what is chosen
// there draws whole at once and the rest of that fade stops: never a hole.
void SelectTerrainNodes(const engine_core::TerrainView& view, const TerrainCamera& camera, double now_seconds,
                        TerrainFadeState& state, std::vector<NodeChoice>& out);

// After SelectTerrainNodes for the same view, camera, and state: the nodes
// that cast shadows without being drawn. Shadows come from the same
// selection with no view culling: in view, the nodes drawn fading in (or
// steady), which cast already; out of view, the nodes that selection would
// draw there, which out lists.
void SelectTerrainCasters(const engine_core::TerrainView& view, const TerrainCamera& camera,
                          TerrainFadeState& state, std::vector<std::size_t>& out);

// After SelectTerrainNodes, with its choices: the published children of the
// nodes chosen fading in (or steady) whose pixel error is at least
// kTerrainPrefetchPixelError, nearest first. Those uploaded(index) says are
// held already go to keep (to stay held); the first kTerrainPrefetchUploads
// that are not go to upload, and the rest wait for later frames.
void SelectTerrainPrefetch(const engine_core::TerrainView& view, const TerrainCamera& camera,
                           const std::vector<NodeChoice>& choices, TerrainFadeState& state,
                           const std::function<bool(std::size_t index)>& uploaded, std::vector<std::size_t>& keep,
                           std::vector<std::size_t>& upload);

}  // namespace runner
