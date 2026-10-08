#include "terrain/BlendWeights.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace engine_core::terrain {

namespace {

struct Entry {
    std::uint8_t id = 0;
    float weight = 0.f;
};

// Descending weight, ties broken by lower Id -- the one ordering rule used
// throughout this file, for picking a top-4 subset and for the vertex's own
// stored (sorted) slots.
bool by_weight_desc_id_asc(const Entry& a, const Entry& b) {
    if (a.weight != b.weight) {
        return a.weight > b.weight;
    }
    return a.id < b.id;
}

// The set of Ids a vertex carries with nonzero weight: at most 4, sorted and
// deduplicated, held inline (no heap allocation) since split_border_triangles
// computes one of these per vertex of the mesh -- cheap enough to precompute
// for every vertex once, rather than re-deriving it per corner per triangle.
struct IdSet {
    std::uint8_t ids[4] = {0, 0, 0, 0};
    int count = 0;
    bool operator==(const IdSet& o) const {
        if (count != o.count) {
            return false;
        }
        for (int k = 0; k < count; ++k) {
            if (ids[k] != o.ids[k]) {
                return false;
            }
        }
        return true;
    }
};

// decision 1: "Identical sets" compares Ids with nonzero weight as a set;
// weights may differ between corners.
IdSet id_set_of(const anarchy::amesh::Vertex& v) {
    IdSet out;
    for (int k = 0; k < 4; ++k) {
        if (v.t[k] > 0.f) {
            out.ids[out.count++] = v.rgba[k];
        }
    }
    std::sort(out.ids, out.ids + out.count);
    out.count = static_cast<int>(std::unique(out.ids, out.ids + out.count) - out.ids);
    return out;
}

// original's own weight for id, or 0 if original does not carry it.
float weight_for(const anarchy::amesh::Vertex& original, std::uint8_t id) {
    for (int k = 0; k < 4; ++k) {
        if (original.t[k] > 0.f && original.rgba[k] == id) {
            return original.t[k];
        }
    }
    return 0.f;
}

// Writes merged (the triangle's merged set: up to 4 entries in merged[0..
// merged_count), already sorted descending summed weight with ties by lower
// Id) into out's rgba/t slots, in that exact order, with t[k] = original's
// own weight for merged[k].id (0 if original does not carry it), renormalized
// so out's weights sum to 1. Using merged's order for every corner -- not
// each corner's own independent sort -- is the point: a split triangle's
// three corners end up with byte-identical rgba[0..3] (same Id per slot), so
// the vertex attribute the (future) shader reads is already consistent
// across the triangle without a geometry shader, and only the per-slot
// weight (t[]) needs interpolating. out starts as a copy of original, so
// position/normal/uv/etc. carry over unchanged.
void project_and_write(anarchy::amesh::Vertex& out, const anarchy::amesh::Vertex& original, const Entry* merged,
                        int merged_count) {
    out = original;
    float retained[4] = {0.f, 0.f, 0.f, 0.f};
    float sum = 0.f;
    for (int k = 0; k < merged_count; ++k) {
        retained[k] = weight_for(original, merged[k].id);
        sum += retained[k];
    }
    for (int k = 0; k < 4; ++k) {
        if (k < merged_count) {
            out.rgba[k] = merged[k].id;
            // sum <= 0 isn't expected (merged is the union of every corner's
            // own occupied Ids) unless more than 4 distinct Ids existed
            // across the triangle and this corner's own Ids were all
            // dropped from the kept top 4; leave the weight at 0 rather
            // than guess which of the triangle's (not this corner's own)
            // Ids to favor.
            out.t[k] = sum > 0.f ? retained[k] / sum : 0.f;
        } else {
            out.rgba[k] = 0;
            out.t[k] = 0.f;
        }
    }
}

}  // namespace

BlendIds blend_weights(const float distance[8], const std::uint8_t id[8], float voxel_size) {
    // Fixed, stack-held tallies -- at most 8 distinct Ids can ever appear
    // among 8 corners -- so a cell visited by every Surface Nets vertex (the
    // hot path) never allocates.
    std::uint8_t seen_id[8];
    int seen_count[8];
    int distinct = 0;
    int total = 0;
    for (int c = 0; c < 8; ++c) {
        if (std::fabs(distance[c]) > voxel_size) {
            continue;
        }
        const std::uint8_t corner_id = id[c];
        int slot = -1;
        for (int k = 0; k < distinct; ++k) {
            if (seen_id[k] == corner_id) {
                slot = k;
                break;
            }
        }
        if (slot < 0) {
            slot = distinct++;
            seen_id[slot] = corner_id;
            seen_count[slot] = 0;
        }
        ++seen_count[slot];
        ++total;
    }

    BlendIds out;
    if (total == 0) {
        int lowest = 0;
        for (int c = 1; c < 8; ++c) {
            if (distance[c] < distance[lowest]) {
                lowest = c;
            }
        }
        out.ids[0] = id[lowest];
        out.weights[0] = 1.f;
        return out;
    }

    Entry entries[8];
    for (int k = 0; k < distinct; ++k) {
        entries[k] = Entry{seen_id[k], static_cast<float>(seen_count[k]) / static_cast<float>(total)};
    }
    std::sort(entries, entries + distinct, by_weight_desc_id_asc);

    const int kept = std::min(distinct, 4);
    float sum = 0.f;
    for (int k = 0; k < kept; ++k) {
        sum += entries[k].weight;
    }
    for (int k = 0; k < kept; ++k) {
        out.ids[k] = entries[k].id;
        out.weights[k] = sum > 0.f ? entries[k].weight / sum : 0.f;
    }
    return out;
}

