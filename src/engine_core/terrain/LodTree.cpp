#include "terrain/LodTree.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

namespace engine_core::terrain {

namespace {

NodeKey chunk_key(ChunkCoord coord) { return NodeKey{0, coord.x, coord.y, coord.z}; }

int axis_distance(int c, int node, int span) {
    const int lo = node * span;
    const int hi = lo + span - 1;
    return std::max({0, lo - c, c - hi});
}

// L-infinity distance, in chunks, from chunk to key's chunk box (0 inside).
int linf_chunks(ChunkCoord chunk, const NodeKey& key) {
    const int span = 1 << key.level;
    return std::max({axis_distance(chunk.x, key.x, span), axis_distance(chunk.y, key.y, span),
                     axis_distance(chunk.z, key.z, span)});
}

// node_bounds(key) grown to take in mesh's own AABB (R2).
void covering_bounds(const NodeKey& key, float voxel_size, const anarchy::amesh::Data& mesh, Vec3& min, Vec3& max) {
    node_bounds(key, voxel_size, min, max);
    if (mesh.vertices.empty()) {
        return;
    }
    min.x = std::min(min.x, mesh.bbox_min[0]);
    min.y = std::min(min.y, mesh.bbox_min[1]);
    min.z = std::min(min.z, mesh.bbox_min[2]);
    max.x = std::max(max.x, mesh.bbox_max[0]);
    max.y = std::max(max.y, mesh.bbox_max[1]);
    max.z = std::max(max.z, mesh.bbox_max[2]);
}

// One axis has collapsed at this level: a single node, or the two either
// side of the origin, which floor division never joins.
bool axis_settled(int lo, int hi) { return lo == hi || (lo == -1 && hi == 0); }

}  // namespace

LodTree::LodTree(float voxel_size, std::uint64_t* revisions)
    : voxel_size_(voxel_size), revisions_(revisions != nullptr ? revisions : &own_revisions_) {}

const LodTree::Node* LodTree::find(const NodeKey& key) const {
    const auto found = nodes_.find(key);
    return found != nodes_.end() ? &found->second : nullptr;
}

LodTree::Node* LodTree::find_mutable(const NodeKey& key) {
    const auto found = nodes_.find(key);
    return found != nodes_.end() ? &found->second : nullptr;
}

void LodTree::extend_extent(ChunkCoord coord) {
    if (level0_count_ == 0) {
        lo_ = hi_ = coord;
        return;
    }
    lo_ = ChunkCoord{std::min(lo_.x, coord.x), std::min(lo_.y, coord.y), std::min(lo_.z, coord.z)};
    hi_ = ChunkCoord{std::max(hi_.x, coord.x), std::max(hi_.y, coord.y), std::max(hi_.z, coord.z)};
}

void LodTree::refresh_top() {
    if (extent_dirty_) {
        extent_dirty_ = false;
        bool first = true;
        for (const auto& [key, node] : nodes_) {
            (void)node;
            if (key.level != 0) {
                continue;
            }
            const ChunkCoord coord{key.x, key.y, key.z};
            if (first) {
                lo_ = hi_ = coord;
                first = false;
            } else {
                lo_ = ChunkCoord{std::min(lo_.x, coord.x), std::min(lo_.y, coord.y), std::min(lo_.z, coord.z)};
                hi_ = ChunkCoord{std::max(hi_.x, coord.x), std::max(hi_.y, coord.y), std::max(hi_.z, coord.z)};
            }
        }
    }
    int top = 0;
    if (level0_count_ > 0) {
        for (;; ++top) {
            const NodeKey a = node_of(lo_, top);
            const NodeKey b = node_of(hi_, top);
            if (top >= 30 || (axis_settled(a.x, b.x) && axis_settled(a.y, b.y) && axis_settled(a.z, b.z))) {
                break;
            }
        }
    }
    if (top == top_) {
        return;
    }
    if (top < top_) {
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            if (it->first.level > top) {
                it = nodes_.erase(it);
            } else {
                ++it;
            }
        }
        top_ = top;
        changed_ = true;
        return;
    }
    top_ = top;
    std::vector<ChunkCoord> chunks;
    chunks.reserve(level0_count_);
    for (const auto& [key, node] : nodes_) {
        (void)node;
        if (key.level == 0) {
            chunks.push_back(ChunkCoord{key.x, key.y, key.z});
        }
    }
    for (const ChunkCoord& coord : chunks) {
        ensure_ancestors(coord, false);
    }
}

