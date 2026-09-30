#include "amesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <unordered_map>
#include <utility>

namespace anarchy::amesh {

namespace {

std::string Describe(std::uint64_t offset, const std::string& reason) {
    return "AMESH byte " + std::to_string(offset) + ": " + reason;
}

constexpr std::array<std::uint32_t, 256> MakeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[n] = c;
    }
    return table;
}
constexpr std::array<std::uint32_t, 256> kCrcTable = MakeCrcTable();

// Records are copied out, never cast in place: the buffer has no alignment promise.
template <typename T>
T Load(ByteSpan bytes, std::uint64_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void Store(std::vector<std::byte>& out, std::uint64_t offset, const T& value) {
    std::memcpy(out.data() + offset, &value, sizeof(T));
}

std::string Num(std::uint64_t value) {
    return std::to_string(value);
}

bool Finite(std::initializer_list<float> values) {
    for (const float value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}

// UTF-8 without NUL, overlong forms, surrogates, or code points past U+10FFFF.
bool ValidName(const unsigned char* s, std::size_t n) {
    std::size_t i = 0;
    while (i < n) {
        const unsigned lead = s[i];
        if (lead == 0) {
            return false;
        }
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length;
        std::uint32_t cp;
        std::uint32_t least;
        if ((lead & 0xE0) == 0xC0) {
            length = 2, cp = lead & 0x1F, least = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3, cp = lead & 0x0F, least = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4, cp = lead & 0x07, least = 0x10000;
        } else {
            return false;
        }
        if (n - i < length) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const unsigned next = s[i + k];
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (next & 0x3F);
        }
        if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

// Where each array starts, from the header's counts and flags.
struct Layout {
    AEHeader header;
    std::uint64_t vertices = 0;
    std::uint64_t skins = 0;
    std::uint64_t triangles = 0;
    std::uint64_t lods = 0;
    std::uint64_t bones = 0;
    std::uint64_t names = 0;
    std::uint64_t subsets = 0;
    std::uint64_t crc = 0;
    std::uint64_t size = 0;
};

Layout Plan(const AEHeader& h) {
    const bool skinned = (h.flags & FLAG_SKINNED) != 0;
    Layout l{};
    l.header = h;
    std::uint64_t at = kHeaderSize;
    l.vertices = at;
    at += std::uint64_t{h.vertex_count} * kVertexSize;
    l.skins = at;
    if (skinned) {
        at += std::uint64_t{h.vertex_count} * kSkinSize;
    }
    l.triangles = at;
    at += std::uint64_t{h.triangle_count} * kTriangleSize;
    l.lods = at;
    if (h.flags & FLAG_LODS) {
        at += std::uint64_t{h.lod_count} * sizeof(std::uint32_t);
    }
    l.bones = at;
    if (skinned) {
        at += std::uint64_t{h.bone_count} * kBoneSize;
    }
    l.names = at;
    at += h.name_blob_size;
    l.subsets = at;
    if (h.flags & FLAG_SUBSETS) {
        at += std::uint64_t{h.subset_count} * kSubsetSize;
    }
    l.crc = at;
    l.size = at + kCrcSize;
    return l;
}

void CheckHeader(const AEHeader& h) {
    if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) {
        throw AEMeshError(offsetof(AEHeader, magic), "the magic is not \"AESH\"");
    }
    if (h.version_major != kVersionMajor || h.version_minor != kVersionMinor) {
        throw AEMeshError(offsetof(AEHeader, version_major),
                          "version " + Num(h.version_major) + "." + Num(h.version_minor) + " is not 1.0");
    }
    if (h.header_size != kHeaderSize) {
        throw AEMeshError(offsetof(AEHeader, header_size), "header_size is " + Num(h.header_size) + ", not 64");
    }
    if ((h.flags & ~kKnownFlags) != 0) {
        throw AEMeshError(offsetof(AEHeader, flags), "unknown flag bits are set: " + Num(h.flags & ~kKnownFlags));
    }
    if (h.reserved1 != 0) {
        throw AEMeshError(offsetof(AEHeader, reserved1), "reserved1 is not 0");
    }
    if (h.reserved2 != 0) {
        throw AEMeshError(offsetof(AEHeader, reserved2), "reserved2 is not 0");
    }

    struct Limit {
        std::uint64_t offset;
        const char* name;
        std::uint64_t value;
        std::uint64_t most;
    };
    const Limit limits[] = {
        {offsetof(AEHeader, vertex_count), "vertex_count", h.vertex_count, kMaxVertices},
        {offsetof(AEHeader, triangle_count), "triangle_count", h.triangle_count, kMaxTriangles},
        {offsetof(AEHeader, bone_count), "bone_count", h.bone_count, kMaxBones},
        {offsetof(AEHeader, subset_count), "subset_count", h.subset_count, kMaxSubsets},
        {offsetof(AEHeader, lod_count), "lod_count", h.lod_count, kMaxLods},
        {offsetof(AEHeader, name_blob_size), "name_blob_size", h.name_blob_size, kMaxNameBlob},
    };
    for (const Limit& limit : limits) {
        if (limit.value > limit.most) {
            throw AEMeshError(limit.offset, std::string(limit.name) + " " + Num(limit.value) + " is over the limit of " +
                                                Num(limit.most));
        }
    }

    const bool skinned = (h.flags & FLAG_SKINNED) != 0;
    if (skinned != (h.bone_count >= 1)) {
        throw AEMeshError(offsetof(AEHeader, bone_count),
                          "FLAG_SKINNED and bone_count " + Num(h.bone_count) + " disagree");
    }
    const bool lods = (h.flags & FLAG_LODS) != 0;
    if (lods ? h.lod_count < 2 : h.lod_count != 0) {
        throw AEMeshError(offsetof(AEHeader, lod_count), "FLAG_LODS and lod_count " + Num(h.lod_count) + " disagree");
    }
    const bool subsets = (h.flags & FLAG_SUBSETS) != 0;
    if (subsets != (h.subset_count >= 1)) {
        throw AEMeshError(offsetof(AEHeader, subset_count),
                          "FLAG_SUBSETS and subset_count " + Num(h.subset_count) + " disagree");
    }

    // Unspecified by the format, so rejected: a generator past 3, a LOD-less
    // mesh naming a generator, LODs claiming "none", or a quality byte past 1.
    if (h.lod_generator > kLodGeneratorMeshoptimizer || (lods ? h.lod_generator == kLodGeneratorNone
                                                              : h.lod_generator != kLodGeneratorNone)) {
        throw AEMeshError(offsetof(AEHeader, lod_generator),
                          "lod_generator " + Num(h.lod_generator) + " does not fit the mesh's LODs");
    }
    if (h.high_quality_lods > 1 || (!lods && h.high_quality_lods != 0)) {
        throw AEMeshError(offsetof(AEHeader, high_quality_lods),
                          "high_quality_lods " + Num(h.high_quality_lods) + " does not fit the mesh's LODs");
    }

    for (int axis = 0; axis < 3; ++axis) {
        const float lo = h.bbox_min[axis];
        const float hi = h.bbox_max[axis];
        if (!Finite({lo, hi}) || lo > hi) {
            throw AEMeshError(offsetof(AEHeader, bbox_min), "the bounding box is not finite, or min exceeds max");
        }
        if (h.vertex_count == 0 && (lo != 0 || hi != 0)) {
            throw AEMeshError(offsetof(AEHeader, bbox_min), "an empty mesh's bounding box must be all zeros");
        }
    }
}

void CheckVertices(ByteSpan bytes, const Layout& l) {
    const AEHeader& h = l.header;
    const bool tangents = (h.flags & FLAG_TANGENTS) != 0;
    const bool colors = (h.flags & FLAG_VERTEX_COLOR) != 0;
    for (std::uint32_t i = 0; i < h.vertex_count; ++i) {
        const std::uint64_t at = l.vertices + std::uint64_t{i} * kVertexSize;
        const auto v = Load<AEVertex>(bytes, at);
        if (!Finite({v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.u, v.v})) {
            throw AEMeshError(at, "vertex " + Num(i) + " has a value that is not finite");
        }
        const float p[3] = {v.px, v.py, v.pz};
        for (int axis = 0; axis < 3; ++axis) {
            if (p[axis] < h.bbox_min[axis] || p[axis] > h.bbox_max[axis]) {
                throw AEMeshError(at, "vertex " + Num(i) + " is outside the header's bounding box");
            }
        }
        if (tangents) {
            // Unspecified: a zero sign gives no bitangent, so it is rejected.
            if (v.ts == 0) {
                throw AEMeshError(at + offsetof(AEVertex, ts), "vertex " + Num(i) + " has a bitangent sign of 0");
            }
        } else if (v.tx != 0 || v.ty != 0 || v.tz != 0 || v.ts != 127) {
            throw AEMeshError(at + offsetof(AEVertex, tx),
                              "vertex " + Num(i) + " stores a tangent, but FLAG_TANGENTS is clear");
        }
        if (!colors && (v.r != 255 || v.g != 255 || v.b != 255 || v.a != 255)) {
            throw AEMeshError(at + offsetof(AEVertex, r),
                              "vertex " + Num(i) + " is not white, but FLAG_VERTEX_COLOR is clear");
        }
    }
}

void CheckSkins(ByteSpan bytes, const Layout& l) {
    const AEHeader& h = l.header;
    if ((h.flags & FLAG_SKINNED) == 0) {
        return;
    }
    for (std::uint32_t i = 0; i < h.vertex_count; ++i) {
        const std::uint64_t at = l.skins + std::uint64_t{i} * kSkinSize;
        const auto s = Load<AESkin>(bytes, at);
        for (int k = 0; k < 4; ++k) {
            const std::uint16_t bone = s.bone_index[k];
            const std::uint8_t weight = s.weight[k];
            if ((bone == kNoBone) != (weight == 0)) {
                throw AEMeshError(at + 2 * k, "vertex " + Num(i) + " pairs bone " + Num(bone) + " with weight " +
                                                  Num(weight) + "; an unused slot is (0xFFFF, 0)");
            }
            if (bone == kNoBone) {
                continue;
            }
            if (bone >= h.bone_count) {
                throw AEMeshError(at + 2 * k, "vertex " + Num(i) + " names bone " + Num(bone) + " of " +
                                                  Num(h.bone_count));
            }
            // Unspecified: one bone twice in a vertex is rejected.
            for (int j = 0; j < k; ++j) {
                if (s.bone_index[j] == bone) {
                    throw AEMeshError(at + 2 * k, "vertex " + Num(i) + " names bone " + Num(bone) + " twice");
                }
            }
        }
    }
}

void CheckTriangles(ByteSpan bytes, const Layout& l) {
    const AEHeader& h = l.header;
    for (std::uint32_t i = 0; i < h.triangle_count; ++i) {
        const std::uint64_t at = l.triangles + std::uint64_t{i} * kTriangleSize;
        const auto t = Load<AETriangle>(bytes, at);
        const std::uint32_t corners[3] = {t.i0, t.i1, t.i2};
        for (int k = 0; k < 3; ++k) {
            if (corners[k] >= h.vertex_count) {
                throw AEMeshError(at + 4 * k, "triangle " + Num(i) + " names vertex " + Num(corners[k]) + " of " +
                                                  Num(h.vertex_count));
            }
        }
    }
}

void CheckLods(ByteSpan bytes, const Layout& l) {
    const AEHeader& h = l.header;
    if ((h.flags & FLAG_LODS) == 0) {
        return;
    }
    std::uint32_t previous = 0;
    for (std::uint16_t k = 0; k < h.lod_count; ++k) {
        const std::uint64_t at = l.lods + std::uint64_t{k} * sizeof(std::uint32_t);
        const auto end = Load<std::uint32_t>(bytes, at);
        if (end <= previous) {
            throw AEMeshError(at, "lod_face_end[" + Num(k) + "] = " + Num(end) + " does not increase");
        }
        if (end > h.triangle_count) {
            throw AEMeshError(at, "lod_face_end[" + Num(k) + "] = " + Num(end) + " is past the last triangle");
        }
        previous = end;
    }
    if (previous != h.triangle_count) {
        throw AEMeshError(l.lods + (h.lod_count - 1) * sizeof(std::uint32_t),
                          "the last lod_face_end is not triangle_count");
    }
}

void CheckBones(ByteSpan bytes, const Layout& l, bool strict_parents) {
    const AEHeader& h = l.header;
    if ((h.flags & FLAG_SKINNED) == 0) {
        return;
    }
    const auto* blob = reinterpret_cast<const unsigned char*>(bytes.data() + l.names);
    std::vector<std::uint16_t> parents(h.bone_count);
    for (std::uint16_t b = 0; b < h.bone_count; ++b) {
        const std::uint64_t at = l.bones + std::uint64_t{b} * kBoneSize;
        const auto bone = Load<AEBone>(bytes, at);
        if (bone.name_offset != kNoName && bone.name_length != 0) {
            if (std::uint64_t{bone.name_offset} + bone.name_length > h.name_blob_size) {
                throw AEMeshError(at, "bone " + Num(b) + "'s name runs past the name blob");
            }
            if (!ValidName(blob + bone.name_offset, bone.name_length)) {
                throw AEMeshError(l.names + bone.name_offset, "bone " + Num(b) + "'s name is not UTF-8 without NUL");
            }
        }
        if (bone.parent != kNoBone && (bone.parent == b || bone.parent >= h.bone_count)) {
            throw AEMeshError(at + offsetof(AEBone, parent), "bone " + Num(b) + " has parent " + Num(bone.parent));
        }
        // Unspecified: lod_parent must name a bone or be 0xFFFF.
        if (bone.lod_parent != kNoBone && bone.lod_parent >= h.bone_count) {
            throw AEMeshError(at + offsetof(AEBone, lod_parent),
                              "bone " + Num(b) + " has lod_parent " + Num(bone.lod_parent));
        }
        if (!Finite({bone.cull_radius, bone.m00, bone.m01, bone.m02, bone.m10, bone.m11, bone.m12, bone.m20, bone.m21,
                     bone.m22, bone.tx, bone.ty, bone.tz}) ||
            bone.cull_radius < 0) {
            throw AEMeshError(at + offsetof(AEBone, cull_radius),
                              "bone " + Num(b) + " has a value that is not finite, or a negative cull radius");
        }
        parents[b] = bone.parent;
    }
    if (!strict_parents) {
        return;
    }
    // 0 unvisited, 1 on the walk now, 2 known to reach a root.
    std::vector<std::uint8_t> state(h.bone_count, 0);
    std::vector<std::uint16_t> walk;
    for (std::uint16_t b = 0; b < h.bone_count; ++b) {
        walk.clear();
        std::uint16_t at = b;
        while (at != kNoBone && state[at] != 2) {
            if (state[at] == 1) {
                throw AEMeshError(l.bones + std::uint64_t{at} * kBoneSize + offsetof(AEBone, parent),
                                  "bone " + Num(at) + "'s parents form a cycle");
            }
            state[at] = 1;
            walk.push_back(at);
            at = parents[at];
        }
        for (const std::uint16_t done : walk) {
            state[done] = 2;
        }
    }
}

void CheckSubsets(ByteSpan bytes, const Layout& l) {
    const AEHeader& h = l.header;
    if ((h.flags & FLAG_SUBSETS) == 0) {
        return;
    }
    for (std::uint16_t s = 0; s < h.subset_count; ++s) {
        const std::uint64_t at = l.subsets + std::uint64_t{s} * kSubsetSize;
        const auto sub = Load<AESubset>(bytes, at);
        // Unspecified: an empty subset draws nothing, so it is rejected.
        if (sub.tri_count == 0 || std::uint64_t{sub.tri_begin} + sub.tri_count > h.triangle_count) {
            throw AEMeshError(at, "subset " + Num(s) + "'s triangles are empty or past the mesh");
        }
        const std::uint64_t vert_end = std::uint64_t{sub.vert_begin} + sub.vert_count;
        if (vert_end > h.vertex_count) {
            throw AEMeshError(at + offsetof(AESubset, vert_begin), "subset " + Num(s) + "'s vertices are past the mesh");
        }
        if (sub.bone_count > kMaxSubsetBones) {
            throw AEMeshError(at + offsetof(AESubset, bone_count),
                              "subset " + Num(s) + " lists " + Num(sub.bone_count) + " bones, over 26");
        }
        if (sub.bone_count != 0 && (h.flags & FLAG_SKINNED) == 0) {
            throw AEMeshError(at + offsetof(AESubset, bone_count),
                              "subset " + Num(s) + " lists bones, but the mesh has no skeleton");
        }
        for (std::uint32_t k = 0; k < kMaxSubsetBones; ++k) {
            const std::uint16_t bone = sub.bones[k];
            const std::uint64_t bone_at = at + offsetof(AESubset, bones) + 2 * k;
            if (k >= sub.bone_count) {
                // Unspecified: slots past bone_count must be 0xFFFF.
                if (bone != kNoBone) {
                    throw AEMeshError(bone_at, "subset " + Num(s) + " has a bone past its bone_count");
                }
                continue;
            }
            if (bone >= h.bone_count) {
                throw AEMeshError(bone_at, "subset " + Num(s) + " names bone " + Num(bone) + " of " +
                                               Num(h.bone_count));
            }
            for (std::uint32_t j = 0; j < k; ++j) {
                if (sub.bones[j] == bone) {
                    throw AEMeshError(bone_at, "subset " + Num(s) + " names bone " + Num(bone) + " twice");
                }
            }
        }
        // Unspecified: a subset's triangles must stay inside its vertex range.
        for (std::uint32_t i = sub.tri_begin; i < sub.tri_begin + sub.tri_count; ++i) {
            const std::uint64_t tri_at = l.triangles + std::uint64_t{i} * kTriangleSize;
            const auto t = Load<AETriangle>(bytes, tri_at);
            for (const std::uint32_t corner : {t.i0, t.i1, t.i2}) {
                if (corner < sub.vert_begin || corner >= vert_end) {
                    throw AEMeshError(tri_at, "triangle " + Num(i) + " leaves subset " + Num(s) + "'s vertex range");
                }
            }
        }
    }
}

// Everything read() rejects, without building a Data.
Layout Validate(ByteSpan bytes, const ReadOptions& options) {
    const std::uint64_t size = bytes.size();
    if (size < kHeaderSize) {
        throw AEMeshError(size, "the file is " + Num(size) + " bytes; the header alone is 64");
    }
    if (size > kMaxFileSize) {
        throw AEMeshError(0, "the file is " + Num(size) + " bytes, over the 512 MiB limit");
    }
    const auto header = Load<AEHeader>(bytes, 0);
    CheckHeader(header);
    const Layout l = Plan(header);
    if (l.size != size) {
        throw AEMeshError(std::min(l.size, size), "the header describes " + Num(l.size) + " bytes, but the file is " +
                                                      Num(size));
    }
    if (options.verify_crc) {
        const auto stored = Load<std::uint32_t>(bytes, l.crc);
        const std::uint32_t computed = crc32(bytes.first(static_cast<std::size_t>(l.crc)));
        if (stored != computed) {
            throw AEMeshError(l.crc, "the CRC is " + Num(stored) + ", but the bytes give " + Num(computed));
        }
    }
    CheckVertices(bytes, l);
    CheckSkins(bytes, l);
    CheckTriangles(bytes, l);
    CheckLods(bytes, l);
    CheckBones(bytes, l, options.strict_parents);
    CheckSubsets(bytes, l);
    return l;
}

Data Decode(ByteSpan bytes, const Layout& l) {
    const AEHeader& h = l.header;
    Data data;
    data.flags = h.flags;
    data.lod_generator = h.lod_generator;
    data.high_quality_lods = h.high_quality_lods != 0;
    std::copy(std::begin(h.bbox_min), std::end(h.bbox_min), data.bbox_min);
    std::copy(std::begin(h.bbox_max), std::end(h.bbox_max), data.bbox_max);

    const bool tangents = (h.flags & FLAG_TANGENTS) != 0;
    data.vertices.resize(h.vertex_count);
    for (std::uint32_t i = 0; i < h.vertex_count; ++i) {
        const auto v = Load<AEVertex>(bytes, l.vertices + std::uint64_t{i} * kVertexSize);
        Vertex& out = data.vertices[i];
        out.p[0] = v.px, out.p[1] = v.py, out.p[2] = v.pz;
        out.n[0] = v.nx, out.n[1] = v.ny, out.n[2] = v.nz;
        out.uv[0] = v.u, out.uv[1] = v.v;
        if (tangents) {
            const std::int8_t t[3] = {v.tx, v.ty, v.tz};
            for (int k = 0; k < 3; ++k) {
                out.t[k] = std::clamp(t[k] / 127.0f, -1.0f, 1.0f);
            }
            out.t[3] = v.ts < 0 ? -1.0f : 1.0f;
        }
        out.rgba[0] = v.r, out.rgba[1] = v.g, out.rgba[2] = v.b, out.rgba[3] = v.a;
    }

    if (h.flags & FLAG_SKINNED) {
        for (std::uint32_t i = 0; i < h.vertex_count; ++i) {
            const auto s = Load<AESkin>(bytes, l.skins + std::uint64_t{i} * kSkinSize);
            unsigned sum = 0;
            for (const std::uint8_t w : s.weight) {
                sum += w;
            }
            Vertex& out = data.vertices[i];
            for (int k = 0; k < 4; ++k) {
                out.bone[k] = s.bone_index[k];
                // Dividing by the sum is w / 255 for a well-formed vertex, and renormalizes the rest.
                out.weight[k] = sum == 0 ? 0.0f : static_cast<float>(s.weight[k]) / static_cast<float>(sum);
            }
        }
    }

    data.indices.resize(std::size_t{h.triangle_count} * 3);
    if (!data.indices.empty()) {
        std::memcpy(data.indices.data(), bytes.data() + l.triangles, data.indices.size() * sizeof(std::uint32_t));
    }

    if (h.flags & FLAG_LODS) {
        std::uint32_t begin = 0;
        for (std::uint16_t k = 0; k < h.lod_count; ++k) {
            const auto end = Load<std::uint32_t>(bytes, l.lods + std::uint64_t{k} * sizeof(std::uint32_t));
            data.lods.push_back({begin, end - begin});
            begin = end;
        }
    } else {
        data.lods.push_back({0, h.triangle_count});
    }

    const auto* blob = reinterpret_cast<const char*>(bytes.data() + l.names);
    data.bones.resize(h.bone_count);
    for (std::uint16_t b = 0; b < h.bone_count; ++b) {
        const auto bone = Load<AEBone>(bytes, l.bones + std::uint64_t{b} * kBoneSize);
        Bone& out = data.bones[b];
        if (bone.name_offset != kNoName && bone.name_length != 0) {
            out.name.assign(blob + bone.name_offset, bone.name_length);
        }
        out.parent = bone.parent;
        out.lod_parent = bone.lod_parent;
        out.cull_radius = bone.cull_radius;
        const float m[3][3] = {{bone.m00, bone.m01, bone.m02}, {bone.m10, bone.m11, bone.m12},
                               {bone.m20, bone.m21, bone.m22}};
        std::memcpy(out.m, m, sizeof(m));
        out.t[0] = bone.tx, out.t[1] = bone.ty, out.t[2] = bone.tz;
    }

    data.subsets.resize(h.subset_count);
    for (std::uint16_t s = 0; s < h.subset_count; ++s) {
        const auto sub = Load<AESubset>(bytes, l.subsets + std::uint64_t{s} * kSubsetSize);
        Subset& out = data.subsets[s];
        out.tri_begin = sub.tri_begin;
        out.tri_count = sub.tri_count;
        out.vert_begin = sub.vert_begin;
        out.vert_count = sub.vert_count;
        out.bones.assign(sub.bones, sub.bones + sub.bone_count);
    }
    return data;
}

// Four influences, largest first, as bytes summing to 255. Unused slots are (0xFFFF, 0).
AESkin QuantizeSkin(const Vertex& v, std::size_t index, std::uint64_t at, std::size_t bone_count) {
    struct Slot {
        std::uint16_t bone;
        double weight;
        unsigned quantized;
        double remainder;
    };
    Slot slots[4];
    int used = 0;
    double total = 0;
    for (int k = 0; k < 4; ++k) {
        const float weight = v.weight[k];
        const std::uint16_t bone = v.bone[k];
        if (!std::isfinite(weight) || weight < 0) {
            throw AEMeshError(at, "vertex " + Num(index) + " has a negative or non-finite weight");
        }
        if (bone == kNoBone) {
            if (weight != 0) {
                throw AEMeshError(at, "vertex " + Num(index) + " puts weight on an unused slot");
            }
            continue;
        }
        if (bone >= bone_count) {
            throw AEMeshError(at, "vertex " + Num(index) + " names bone " + Num(bone) + " of " + Num(bone_count));
        }
        for (int j = 0; j < used; ++j) {
            if (slots[j].bone == bone) {
                throw AEMeshError(at, "vertex " + Num(index) + " names bone " + Num(bone) + " twice");
            }
        }
        if (weight == 0) {
            continue;
        }
        slots[used++] = {bone, weight, 0, 0};
        total += weight;
    }

    // Largest remainder, so the bytes sum to exactly 255.
    unsigned sum = 0;
    for (int k = 0; k < used; ++k) {
        const double scaled = slots[k].weight / total * 255.0;
        slots[k].quantized = static_cast<unsigned>(std::floor(scaled));
        slots[k].remainder = scaled - slots[k].quantized;
        sum += slots[k].quantized;
    }
    std::stable_sort(slots, slots + used, [](const Slot& a, const Slot& b) { return a.remainder > b.remainder; });
    for (int k = 0; k < used && sum < 255; ++k, ++sum) {
        ++slots[k].quantized;
    }
    std::stable_sort(slots, slots + used, [](const Slot& a, const Slot& b) { return a.quantized > b.quantized; });

    AESkin skin;
    int written = 0;
    for (int k = 0; k < used; ++k) {
        if (slots[k].quantized == 0) {
            continue;
        }
        skin.bone_index[written] = slots[k].bone;
        skin.weight[written] = static_cast<std::uint8_t>(slots[k].quantized);
        ++written;
    }
    for (; written < 4; ++written) {
        skin.bone_index[written] = kNoBone;
        skin.weight[written] = 0;
    }
    return skin;
}

std::int8_t Snorm8(float value) {
    return static_cast<std::int8_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 127.0f));
}

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

