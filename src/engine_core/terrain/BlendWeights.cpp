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

// The set of Ids v carries with nonzero weight, sorted and deduplicated, for
// set equality between a triangle's three corners (decision 1: "Identical
// sets" compares Ids with nonzero weight as a set; weights may differ).
std::vector<std::uint8_t> id_set(const anarchy::amesh::Vertex& v) {
    std::vector<std::uint8_t> ids;
    for (int k = 0; k < 4; ++k) {
        if (v.t[k] > 0.f) {
            ids.push_back(v.rgba[k]);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
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

// Writes merged (the triangle's merged set: up to 4 entries, already sorted
// descending summed weight with ties by lower Id) into out's rgba/t slots,
// in that exact order, with t[k] = original's own weight for merged[k].id
// (0 if original does not carry it), renormalized so out's weights sum to
// 1. Using merged's order for every corner -- not each corner's own
// independent sort -- is the point: a split triangle's three corners end up
// with byte-identical rgba[0..3] (same Id per slot), so the vertex
// attribute the (future) shader reads is already consistent across the
// triangle without a geometry shader, and only the per-slot weight (t[])
// needs interpolating. out starts as a copy of original, so
// position/normal/uv/etc. carry over unchanged.
void project_and_write(anarchy::amesh::Vertex& out, const anarchy::amesh::Vertex& original,
                        const std::vector<Entry>& merged) {
    out = original;
    float retained[4] = {0.f, 0.f, 0.f, 0.f};
    float sum = 0.f;
    for (std::size_t k = 0; k < merged.size() && k < 4; ++k) {
        retained[k] = weight_for(original, merged[k].id);
        sum += retained[k];
    }
    for (int k = 0; k < 4; ++k) {
        if (k < static_cast<int>(merged.size())) {
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
    std::vector<std::uint8_t> seen;
    std::vector<int> counts;
    int total = 0;
    for (int c = 0; c < 8; ++c) {
        if (std::fabs(distance[c]) > voxel_size) {
            continue;
        }
        const std::uint8_t corner_id = id[c];
        bool found = false;
        for (std::size_t k = 0; k < seen.size(); ++k) {
            if (seen[k] == corner_id) {
                counts[k] += 1;
                found = true;
                break;
            }
        }
        if (!found) {
            seen.push_back(corner_id);
            counts.push_back(1);
        }
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

    std::vector<Entry> entries;
    entries.reserve(seen.size());
    for (std::size_t k = 0; k < seen.size(); ++k) {
        entries.push_back(Entry{seen[k], static_cast<float>(counts[k]) / static_cast<float>(total)});
    }
    std::sort(entries.begin(), entries.end(), by_weight_desc_id_asc);

    const std::size_t kept = std::min<std::size_t>(entries.size(), 4);
    float sum = 0.f;
    for (std::size_t k = 0; k < kept; ++k) {
        sum += entries[k].weight;
    }
    for (std::size_t k = 0; k < kept; ++k) {
        out.ids[k] = entries[k].id;
        out.weights[k] = sum > 0.f ? entries[k].weight / sum : 0.f;
    }
    return out;
}

void split_border_triangles(anarchy::amesh::Data& render) {
    const std::size_t original_triangle_count = render.indices.size() / 3;
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
        const std::vector<std::uint8_t> set0 = id_set(render.vertices[i0]);
        const std::vector<std::uint8_t> set1 = id_set(render.vertices[i1]);
        const std::vector<std::uint8_t> set2 = id_set(render.vertices[i2]);
        if (set0 == set1 && set1 == set2) {
            continue;  // the triangle already agrees: keep its shared vertices
        }
        any_split = true;

        // The merged set: the top 4 Ids by summed weight across the 3
        // corners (a corner missing an Id contributes 0), ties by lower Id.
        std::vector<Entry> merged;
        auto accumulate = [&merged](const anarchy::amesh::Vertex& v) {
            for (int k = 0; k < 4; ++k) {
                if (v.t[k] <= 0.f) {
                    continue;
                }
                bool found = false;
                for (Entry& e : merged) {
                    if (e.id == v.rgba[k]) {
                        e.weight += v.t[k];
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    merged.push_back(Entry{v.rgba[k], v.t[k]});
                }
            }
        };
        accumulate(render.vertices[i0]);
        accumulate(render.vertices[i1]);
        accumulate(render.vertices[i2]);
        std::sort(merged.begin(), merged.end(), by_weight_desc_id_asc);
        if (merged.size() > 4) {
            merged.resize(4);
        }

        anarchy::amesh::Vertex v0, v1, v2;
        project_and_write(v0, render.vertices[i0], merged);
        project_and_write(v1, render.vertices[i1], merged);
        project_and_write(v2, render.vertices[i2], merged);

        const auto n0 = static_cast<std::uint32_t>(render.vertices.size());
        render.vertices.push_back(v0);
        const auto n1 = static_cast<std::uint32_t>(render.vertices.size());
        render.vertices.push_back(v1);
        const auto n2 = static_cast<std::uint32_t>(render.vertices.size());
        render.vertices.push_back(v2);

        // This triangle alone gets its own 3 vertices; render.indices.size()
        // is unchanged (same triangle count throughout this loop), so
        // reassigning base/base+1/base+2 here never disturbs a later t's
        // own (still-unvisited) index slots.
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