int LodTree::top_level() {
    refresh_top();
    return top_;
}

void LodTree::mark_stale(Node& node) {
    node.revision = next_revision();
    changed_ = true;   // a published node's stale flag (R26) shows at once
}

void LodTree::ensure_ancestors(ChunkCoord coord, bool mark) {
    NodeKey key = chunk_key(coord);
    for (int level = 1; level <= top_; ++level) {
        key = parent_of(key);
        const auto [it, created] = nodes_.try_emplace(key);
        Node& node = it->second;
        if (created) {
            node.revision = node.min_revision = next_revision();   // stale until built
            node_bounds(key, voxel_size_, node.bounds_min, node.bounds_max);
        } else if (mark) {
            mark_stale(node);
        }
    }
}

bool LodTree::has_children(const NodeKey& key) const {
    for (const NodeKey& child : children_of(key)) {
        if (nodes_.count(child) != 0) {
            return true;
        }
    }
    return false;
}

bool LodTree::surfaced(const NodeKey& key, const Node& node) const {
    // A level >= 1 node not built yet (one an edit's queued chunks just
    // made, most of them air that comes back empty) counts once it is.
    return key.level == 0 ? node.has_surface : (node.built && node.has_surface);
}

bool LodTree::child_ready(const NodeKey& key, const Node& node) const {
    if (key.level == 0) {
        // A failed chunk with no resident mesh stops blocking: the parent
        // builds without it.
        return !node.in_flight && (!node.has_surface || node.chunk_mesh != nullptr || node.failed);
    }
    return !node.stale() && (!node.has_surface || node.resident);
}

bool LodTree::parent_covers(const NodeKey& key) const {
    if (key.level >= top_) {
        return false;
    }
    const Node* parent = find(parent_of(key));
    return parent != nullptr && parent->built && !parent->stale();
}

bool LodTree::parent_wants(const NodeKey& key) const {
    if (key.level >= top_) {
        return true;
    }
    const Node* parent = find(parent_of(key));
    return parent == nullptr || parent->stale();
}

void LodTree::chunk_queued(ChunkCoord coord, bool edited) {
    residency_dirty_ = true;
    const auto [it, created] = nodes_.try_emplace(chunk_key(coord));
    if (created) {
        extend_extent(coord);
        ++level0_count_;
    }
    it->second.in_flight = true;   // the reference survives the rehashes below
    it->second.failed = false;
    refresh_top();
    ensure_ancestors(coord, edited);
}

void LodTree::chunk_meshed(ChunkCoord coord, std::shared_ptr<const anarchy::amesh::Data> mesh, bool edited) {
    residency_dirty_ = true;
    if (mesh == nullptr || mesh->indices.empty()) {
        chunk_removed(coord);
        return;
    }
    const NodeKey key = chunk_key(coord);
    const auto [it, created] = nodes_.try_emplace(key);
    if (created) {
        extend_extent(coord);
        ++level0_count_;
    }
    Node& node = it->second;
    const bool was_surface = node.has_surface;
    node.in_flight = false;
    node.failed = false;
    node.has_surface = true;
    covering_bounds(key, voxel_size_, *mesh, node.bounds_min, node.bounds_max);
    node.chunk_mesh = std::move(mesh);
    node.mesh_revision = next_revision();
    changed_ = true;
    refresh_top();
    ensure_ancestors(coord, edited || !was_surface);
}

void LodTree::chunk_removed(ChunkCoord coord) {
    residency_dirty_ = true;
    const NodeKey key = chunk_key(coord);
    const auto found = nodes_.find(key);
    if (found == nodes_.end()) {
        return;
    }
    const bool had_surface = found->second.has_surface;
    if (found->second.chunk_mesh != nullptr) {
        changed_ = true;
    }
    --level0_count_;
    extent_dirty_ = true;
    prune_up(key, had_surface);
    refresh_top();
}

