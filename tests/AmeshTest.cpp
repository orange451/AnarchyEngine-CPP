#include "amesh.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// AMESH without a GL context: round trips, what the writer encodes, and every
// kind of file the reader turns away, with the byte it points at. GpuMesh is
// built with AE_MESH_NO_GL here, so its calls must do nothing.
namespace {

using namespace anarchy::amesh;

int gFailures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        ++gFailures;
    }
}

bool Near(float a, float b, float tolerance = 1e-6f) {
    return std::fabs(a - b) <= tolerance;
}

template <typename T>
T Peek(const std::vector<std::byte>& bytes, std::size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void Poke(std::vector<std::byte>& bytes, std::size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

// Rewrites the trailing CRC after a test edits the bytes.
void Recrc(std::vector<std::byte>& bytes) {
    const std::size_t body = bytes.size() - kCrcSize;
    Poke(bytes, body, crc32(ByteSpan(bytes).first(body)));
}

// Expects read() to throw at byte offset `at`.
void ExpectRejected(const std::vector<std::byte>& bytes, std::uint64_t at, const std::string& what,
                    ReadOptions options = {}) {
    try {
        read(bytes, options);
        Expect(false, what + ": read() accepted it");
    } catch (const AEMeshError& error) {
        Expect(error.byte_offset == at, what + ": rejected at byte " + std::to_string(error.byte_offset) +
                                            ", expected " + std::to_string(at) + " (" + error.reason + ")");
    }
}

void ExpectWriteRejected(const Data& data, const std::string& what) {
    try {
        write(data);
        Expect(false, what + ": write() accepted it");
    } catch (const AEMeshError&) {
    }
}

Vertex At(float x, float y, float z, float u = 0, float v = 0) {
    Vertex vertex;
    vertex.p[0] = x, vertex.p[1] = y, vertex.p[2] = z;
    vertex.n[2] = 1;
    vertex.uv[0] = u, vertex.uv[1] = v;
    return vertex;
}

// A unit quad facing +Z, two triangles, UVs over [0,1].
Data Quad() {
    Data data;
    data.vertices = {At(0, 0, 0, 0, 0), At(1, 0, 0, 1, 0), At(1, 1, 0, 1, 1), At(0, 1, 0, 0, 1)};
    data.indices = {0, 1, 2, 0, 2, 3};
    return data;
}

// The quad on two bones: the bottom follows the root, the top the tip.
Data SkinnedQuad() {
    Data data = Quad();
    for (int i = 0; i < 4; ++i) {
        data.vertices[i].bone[0] = 0;
        data.vertices[i].bone[1] = 1;
        data.vertices[i].weight[0] = i < 2 ? 1.0f : 0.25f;
        data.vertices[i].weight[1] = i < 2 ? 0.0f : 0.75f;
    }
    Bone root;
    root.name = "root";
    Bone tip;
    tip.name = "tip";
    tip.parent = 0;
    data.bones = {root, tip};
    return data;
}

// A tetrahedron of side s at (x, 0, 0), as a piece.
ConvexPiece Tetra(float x, float s = 1) {
    ConvexPiece piece;
    piece.points = {{x, 0, 0}, {x + s, 0, 0}, {x, s, 0}, {x, 0, s}};
    return piece;
}

// The quad with three pieces of recipe 7.
Data PiecedQuad() {
    Data data = Quad();
    data.piece_recipe = 7;
    data.pieces = {Tetra(0), Tetra(2), Tetra(4, 2)};
    data.pieces[2].points.push_back({5, 1, 1});
    return data;
}

// Where the skins start in a file of four vertices.
constexpr std::size_t kQuadSkins = kHeaderSize + 4 * kVertexSize;

void TestSelfTest() {
    Expect(amesh_self_test(), "amesh_self_test() passes");
}

void TestHeaderOffsets() {
    static_assert(offsetof(AEHeader, flags) == 10);
    static_assert(offsetof(AEHeader, vertex_count) == 12);
    static_assert(offsetof(AEHeader, name_blob_size) == 24);
    static_assert(offsetof(AEHeader, lod_generator) == 30);
    static_assert(offsetof(AEHeader, high_quality_lods) == 31);
    static_assert(offsetof(AEHeader, bbox_min) == 32);
    static_assert(offsetof(AEHeader, bbox_max) == 44);
    static_assert(offsetof(AEHeader, piece_count) == 56);
    static_assert(offsetof(AEHeader, piece_point_total) == 60);
    static_assert(offsetof(AEVertex, tx) == 32);
    static_assert(offsetof(AEVertex, r) == 36);
    static_assert(offsetof(AEBone, cull_radius) == 12);
    static_assert(offsetof(AEBone, tx) == 52);
    static_assert(offsetof(AESubset, bones) == 20);
}

void TestEmptyMesh() {
    const std::vector<std::byte> bytes = write(Data{});
    Expect(bytes.size() == kHeaderSize + kCrcSize, "an empty mesh is a header and a CRC");
    const Data back = read(bytes);
    Expect(back.vertices.empty() && back.indices.empty(), "an empty mesh reads back empty");
    Expect(back.lods.size() == 1 && back.lods[0].tri_begin == 0 && back.lods[0].tri_count == 0,
           "an empty mesh still has one LOD");
    Expect(write(back) == bytes, "an empty mesh rewrites to the same bytes");
}

void TestStaticMeshLayout() {
    const std::vector<std::byte> bytes = write(Quad());
    Expect(bytes.size() == kHeaderSize + 4 * kVertexSize + 2 * kTriangleSize + kCrcSize,
           "a static mesh has no skins, bones, LODs, or subsets");
    const auto header = Peek<AEHeader>(bytes, 0);
    Expect(std::memcmp(header.magic, "AESH", 4) == 0 && header.version_major == 1 && header.version_minor == 0 &&
               header.header_size == 64,
           "the header says AESH 1.0, 64 bytes");
    Expect(header.flags == FLAG_UNORM_UV, "a white quad with UVs in [0,1] sets only FLAG_UNORM_UV");
    Expect(header.bbox_min[0] == 0 && header.bbox_max[0] == 1 && header.bbox_max[1] == 1 && header.bbox_max[2] == 0,
           "the writer computes the bounding box");
    const auto vertex = Peek<AEVertex>(bytes, kHeaderSize);
    Expect(vertex.tx == 0 && vertex.ty == 0 && vertex.tz == 0 && vertex.ts == 127,
           "without tangents the writer stores 0,0,0,127");
    Expect(vertex.r == 255 && vertex.a == 255, "without colors the writer stores white");

    const Data back = read(bytes);
    for (const Vertex& v : back.vertices) {
        Expect(v.bone[0] == kNoBone && v.weight[0] == 0, "a static vertex has no influences");
    }
}

void TestFullRoundTrip() {
    Data data = SkinnedQuad();
    // Duplicate the quad as a second LOD, and cover each LOD with a subset.
    data.vertices.insert(data.vertices.end(), data.vertices.begin(), data.vertices.end());
    data.indices.insert(data.indices.end(), {4, 5, 6, 4, 6, 7});
    data.lods = {{0, 2}, {2, 2}};
    data.lod_generator = kLodGeneratorMeshoptimizer;
    data.high_quality_lods = true;
    Subset first;
    first.tri_begin = 0, first.tri_count = 2, first.vert_begin = 0, first.vert_count = 4;
    first.bones = {0, 1};
    Subset second;
    second.tri_begin = 2, second.tri_count = 2, second.vert_begin = 4, second.vert_count = 4;
    second.bones = {1};
    data.subsets = {first, second};
    Bone twin;
    twin.name = "tip";  // shares "tip"'s bytes in the blob
    twin.parent = 1;
    twin.lod_parent = 1;
    twin.cull_radius = 2.5f;
    twin.t[2] = -3;
    Bone unnamed;
    unnamed.parent = 2;
    data.bones.push_back(twin);
    data.bones.push_back(unnamed);
    data.vertices[1].rgba[0] = 10;
    compute_normals(data);
    compute_tangents(data);

    const std::vector<std::byte> bytes = write(data);
    const auto header = Peek<AEHeader>(bytes, 0);
    Expect(header.flags == (FLAG_SKINNED | FLAG_LODS | FLAG_SUBSETS | FLAG_VERTEX_COLOR | FLAG_TANGENTS | FLAG_UNORM_UV),
           "every flag is set from the contents");
    Expect(header.name_blob_size == 7, "\"root\" and \"tip\" are packed once each (" +
                                           std::to_string(header.name_blob_size) + " bytes)");
    Expect(header.lod_generator == kLodGeneratorMeshoptimizer && header.high_quality_lods == 1,
           "the LOD provenance is written");

    const Data back = read(bytes);
    Expect(back.flags == header.flags, "read() keeps the header's flags");
    Expect(back.lods.size() == 2 && back.lods[1].tri_begin == 2 && back.lods[1].tri_count == 2,
           "the LOD ranges come back");
    Expect(back.subsets.size() == 2 && back.subsets[0].bones == first.bones && back.subsets[1].vert_begin == 4,
           "the subsets come back");
    Expect(back.bones.size() == 4 && back.bones[2].name == "tip" && back.bones[3].name.empty(),
           "shared and empty names come back");
    Expect(back.bones[2].lod_parent == 1 && back.bones[2].cull_radius == 2.5f && back.bones[2].t[2] == -3,
           "bone fields come back");
    Expect(back.vertices[1].rgba[0] == 10, "vertex colors come back");
    Expect(Near(back.vertices[0].t[0], 1) && back.vertices[0].t[3] == 1, "tangents come back");
    Expect(write(back) == bytes, "a read mesh rewrites to the same bytes");
}

void TestWeights() {
    Data data = SkinnedQuad();
    data.vertices[0].weight[0] = 1.0f / 3, data.vertices[0].weight[1] = 1.0f / 3;
    data.vertices[0].bone[2] = 1;  // duplicate bone
    ExpectWriteRejected(data, "one bone twice in a vertex");

    data = SkinnedQuad();
    Bone third;
    third.parent = 0;
    data.bones.push_back(third);
    data.vertices[0].bone[2] = 2;
    data.vertices[0].weight[0] = data.vertices[0].weight[1] = data.vertices[0].weight[2] = 1.0f;
    // A weight that rounds to zero is dropped, and the rest still sum to 255.
    data.vertices[1].weight[0] = 1.0f, data.vertices[1].weight[1] = 0.001f;
    // Unnormalized weights are normalized before they are quantized.
    data.vertices[2].weight[0] = 2.0f, data.vertices[2].weight[1] = 6.0f;
    const std::vector<std::byte> bytes = write(data);
    const auto even = Peek<AESkin>(bytes, kQuadSkins);
    Expect(even.weight[0] + even.weight[1] + even.weight[2] == 255 && even.weight[3] == 0 &&
               even.bone_index[3] == kNoBone,
           "three equal weights quantize to 255 in all");
    const auto dropped = Peek<AESkin>(bytes, kQuadSkins + kSkinSize);
    Expect(dropped.bone_index[0] == 0 && dropped.weight[0] == 255 && dropped.bone_index[1] == kNoBone &&
               dropped.weight[1] == 0,
           "a weight too small for a byte leaves an unused slot");
    const auto sorted = Peek<AESkin>(bytes, kQuadSkins + 2 * kSkinSize);
    Expect(sorted.bone_index[0] == 1 && sorted.weight[0] == 191 && sorted.bone_index[1] == 0 && sorted.weight[1] == 64,
           "weights are sorted largest first and scaled to 255");

    // A file whose weights sum to 200 is renormalized on read.
    std::vector<std::byte> uneven = bytes;
    AESkin skin = Peek<AESkin>(uneven, kQuadSkins + 3 * kSkinSize);
    skin.bone_index[0] = 0, skin.weight[0] = 100;
    skin.bone_index[1] = 1, skin.weight[1] = 100;
    skin.bone_index[2] = skin.bone_index[3] = kNoBone;
    skin.weight[2] = skin.weight[3] = 0;
    Poke(uneven, kQuadSkins + 3 * kSkinSize, skin);
    Recrc(uneven);
    const Data back = read(uneven);
    Expect(Near(back.vertices[3].weight[0], 0.5f) && Near(back.vertices[3].weight[1], 0.5f),
           "weights summing to 200 read back as halves");

    // All-zero weights stay static.
    data = SkinnedQuad();
    for (float& w : data.vertices[0].weight) {
        w = 0;
    }
    const Data still = read(write(data));
    Expect(still.vertices[0].bone[0] == kNoBone && still.vertices[0].weight[0] == 0,
           "a vertex without weight stays in bind pose");
}

void TestComputeHelpers() {
    Data data = Quad();
    for (Vertex& v : data.vertices) {
        v.n[2] = 0;
    }
    compute_normals(data);
    Expect(Near(data.vertices[0].n[2], 1) && Near(data.vertices[2].n[2], 1), "CCW faces get +Z normals");

    compute_tangents(data);
    Expect(Near(data.vertices[0].t[0], 1) && Near(data.vertices[0].t[1], 0) && data.vertices[0].t[3] == 1,
           "U along +X gives tangent +X, and V along +Y gives sign +1");
    Expect((data.flags & FLAG_TANGENTS) != 0, "compute_tangents sets FLAG_TANGENTS");

    Data mirrored = Quad();
    for (Vertex& v : mirrored.vertices) {
        v.uv[1] = 1 - v.uv[1];
    }
    compute_tangents(mirrored);
    Expect(mirrored.vertices[0].t[3] == -1, "V running down -Y flips the bitangent sign");

    Data box = Quad();
    box.vertices[2].p[2] = -4;
    compute_aabb(box);
    Expect(box.bbox_min[2] == -4 && box.bbox_max[0] == 1 && box.bbox_max[2] == 0, "compute_aabb spans the positions");
    Data empty;
    empty.bbox_min[0] = 5;
    compute_aabb(empty);
    Expect(empty.bbox_min[0] == 0 && empty.bbox_max[0] == 0, "an empty mesh's box is zeros");
}

void TestWriteOptions() {
    Data data = Quad();
    data.vertices[0].rgba[1] = 0;
    compute_tangents(data);
    WriteOptions options;
    options.write_colors = false;
    options.write_tangents = false;
    options.write_uv_unorm_flag = false;
    const std::vector<std::byte> bytes = write(data, options);
    Expect(Peek<AEHeader>(bytes, 0).flags == 0, "the options clear the optional flags");
    const Data back = read(bytes);
    Expect(back.vertices[0].rgba[1] == 255, "colors are written white without write_colors");
    Expect(back.vertices[0].t[0] == 0 && back.vertices[0].t[3] == 1, "tangents are dropped without write_tangents");

    Data wide = Quad();
    wide.vertices[2].uv[0] = 2;
    Expect((Peek<AEHeader>(write(wide), 0).flags & FLAG_UNORM_UV) == 0, "UVs past 1 leave FLAG_UNORM_UV clear");
}

void TestWriterRejects() {
    Data data = Quad();
    data.indices.push_back(0);
    ExpectWriteRejected(data, "an index count that is not a multiple of 3");

    data = Quad();
    data.indices[4] = 9;
    ExpectWriteRejected(data, "an index past the vertices");

    data = Quad();
    data.lods = {{0, 1}};
    ExpectWriteRejected(data, "one LOD that leaves out a triangle");
    data.lods = {{0, 1}, {1, 0}, {1, 1}};
    ExpectWriteRejected(data, "an empty LOD");

    data = SkinnedQuad();
    data.bones[0].name = std::string("a\0b", 3);
    ExpectWriteRejected(data, "a bone name with NUL");
    data = SkinnedQuad();
    data.bones[0].name = "\xC0\x80";
    ExpectWriteRejected(data, "a bone name that is not UTF-8");

    data = SkinnedQuad();
    data.bones[0].parent = 1;
    ExpectWriteRejected(data, "bones whose parents form a cycle");
    data = SkinnedQuad();
    data.bones[1].parent = 1;
    ExpectWriteRejected(data, "a bone that is its own parent");

    data = SkinnedQuad();
    data.vertices[0].bone[1] = 7;
    ExpectWriteRejected(data, "a weight on a bone past the skeleton");
    data = SkinnedQuad();
    data.vertices[0].bone[2] = kNoBone;
    data.vertices[0].weight[2] = 0.5f;
    ExpectWriteRejected(data, "weight on an unused slot");

    data = SkinnedQuad();
    Subset wide;
    wide.tri_count = 2;
    wide.vert_count = 4;
    wide.bones.assign(27, 0);
    data.subsets = {wide};
    ExpectWriteRejected(data, "a subset with 27 bones");

    data = Quad();
    Subset boned;
    boned.tri_count = 2;
    boned.vert_count = 4;
    boned.bones = {0};
    data.subsets = {boned};
    ExpectWriteRejected(data, "a subset with bones on a mesh with no skeleton");

    data = Quad();
    Subset narrow;
    narrow.tri_count = 2;
    narrow.vert_count = 3;
    data.subsets = {narrow};
    ExpectWriteRejected(data, "a subset whose triangles leave its vertex range");

    data = Quad();
    data.vertices[0].p[1] = std::nanf("");
    ExpectWriteRejected(data, "a NaN position");
}

void TestReaderRejects() {
    const std::vector<std::byte> good = write(SkinnedQuad());
    const std::size_t triangles = kHeaderSize + 4 * (kVertexSize + kSkinSize);
    const std::size_t bones = triangles + 2 * kTriangleSize;

    ExpectRejected(std::vector<std::byte>(good.begin(), good.begin() + 63), 63, "a 63-byte file");

    std::vector<std::byte> bytes = good;
    bytes[0] = std::byte{'X'};
    ExpectRejected(bytes, 0, "a wrong magic");

    bytes = good;
    Poke<std::uint16_t>(bytes, offsetof(AEHeader, version_minor), 2);
    ExpectRejected(bytes, offsetof(AEHeader, version_major), "version 1.2");

    bytes = good;
    Poke<std::uint16_t>(bytes, offsetof(AEHeader, header_size), 80);
    ExpectRejected(bytes, offsetof(AEHeader, header_size), "header_size 80");

    bytes = good;
    Poke<std::uint16_t>(bytes, offsetof(AEHeader, flags), static_cast<std::uint16_t>(FLAG_SKINNED | (1u << 7)));
    ExpectRejected(bytes, offsetof(AEHeader, flags), "an unknown flag bit");

    bytes = good;
    Poke<std::uint32_t>(bytes, offsetof(AEHeader, piece_point_total), 1);
    ExpectRejected(bytes, offsetof(AEHeader, piece_count), "piece_point_total without FLAG_HULLS");

    bytes = good;
    Poke<std::uint16_t>(bytes, offsetof(AEHeader, flags), 0);
    ExpectRejected(bytes, offsetof(AEHeader, bone_count), "bones without FLAG_SKINNED");

    bytes = good;
    Poke<std::uint16_t>(bytes, offsetof(AEHeader, lod_count), 1);
    ExpectRejected(bytes, offsetof(AEHeader, lod_count), "lod_count 1 without FLAG_LODS");

    bytes = good;
    Poke<std::uint32_t>(bytes, offsetof(AEHeader, vertex_count), kMaxVertices + 1);
    ExpectRejected(bytes, offsetof(AEHeader, vertex_count), "too many vertices");

    bytes = good;
    bytes.push_back(std::byte{0});
    ExpectRejected(bytes, good.size(), "a trailing byte");

    // A flipped bit in a UV fails the CRC, and reads when the CRC is not checked.
    bytes = good;
    bytes[kHeaderSize + offsetof(AEVertex, u)] ^= std::byte{1};
    ExpectRejected(bytes, good.size() - kCrcSize, "a flipped bit");
    ReadOptions trusting;
    trusting.verify_crc = false;
    try {
        read(bytes, trusting);
    } catch (const AEMeshError& error) {
        Expect(false, std::string("verify_crc = false still checked the CRC: ") + error.what());
    }

    bytes = good;
    Poke<std::uint32_t>(bytes, triangles + 4, 4);
    Recrc(bytes);
    ExpectRejected(bytes, triangles + 4, "a triangle naming vertex 4 of 4");

    // Parents 0 -> 1 -> 0: a strict read rejects the cycle; a lenient one reads it.
    bytes = good;
    Poke<std::uint16_t>(bytes, bones + offsetof(AEBone, parent), 1);
    Recrc(bytes);
    ExpectRejected(bytes, bones + offsetof(AEBone, parent), "a parent cycle");
    ReadOptions lenient;
    lenient.strict_parents = false;
    try {
        read(bytes, lenient);
    } catch (const AEMeshError& error) {
        Expect(false, std::string("strict_parents = false still rejected a cycle: ") + error.what());
    }

    bytes = good;
    Poke<std::uint16_t>(bytes, bones + kBoneSize + offsetof(AEBone, parent), 1);
    Recrc(bytes);
    ExpectRejected(bytes, bones + kBoneSize + offsetof(AEBone, parent), "a bone that is its own parent", lenient);

    bytes = good;
    Poke<std::uint16_t>(bytes, bones + offsetof(AEBone, parent), 2);
    Recrc(bytes);
    ExpectRejected(bytes, bones + offsetof(AEBone, parent), "a parent past the skeleton", lenient);

    bytes = good;
    Poke<std::uint32_t>(bytes, bones + offsetof(AEBone, name_length), 1000);
    Recrc(bytes);
    ExpectRejected(bytes, bones, "a name past the blob");

    bytes = good;
    Poke<std::uint16_t>(bytes, kHeaderSize + 4 * kVertexSize + 2 * 3, 1);
    Recrc(bytes);
    ExpectRejected(bytes, kHeaderSize + 4 * kVertexSize + 2 * 3, "a bone paired with weight 0");

    bytes = good;
    Poke<std::uint8_t>(bytes, kHeaderSize + offsetof(AEVertex, g), 0);
    Recrc(bytes);
    ExpectRejected(bytes, kHeaderSize + offsetof(AEVertex, r), "a colored vertex without FLAG_VERTEX_COLOR");

    bytes = good;
    Poke<float>(bytes, kHeaderSize + offsetof(AEVertex, px), 9.0f);
    Recrc(bytes);
    ExpectRejected(bytes, kHeaderSize, "a vertex outside the bounding box");

    // LOD ends that do not increase.
    Data lodded = Quad();
    lodded.lods = {{0, 1}, {1, 1}};
    bytes = write(lodded);
    const std::size_t lods = kHeaderSize + 4 * kVertexSize + 2 * kTriangleSize;
    Expect(Peek<std::uint32_t>(bytes, lods) == 1 && Peek<std::uint32_t>(bytes, lods + 4) == 2,
           "lod_face_end holds exclusive ends");
    Poke<std::uint32_t>(bytes, lods, 2);
    Recrc(bytes);
    ExpectRejected(bytes, lods + 4, "lod_face_end that does not increase");
}

void TestPiecesRoundTrip() {
    const Data data = PiecedQuad();
    const std::vector<std::byte> bytes = write(data);
    const auto header = Peek<AEHeader>(bytes, 0);
    Expect(header.version_major == 1 && header.version_minor == 1, "a file with pieces is written as 1.1");
    Expect((header.flags & FLAG_HULLS) != 0, "pieces set FLAG_HULLS");
    Expect(header.piece_count == 3 && header.piece_point_total == 13, "the header counts the pieces and their points");
    Expect(bytes.size() == kHeaderSize + 4 * kVertexSize + 2 * kTriangleSize + 4 + 3 * 4 + 13 * 12 + kCrcSize,
           "the section is a recipe, a count per piece, and the points");
    const Data back = read(bytes);
    Expect(back.piece_recipe == 7, "the recipe round-trips");
    Expect(back.pieces.size() == 3 && back.pieces[2].points.size() == 5, "the pieces round-trip");
    Expect(back.pieces[1].points[1] == std::array<float, 3>{3, 0, 0}, "a piece's points round-trip");
    Expect(write(back) == bytes, "a read file rewrites to the same bytes");
}

void TestVersionOneZeroReads() {
    const std::vector<std::byte> bytes = write(Quad());
    const Data back = read(bytes);
    Expect(back.vertices.size() == 4 && back.pieces.empty(), "a 1.0 file reads, with no pieces");
}

void TestPiecesRejected() {
    const std::vector<std::byte> good = write(PiecedQuad());
    const std::size_t section = kHeaderSize + 4 * kVertexSize + 2 * kTriangleSize;
    const std::size_t points = section + 4 + 3 * 4;

    std::vector<std::byte> bytes = good;
    Poke<std::uint16_t>(bytes, offsetof(AEHeader, version_minor), 0);
    Recrc(bytes);
    ExpectRejected(bytes, offsetof(AEHeader, flags), "a 1.0 file with FLAG_HULLS");

    bytes = good;
    Poke<std::uint32_t>(bytes, offsetof(AEHeader, piece_count), kMaxPieces + 1);
    ExpectRejected(bytes, offsetof(AEHeader, piece_count), "piece_count over 256");

    bytes = write(Quad());
    Poke<std::uint32_t>(bytes, offsetof(AEHeader, piece_count), 1);
    Recrc(bytes);
    ExpectRejected(bytes, offsetof(AEHeader, piece_count), "piece_count without FLAG_HULLS");

    bytes = good;
    Poke<std::uint32_t>(bytes, section + 4, 3);
    Poke<std::uint32_t>(bytes, section + 8, 5);
    Recrc(bytes);
    ExpectRejected(bytes, section + 4, "a piece of 3 points");

    bytes = good;
    Poke<std::uint32_t>(bytes, section + 4, 5);
    Recrc(bytes);
    ExpectRejected(bytes, section + 4, "piece counts that disagree with piece_point_total");

    bytes = good;
    Poke<float>(bytes, points, std::nanf(""));
    Recrc(bytes);
    ExpectRejected(bytes, points, "a point that is not finite");

    Data data = PiecedQuad();
    data.pieces[0].points.resize(kMaxPiecePoints + 1, {0, 0, 0});
    ExpectWriteRejected(data, "a piece of 129 points");
    data = PiecedQuad();
    data.pieces[0].points.resize(3);
    ExpectWriteRejected(data, "a piece of 3 points");
    data = PiecedQuad();
    data.pieces[1].points[0][1] = INFINITY;
    ExpectWriteRejected(data, "a piece point that is not finite");
}

void TestNoPiecesSameSize() {
    const std::vector<std::byte> bytes = write(Quad());
    const auto header = Peek<AEHeader>(bytes, 0);
    Expect(bytes.size() == kHeaderSize + 4 * kVertexSize + 2 * kTriangleSize + kCrcSize,
           "a mesh without pieces is as large as in 1.0");
    Expect(header.piece_count == 0 && header.piece_point_total == 0 && (header.flags & FLAG_HULLS) == 0,
           "a mesh without pieces has both piece words 0 and no FLAG_HULLS");
    Expect(header.version_major == 1 && header.version_minor == 0, "a file without pieces is written as 1.0");
    Data stripped = read(write(PiecedQuad()));
    stripped.pieces.clear();
    Expect(Peek<AEHeader>(write(stripped), 0).version_minor == 0, "a file that loses its pieces goes back to 1.0");
}

void TestGpuMeshWithoutGl() {
    GpuMesh mesh;
    mesh.upload(SkinnedQuad());
    Expect(!mesh.valid(), "with AE_MESH_NO_GL an upload creates nothing");
    mesh.bind();
    mesh.draw(3);
    mesh.draw_subset(3);
    mesh.destroy();

    // The bounds of what was uploaded, kept for culling, with or without GL.
    Data box;
    for (const float x : {-1.f, 2.f}) {
        Vertex v;
        v.p[0] = x;
        v.p[1] = x * 3.f;
        v.p[2] = 0.5f;
        box.vertices.push_back(v);
    }
    box.vertices.push_back(box.vertices[0]);
    box.indices = {0, 1, 2};
    GpuMesh bounded;
    bounded.upload(box);
    Expect(bounded.bounds_min()[0] == -1.f && bounded.bounds_min()[1] == -3.f && bounded.bounds_min()[2] == 0.5f,
           "GpuMesh keeps its box's low corner");
    Expect(bounded.bounds_max()[0] == 2.f && bounded.bounds_max()[1] == 6.f && bounded.bounds_max()[2] == 0.5f,
           "and its high corner");

    // Each upload is a generation of its own, so a cache keyed on the mesh's
    // address still sees geometry uploaded again in place.
    GpuMesh fresh;
    Expect(fresh.generation() == 0, "a mesh never uploaded is generation 0");
    GpuMesh again;
    again.upload(box);
    const std::uint64_t first = again.generation();
    again.upload(box);
    Expect(first != 0 && again.generation() != first, "uploading again in place moves the generation on");
    Expect(bounded.generation() != first && bounded.generation() != again.generation(),
           "two meshes never share a generation");
}

}  // namespace

int main() {
    const std::vector<std::function<void()>> tests = {
        TestSelfTest,         TestHeaderOffsets,     TestEmptyMesh,         TestStaticMeshLayout,
        TestFullRoundTrip,    TestWeights,           TestComputeHelpers,    TestWriteOptions,
        TestWriterRejects,    TestReaderRejects,     TestPiecesRoundTrip,   TestVersionOneZeroReads,
        TestPiecesRejected,   TestNoPiecesSameSize,  TestGpuMeshWithoutGl,
    };
    for (const auto& test : tests) {
        try {
            test();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL uncaught: %s\n", error.what());
            ++gFailures;
        }
    }
    if (gFailures != 0) {
        std::fprintf(stderr, "%d AMESH check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("AMESH checks passed\n");
    return 0;
}