Vec3 Sub(const float* a, const float* b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool Normalize(Vec3& v) {
    const float length = std::sqrt(Dot(v, v));
    if (!(length > 1e-20f) || !std::isfinite(length)) {
        return false;
    }
    v = {v.x / length, v.y / length, v.z / length};
    return true;
}

}  // namespace

AEMeshError::AEMeshError(std::uint64_t offset, std::string why)
    : std::runtime_error(Describe(offset, why)), byte_offset(offset), reason(std::move(why)) {}

std::uint32_t crc32(ByteSpan bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::byte b : bytes) {
        crc = kCrcTable[(crc ^ std::to_integer<std::uint32_t>(b)) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

Data read(ByteSpan bytes, ReadOptions options) {
    const Layout layout = Validate(bytes, options);
    return Decode(bytes, layout);
}

std::vector<std::byte> write(const Data& data, WriteOptions options) {
    const std::size_t vertex_count = data.vertices.size();
    if (vertex_count > kMaxVertices) {
        throw AEMeshError(offsetof(AEHeader, vertex_count), Num(vertex_count) + " vertices is over the limit");
    }
    if (data.indices.size() % 3 != 0) {
        throw AEMeshError(offsetof(AEHeader, triangle_count), "the index count is not a multiple of 3");
    }
    const std::size_t triangle_count = data.indices.size() / 3;
    if (triangle_count > kMaxTriangles) {
        throw AEMeshError(offsetof(AEHeader, triangle_count), Num(triangle_count) + " triangles is over the limit");
    }
    if (data.bones.size() > kMaxBones) {
        throw AEMeshError(offsetof(AEHeader, bone_count), Num(data.bones.size()) + " bones is over the limit");
    }
    if (data.subsets.size() > kMaxSubsets) {
        throw AEMeshError(offsetof(AEHeader, subset_count), Num(data.subsets.size()) + " subsets is over the limit");
    }
    if (data.lods.size() > kMaxLods) {
        throw AEMeshError(offsetof(AEHeader, lod_count), Num(data.lods.size()) + " LODs is over the limit");
    }
    if (data.lod_generator > kLodGeneratorMeshoptimizer) {
        throw AEMeshError(offsetof(AEHeader, lod_generator), "lod_generator " + Num(data.lod_generator) + " is unknown");
    }

    // One LOD, which read() gives even an empty mesh, is the same as none. Two
    // or more are consecutive, non-empty, and cover every triangle.
    std::vector<std::uint32_t> lod_ends;
    if (data.lods.size() == 1 && (data.lods[0].tri_begin != 0 || data.lods[0].tri_count != triangle_count)) {
        throw AEMeshError(offsetof(AEHeader, lod_count), "a single LOD must cover every triangle");
    }
    if (data.lods.size() >= 2) {
        std::uint64_t expected = 0;
        for (std::size_t k = 0; k < data.lods.size(); ++k) {
            const LodRange& lod = data.lods[k];
            if (lod.tri_begin != expected || lod.tri_count == 0) {
                throw AEMeshError(offsetof(AEHeader, lod_count),
                                  "LOD " + Num(k) + " does not start where the one before ends, or is empty");
            }
            expected += lod.tri_count;
            lod_ends.push_back(static_cast<std::uint32_t>(expected));
        }
        if (expected != triangle_count) {
            throw AEMeshError(offsetof(AEHeader, lod_count), "the LODs do not cover every triangle");
        }
    }
    const bool has_lods = !lod_ends.empty();

    // Names are packed once each; bones with the same name share an offset.
    struct NameRef {
        std::uint32_t offset = kNoName;
        std::uint32_t length = 0;
    };
    std::vector<NameRef> names(data.bones.size());
    std::vector<std::byte> blob;
    std::unordered_map<std::string, std::uint32_t> packed;
    for (std::size_t b = 0; b < data.bones.size(); ++b) {
        const std::string& name = data.bones[b].name;
        if (name.empty()) {
            continue;
        }
        if (!ValidName(reinterpret_cast<const unsigned char*>(name.data()), name.size())) {
            throw AEMeshError(offsetof(AEHeader, name_blob_size),
                              "bone " + Num(b) + "'s name is not UTF-8 without NUL");
        }
        auto [it, inserted] = packed.try_emplace(name, static_cast<std::uint32_t>(blob.size()));
        if (inserted) {
            if (blob.size() + name.size() > kMaxNameBlob) {
                throw AEMeshError(offsetof(AEHeader, name_blob_size), "the bone names are over the 1 MiB limit");
            }
            const auto* chars = reinterpret_cast<const std::byte*>(name.data());
            blob.insert(blob.end(), chars, chars + name.size());
        }
        names[b] = {it->second, static_cast<std::uint32_t>(name.size())};
    }

    for (std::size_t s = 0; s < data.subsets.size(); ++s) {
        if (data.subsets[s].bones.size() > kMaxSubsetBones) {
            throw AEMeshError(offsetof(AEHeader, subset_count),
                              "subset " + Num(s) + " lists " + Num(data.subsets[s].bones.size()) + " bones, over 26");
        }
    }

    const bool skinned = !data.bones.empty();
    bool any_tangent = false;
    bool any_color = false;
    bool uv_unorm = vertex_count > 0;
    float lo[3] = {0, 0, 0};
    float hi[3] = {0, 0, 0};
    for (std::size_t i = 0; i < vertex_count; ++i) {
        const Vertex& v = data.vertices[i];
        if (!Finite({v.p[0], v.p[1], v.p[2], v.n[0], v.n[1], v.n[2], v.uv[0], v.uv[1], v.t[0], v.t[1], v.t[2], v.t[3]})) {
            throw AEMeshError(kHeaderSize + i * kVertexSize, "vertex " + Num(i) + " has a value that is not finite");
        }
        for (int axis = 0; axis < 3; ++axis) {
            lo[axis] = i == 0 ? v.p[axis] : std::min(lo[axis], v.p[axis]);
            hi[axis] = i == 0 ? v.p[axis] : std::max(hi[axis], v.p[axis]);
        }
        any_tangent = any_tangent || v.t[0] != 0 || v.t[1] != 0 || v.t[2] != 0;
        any_color = any_color || v.rgba[0] != 255 || v.rgba[1] != 255 || v.rgba[2] != 255 || v.rgba[3] != 255;
        uv_unorm = uv_unorm && v.uv[0] >= 0 && v.uv[0] <= 1 && v.uv[1] >= 0 && v.uv[1] <= 1;
    }

    std::uint16_t flags = 0;
    if (skinned) {
        flags |= FLAG_SKINNED;
    }
    if (has_lods) {
        flags |= FLAG_LODS;
    }
    if (!data.subsets.empty()) {
        flags |= FLAG_SUBSETS;
    }
    if (options.write_colors && ((data.flags & FLAG_VERTEX_COLOR) || any_color)) {
        flags |= FLAG_VERTEX_COLOR;
    }
    if (options.write_tangents && ((data.flags & FLAG_TANGENTS) || any_tangent)) {
        flags |= FLAG_TANGENTS;
    }
    if (options.write_uv_unorm_flag && ((data.flags & FLAG_UNORM_UV) || uv_unorm)) {
        flags |= FLAG_UNORM_UV;
    }
    const bool tangents = (flags & FLAG_TANGENTS) != 0;
    const bool colors = (flags & FLAG_VERTEX_COLOR) != 0;

    AEHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version_major = kVersionMajor;
    header.version_minor = kVersionMinor;
    header.header_size = static_cast<std::uint16_t>(kHeaderSize);
    header.flags = flags;
    header.vertex_count = static_cast<std::uint32_t>(vertex_count);
    header.triangle_count = static_cast<std::uint32_t>(triangle_count);
    header.lod_count = static_cast<std::uint16_t>(lod_ends.size());
    header.bone_count = static_cast<std::uint16_t>(data.bones.size());
    header.name_blob_size = static_cast<std::uint32_t>(blob.size());
    header.subset_count = static_cast<std::uint16_t>(data.subsets.size());
    if (has_lods) {
        header.lod_generator = data.lod_generator == kLodGeneratorNone ? kLodGeneratorUnknown : data.lod_generator;
        header.high_quality_lods = static_cast<std::uint8_t>(data.high_quality_lods ? 1 : 0);
    }
    std::copy(std::begin(lo), std::end(lo), header.bbox_min);
    std::copy(std::begin(hi), std::end(hi), header.bbox_max);

    const Layout l = Plan(header);
    if (l.size > kMaxFileSize) {
        throw AEMeshError(0, "the file would be " + Num(l.size) + " bytes, over the 512 MiB limit");
    }
    std::vector<std::byte> out(static_cast<std::size_t>(l.size));
    Store(out, 0, header);

    for (std::size_t i = 0; i < vertex_count; ++i) {
        const Vertex& v = data.vertices[i];
        AEVertex packed_vertex{};
        packed_vertex.px = v.p[0], packed_vertex.py = v.p[1], packed_vertex.pz = v.p[2];
        Vec3 normal{v.n[0], v.n[1], v.n[2]};
        if (!Normalize(normal)) {
            normal = {0, 0, 0};
        }
        packed_vertex.nx = normal.x, packed_vertex.ny = normal.y, packed_vertex.nz = normal.z;
        packed_vertex.u = v.uv[0], packed_vertex.v = v.uv[1];
        if (tangents) {
            Vec3 tangent{v.t[0], v.t[1], v.t[2]};
            if (!Normalize(tangent)) {
                tangent = {0, 0, 0};
            }
            packed_vertex.tx = Snorm8(tangent.x);
            packed_vertex.ty = Snorm8(tangent.y);
            packed_vertex.tz = Snorm8(tangent.z);
            packed_vertex.ts = static_cast<std::int8_t>(v.t[3] < 0 ? -127 : 127);
        } else {
            packed_vertex.tx = packed_vertex.ty = packed_vertex.tz = 0;
            packed_vertex.ts = static_cast<std::int8_t>(127);
        }
        const std::uint8_t white[4] = {255, 255, 255, 255};
        const std::uint8_t* rgba = colors ? v.rgba : white;
        packed_vertex.r = rgba[0], packed_vertex.g = rgba[1], packed_vertex.b = rgba[2], packed_vertex.a = rgba[3];
        Store(out, l.vertices + i * kVertexSize, packed_vertex);
    }

    if (skinned) {
        for (std::size_t i = 0; i < vertex_count; ++i) {
            const std::uint64_t at = l.skins + i * kSkinSize;
            Store(out, at, QuantizeSkin(data.vertices[i], i, at, data.bones.size()));
        }
    }

    if (!data.indices.empty()) {
        std::memcpy(out.data() + l.triangles, data.indices.data(), data.indices.size() * sizeof(std::uint32_t));
    }
    for (std::size_t k = 0; k < lod_ends.size(); ++k) {
        Store(out, l.lods + k * sizeof(std::uint32_t), lod_ends[k]);
    }

    for (std::size_t b = 0; b < data.bones.size(); ++b) {
        const Bone& bone = data.bones[b];
        AEBone packed_bone{};
        packed_bone.name_offset = names[b].offset;
        packed_bone.name_length = names[b].length;
        packed_bone.parent = bone.parent;
        packed_bone.lod_parent = bone.lod_parent;
        packed_bone.cull_radius = bone.cull_radius;
        packed_bone.m00 = bone.m[0][0], packed_bone.m01 = bone.m[0][1], packed_bone.m02 = bone.m[0][2];
        packed_bone.m10 = bone.m[1][0], packed_bone.m11 = bone.m[1][1], packed_bone.m12 = bone.m[1][2];
        packed_bone.m20 = bone.m[2][0], packed_bone.m21 = bone.m[2][1], packed_bone.m22 = bone.m[2][2];
        packed_bone.tx = bone.t[0], packed_bone.ty = bone.t[1], packed_bone.tz = bone.t[2];
        Store(out, l.bones + b * kBoneSize, packed_bone);
    }

    if (!blob.empty()) {
        std::memcpy(out.data() + l.names, blob.data(), blob.size());
    }

    for (std::size_t s = 0; s < data.subsets.size(); ++s) {
        const Subset& subset = data.subsets[s];
        AESubset packed_subset{};
        packed_subset.tri_begin = subset.tri_begin;
        packed_subset.tri_count = subset.tri_count;
        packed_subset.vert_begin = subset.vert_begin;
        packed_subset.vert_count = subset.vert_count;
        packed_subset.bone_count = static_cast<std::uint32_t>(subset.bones.size());
        std::fill(std::begin(packed_subset.bones), std::end(packed_subset.bones), kNoBone);
        std::copy(subset.bones.begin(), subset.bones.end(), packed_subset.bones);
        Store(out, l.subsets + s * kSubsetSize, packed_subset);
    }

    Store(out, l.crc, crc32(ByteSpan(out).first(static_cast<std::size_t>(l.crc))));

    // The rules that survive encoding (indices, parents, subset ranges) are
    // checked on the finished bytes, so write() never makes a file read() rejects.
    Validate(out, ReadOptions{false, true});
    return out;
}

void compute_aabb(Data& data) {
    for (int axis = 0; axis < 3; ++axis) {
        data.bbox_min[axis] = 0;
        data.bbox_max[axis] = 0;
    }
    for (std::size_t i = 0; i < data.vertices.size(); ++i) {
        const float* p = data.vertices[i].p;
        for (int axis = 0; axis < 3; ++axis) {
            data.bbox_min[axis] = i == 0 ? p[axis] : std::min(data.bbox_min[axis], p[axis]);
            data.bbox_max[axis] = i == 0 ? p[axis] : std::max(data.bbox_max[axis], p[axis]);
        }
    }
}

void compute_normals(Data& data) {
    std::vector<Vec3> sums(data.vertices.size());
    for (std::size_t t = 0; t + 2 < data.indices.size(); t += 3) {
        const std::uint32_t a = data.indices[t], b = data.indices[t + 1], c = data.indices[t + 2];
        if (a >= sums.size() || b >= sums.size() || c >= sums.size()) {
            continue;
        }
        // The unnormalized cross product weights each face by its area. CCW faces point out.
        const Vec3 face = Cross(Sub(data.vertices[b].p, data.vertices[a].p), Sub(data.vertices[c].p, data.vertices[a].p));
        for (const std::uint32_t corner : {a, b, c}) {
            sums[corner] = {sums[corner].x + face.x, sums[corner].y + face.y, sums[corner].z + face.z};
        }
    }
    for (std::size_t i = 0; i < sums.size(); ++i) {
        Vec3 n = sums[i];
        // A vertex no face gives a direction to points up.
        if (!Normalize(n)) {
            n = {0, 1, 0};
        }
        data.vertices[i].n[0] = n.x, data.vertices[i].n[1] = n.y, data.vertices[i].n[2] = n.z;
    }
}

void compute_tangents(Data& data) {
    const std::size_t count = data.vertices.size();
    std::vector<Vec3> along_u(count);
    std::vector<Vec3> along_v(count);
    for (std::size_t t = 0; t + 2 < data.indices.size(); t += 3) {
        const std::uint32_t a = data.indices[t], b = data.indices[t + 1], c = data.indices[t + 2];
        if (a >= count || b >= count || c >= count) {
            continue;
        }
        const Vertex& va = data.vertices[a];
        const Vertex& vb = data.vertices[b];
        const Vertex& vc = data.vertices[c];
        const Vec3 e1 = Sub(vb.p, va.p);
        const Vec3 e2 = Sub(vc.p, va.p);
        const float du1 = vb.uv[0] - va.uv[0], dv1 = vb.uv[1] - va.uv[1];
        const float du2 = vc.uv[0] - va.uv[0], dv2 = vc.uv[1] - va.uv[1];
        const float det = du1 * dv2 - du2 * dv1;
        if (!(std::fabs(det) > 1e-20f)) {
            continue;
        }
        const float r = 1.0f / det;
        const Vec3 u{(e1.x * dv2 - e2.x * dv1) * r, (e1.y * dv2 - e2.y * dv1) * r, (e1.z * dv2 - e2.z * dv1) * r};
        const Vec3 v{(e2.x * du1 - e1.x * du2) * r, (e2.y * du1 - e1.y * du2) * r, (e2.z * du1 - e1.z * du2) * r};
        for (const std::uint32_t corner : {a, b, c}) {
            along_u[corner] = {along_u[corner].x + u.x, along_u[corner].y + u.y, along_u[corner].z + u.z};
            along_v[corner] = {along_v[corner].x + v.x, along_v[corner].y + v.y, along_v[corner].z + v.z};
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        Vertex& vertex = data.vertices[i];
        Vec3 n{vertex.n[0], vertex.n[1], vertex.n[2]};
        if (!Normalize(n)) {
            n = {0, 1, 0};
        }
        // Gram-Schmidt: the U direction with its normal part removed.
        const Vec3 u = along_u[i];
        const float along_n = Dot(n, u);
        Vec3 t{u.x - n.x * along_n, u.y - n.y * along_n, u.z - n.z * along_n};
        if (!Normalize(t)) {
            // No usable UVs here, so any direction across the normal will do.
            t = Cross(n, std::fabs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
            Normalize(t);
        }
        vertex.t[0] = t.x, vertex.t[1] = t.y, vertex.t[2] = t.z;
        vertex.t[3] = Dot(Cross(n, t), along_v[i]) < 0 ? -1.0f : 1.0f;
    }
    data.flags |= FLAG_TANGENTS;
}

bool amesh_self_test() {
    // The zlib check value.
    const char digits[] = "123456789";
    if (crc32(ByteSpan(reinterpret_cast<const std::byte*>(digits), 9)) != 0xCBF43926u) {
        return false;
    }

    try {
        // 1) A unit triangle round-trips, and its trailing CRC is the CRC of the rest.
        Data triangle;
        const float corners[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        for (const auto& corner : corners) {
            Vertex v;
            std::copy(corner, corner + 3, v.p);
            v.n[2] = 1;
            triangle.vertices.push_back(v);
        }
        triangle.indices = {0, 1, 2};
        const std::vector<std::byte> triangle_bytes = write(triangle);
        const ByteSpan all(triangle_bytes);
        const auto stored = Load<std::uint32_t>(all, all.size() - kCrcSize);
        if (stored != crc32(all.first(all.size() - kCrcSize))) {
            return false;
        }
        const Data triangle_back = read(all);
        if (triangle_back.vertices.size() != 3 || triangle_back.indices != triangle.indices ||
            triangle_back.lods.size() != 1 || triangle_back.lods[0].tri_count != 1) {
            return false;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            if (std::memcmp(triangle_back.vertices[i].p, triangle.vertices[i].p, sizeof(float) * 3) != 0) {
                return false;
            }
        }

        // 2) Two triangles on two bones keep their bones and weights.
        Data skinned;
        const float quad[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
        for (int i = 0; i < 4; ++i) {
            Vertex v;
            std::copy(quad[i], quad[i] + 3, v.p);
            v.n[2] = 1;
            v.bone[0] = 0;
            v.bone[1] = 1;
            v.weight[0] = i < 2 ? 0.75f : 0.25f;
            v.weight[1] = i < 2 ? 0.25f : 0.75f;
            skinned.vertices.push_back(v);
        }
        skinned.indices = {0, 1, 2, 0, 2, 3};
        Bone root;
        root.name = "root";
        Bone tip;
        tip.name = "tip";
        tip.parent = 0;
        tip.t[1] = 1;
        skinned.bones = {root, tip};
        const Data skinned_back = read(write(skinned));
        if ((skinned_back.flags & FLAG_SKINNED) == 0 || skinned_back.bones.size() != 2 ||
            skinned_back.bones[0].name != "root" || skinned_back.bones[1].name != "tip" ||
            skinned_back.bones[1].parent != 0 || skinned_back.indices != skinned.indices) {
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            const Vertex& v = skinned_back.vertices[i];
            // Sorted largest first: the heavier bone comes back in slot 0.
            const std::uint16_t heavy = i < 2 ? 0 : 1;
            if (v.bone[0] != heavy || v.bone[1] != 1 - heavy || v.bone[2] != kNoBone || v.bone[3] != kNoBone) {
                return false;
            }
            if (std::fabs(v.weight[0] - 0.75f) > 1.0f / 255 || std::fabs(v.weight[1] - 0.25f) > 1.0f / 255 ||
                std::fabs(v.weight[0] + v.weight[1] - 1.0f) > 1e-6f) {
                return false;
            }
        }
    } catch (const AEMeshError&) {
        return false;
    }
    return true;
}

}  // namespace anarchy::amesh