void LodTree::prune_up(const NodeKey& key, bool had_surface) {
    nodes_.erase(key);
    // Ancestors left with no children go; the rest change if key had surface.
    NodeKey up = key;
    bool pruning = true;
    for (int level = key.level + 1; level <= top_; ++level) {
        up = parent_of(up);
        const auto parent = nodes_.find(up);
        if (parent == nodes_.end()) {
            break;
        }
        if (pruning && !has_children(up)) {
            if (parent->second.resident && parent->second.has_surface) {
                changed_ = true;
            }
            nodes_.erase(parent);
            continue;
        }
        pruning = false;
        if (!had_surface) {
            break;
        }
        mark_stale(parent->second);
    }
}

void LodTree::chunk_failed(ChunkCoord coord) {
    residency_dirty_ = true;
    Node* node = find_mutable(chunk_key(coord));
    if (node == nullptr) {
        return;
    }
    if (!node->has_surface) {
        chunk_removed(coord);   // no old mesh to keep: as if it came back empty
        return;
    }
    node->in_flight = false;
    node->failed = true;   // keeps chunk_mesh (null if it was dropped)
}

void LodTree::node_failed(const NodeResult& result) {
    if (result.key.level == 0) {
        return;
    }
    Node* node = find_mutable(result.key);
    if (node == nullptr || result.revision < node->min_revision || node->queued_revision != result.revision) {
        return;   // gone, from a dropped tree, or a newer build was queued since
    }
    // last_build_ms stays: next_builds offers it again after the debounce window.
    node->queued_revision = 0;
}

void LodTree::node_built(const NodeResult& result) {
    if (result.key.level == 0) {
        return;
    }
    Node* node = find_mutable(result.key);
    if (node == nullptr || result.revision < node->min_revision || result.revision <= node->built_revision) {
        return;   // the node is gone, or this is older than what it has (or from a dropped tree)
    }
    residency_dirty_ = true;
    changed_ = true;
    const auto& mesh = result.result.mesh;
    if (mesh == nullptr || mesh->indices.empty()) {
        // No surface: only nodes with surface exist, so it goes, with any
        // ancestor it leaves childless; the rest rebuild without it.
        prune_up(result.key, node->built && node->has_surface);
        return;
    }
    node->built_revision = result.revision;
    node->built = true;
    covering_bounds(result.key, voxel_size_, *mesh, node->bounds_min, node->bounds_max);
    node->compact = std::make_shared<const CompactMesh>(
        pack(*mesh, node->bounds_min, node->bounds_max, result.result.surface_index_count));
    node->has_surface = true;
    node->resident = true;
    node->error = result.result.error;
    node->mesh_revision = next_revision();
}

void LodTree::update_residency(const ChunkCoord* camera_chunk, std::vector<ChunkCoord>& out_drop_chunks,
                               std::vector<ChunkCoord>& out_need_chunks) {
    // Nothing to decide again until the camera changes chunk or the tree does.
    const bool same_camera = camera_chunk != nullptr ? (residency_had_camera_ && *camera_chunk == residency_camera_)
                                                     : !residency_had_camera_;
    if (!residency_dirty_ && same_camera) {
        return;
    }
    refresh_top();
    // Level 1 first: one it marks stale here wants its chunks below.
    for (auto& [key, node] : nodes_) {
        if (key.level != 1 || !node.built || !node.has_surface) {
            continue;   // unbuilt nodes are stale already; empty ones hold nothing
        }
        const int distance = camera_chunk != nullptr ? linf_chunks(*camera_chunk, key) : 0;
        const bool wanted = camera_chunk == nullptr || distance <= kNearChunks || parent_wants(key);
        if (node.resident) {
            if (camera_chunk != nullptr && distance > kFarChunks && !wanted && parent_covers(key)) {
                node.compact.reset();
                node.resident = false;
                changed_ = true;
            }
        } else if (wanted && !node.stale()) {
            mark_stale(node);   // rebuilt from its chunks, which are wanted now
        }
    }
    for (auto& [key, node] : nodes_) {
        if (key.level != 0 || !node.has_surface) {
            continue;
        }
        const ChunkCoord coord{key.x, key.y, key.z};
        const int distance = camera_chunk != nullptr ? linf_chunks(*camera_chunk, key) : 0;
        const bool wanted = camera_chunk == nullptr || distance <= kNearChunks || parent_wants(key);
        if (node.chunk_mesh != nullptr) {
            if (camera_chunk != nullptr && distance > kFarChunks && !wanted && !node.in_flight &&
                parent_covers(key)) {
                node.chunk_mesh.reset();
                changed_ = true;
                out_drop_chunks.push_back(coord);
            }
        } else if (wanted && !node.in_flight && !node.failed) {
            node.in_flight = true;
            out_need_chunks.push_back(coord);
        }
    }
    // What this pass changed itself (level-1 nodes marked stale, chunks
    // asked for) it already took into account.
    residency_dirty_ = false;
    residency_had_camera_ = camera_chunk != nullptr;
    if (camera_chunk != nullptr) {
        residency_camera_ = *camera_chunk;
    }
}