void split_border_triangles(anarchy::amesh::Data& render) {
    const std::size_t vertex_count = render.vertices.size();
    const std::size_t original_triangle_count = render.indices.size() / 3;

    // Each vertex's occupied-Id set, computed once rather than re-derived
    // per corner per triangle (an interior vertex is typically shared by
    // about 6 triangles): the one allocation this function pays up front
    // when it has any work to do at all, instead of one small vector per
    // id_set() call as a first version of this code did -- measurably
    // cheaper for a large chunk mesh, and this runs on every chunk Surface
    // Nets meshes plus every LOD node build_node re-shades.
    std::vector<IdSet> sets(vertex_count);
    for (std::size_t i = 0; i < vertex_count; ++i) {
        sets[i] = id_set_of(render.vertices[i]);
    }

    // Set once some triangle actually needs splitting. A single-material
    // mesh (the common case) never sets it, and the compaction pass below is
    // then skipped entirely -- leaving render's vertices/indices byte-for-
    // byte as they came in, rather than merely unchanged in content but
    // renumbered by first-use order. A caller that already recorded vertex
    // indices into this same Data (LodBuilder::build_node's border_edges,
    // collected before this call) depends on that: compacting even a
    // nothing-to-drop mesh would still renumber it and silently invalidate
    // those indices.
    bool any_split = false;
    for (std::size_t t = 0; t < original_triangle_count; ++t) {
        const std::size_t base = t * 3;
        const std::uint32_t i0 = render.indices[base];
        const std::uint32_t i1 = render.indices[base + 1];
        const std::uint32_t i2 = render.indices[base + 2];
        if (sets[i0] == sets[i1] && sets[i1] == sets[i2]) {
            continue;  // the triangle already agrees: keep its shared vertices
        }
        any_split = true;

        // The merged set: the top 4 Ids by summed weight across the 3
        // corners (a corner missing an Id contributes 0), ties by lower Id.
        // At most 12 entries before trimming (4 slots x 3 corners), held
        // inline.
        Entry merged[12];
        int merged_count = 0;
        auto accumulate = [&merged, &merged_count](const anarchy::amesh::Vertex& v) {
            for (int k = 0; k < 4; ++k) {
                if (v.t[k] <= 0.f) {
                    continue;
                }
                bool found = false;
                for (int m = 0; m < merged_count; ++m) {
                    if (merged[m].id == v.rgba[k]) {
                        merged[m].weight += v.t[k];
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    merged[merged_count++] = Entry{v.rgba[k], v.t[k]};
                }
            }
        };
        accumulate(render.vertices[i0]);
        accumulate(render.vertices[i1]);
        accumulate(render.vertices[i2]);
        std::sort(merged, merged + merged_count, by_weight_desc_id_asc);
        if (merged_count > 4) {
            merged_count = 4;
        }

        anarchy::amesh::Vertex v0, v1, v2;
        project_and_write(v0, render.vertices[i0], merged, merged_count);
        project_and_write(v1, render.vertices[i1], merged, merged_count);
        project_and_write(v2, render.vertices[i2], merged, merged_count);

        const auto n0 = static_cast<std::uint32_t>(render.vertices.size());
        render.vertices.push_back(v0);
        const auto n1 = static_cast<std::uint32_t>(render.vertices.size());
        render.vertices.push_back(v1);
        const auto n2 = static_cast<std::uint32_t>(render.vertices.size());
        render.vertices.push_back(v2);

        // This triangle alone gets its own 3 vertices; render.indices.size()
        // is unchanged (same triangle count throughout this loop), so
        // reassigning base/base+1/base+2 here never disturbs a later t's
        // own (still-unvisited) index slots. The 3 pushes above never touch
        // sets (sized to the vertex count read at entry), which is fine: a
        // newly split vertex is never read from sets again, only written to
        // render.vertices and indexed.
        render.indices[base] = n0;
        render.indices[base + 1] = n1;
        render.indices[base + 2] = n2;
    }

    if (!any_split) {
        return;  // nothing changed: leave vertices/indices exactly as they came in
    }

    // Compact: drop vertices no triangle indexes any more, renumbering what's
    // left in first-use order through indices.
    constexpr std::uint32_t kUnset = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> remap(render.vertices.size(), kUnset);
    std::vector<anarchy::amesh::Vertex> compacted;
    compacted.reserve(render.vertices.size());
    for (std::uint32_t& index : render.indices) {
        if (remap[index] == kUnset) {
            remap[index] = static_cast<std::uint32_t>(compacted.size());
            compacted.push_back(render.vertices[index]);
        }
        index = remap[index];
    }
    render.vertices = std::move(compacted);
}

}  // namespace engine_core::terrain