void LodTree::next_builds(double now_ms, const ChunkCoord* camera_chunk, std::vector<NodeBuildRequest>& out) {
    refresh_top();
    struct Candidate {
        NodeKey key;
        int distance;
    };
    std::vector<Candidate> candidates;
    for (const auto& [key, node] : nodes_) {
        if (key.level == 0 || !node.stale() || node.queued_revision == node.revision ||
            now_ms - node.last_build_ms < kRebuildIntervalMs) {
            continue;
        }
        bool ready = true;
        for (const NodeKey& child : children_of(key)) {
            const Node* found = find(child);
            if (found != nullptr && !child_ready(child, *found)) {
                ready = false;
                break;
            }
        }
        if (ready) {
            candidates.push_back(Candidate{key, camera_chunk != nullptr ? linf_chunks(*camera_chunk, key) : 0});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return std::tie(a.key.level, a.distance, a.key.x, a.key.y, a.key.z) <
               std::tie(b.key.level, b.distance, b.key.x, b.key.y, b.key.z);
    });
    for (const Candidate& candidate : candidates) {
        Node& node = nodes_.at(candidate.key);
        NodeBuildRequest request;
        request.revision = node.revision;
        request.input.key = candidate.key;
        request.input.voxel_size = voxel_size_;
        for (const NodeKey& child_key : children_of(candidate.key)) {
            Node* child = find_mutable(child_key);
            if (child == nullptr || !child->has_surface) {
                continue;
            }
            if (child_key.level == 0) {
                if (child->chunk_mesh == nullptr) {
                    continue;   // its job failed with no mesh resident: build without it
                }
                // A missing child_surface_index_counts entry means the whole
                // mesh: right for a chunk mesh, which has no skirts.
                request.input.children.push_back(child->chunk_mesh);
            } else {
                if (child->compact == nullptr) {
                    continue;
                }
                // Unpacked for this job only (R12): the job owns it and it
                // goes when the build is done.
                request.input.children.push_back(std::make_shared<const anarchy::amesh::Data>(unpack(*child->compact)));
                request.input.child_errors.push_back(child->error);
                request.input.child_surface_index_counts.push_back(child->compact->surface_index_count);
            }
        }
        node.queued_revision = node.revision;
        node.last_build_ms = now_ms;
        out.push_back(std::move(request));
    }
}

std::vector<TerrainNodeView> LodTree::nodes_for_view() {
    refresh_top();
    std::vector<TerrainNodeView> out;
    for (auto& [key, node] : nodes_) {
        TerrainNodeView view;
        view.key = key;
        view.revision = node.mesh_revision;
        view.bounds_min = node.bounds_min;
        view.bounds_max = node.bounds_max;
        if (key.level == 0) {
            if (node.chunk_mesh == nullptr) {
                continue;
            }
            view.mesh = node.chunk_mesh;
        } else {
            if (!node.resident || !node.has_surface || node.compact == nullptr) {
                continue;
            }
            view.compact = node.compact;
            view.error = node.error;
            view.stale = node.stale();
            const std::array<NodeKey, 8> children = children_of(key);
            for (std::size_t i = 0; i < children.size(); ++i) {
                const Node* child = find(children[i]);
                if (child != nullptr && surfaced(children[i], *child)) {
                    view.child_mask = static_cast<std::uint8_t>(view.child_mask | (1u << i));
                }
            }
        }
        out.push_back(std::move(view));
    }
    std::sort(out.begin(), out.end(), [](const TerrainNodeView& a, const TerrainNodeView& b) {
        return std::tie(a.key.level, a.key.x, a.key.y, a.key.z) < std::tie(b.key.level, b.key.x, b.key.y, b.key.z);
    });
    return out;
}

bool LodTree::take_changed() {
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

}  // namespace engine_core::terrain
