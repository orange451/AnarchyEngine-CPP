#pragma once

// AMESH is the baked internal mesh asset.
// Datamodel Mesh is a separate authoring instance
// (AddTeapot / AddCylinder / etc.) and will be converted
// to AMESH later. This module does not implement that instance.
//
// File: magic "AESH", version 1.0. Little-endian, tightly packed, no padding,
// no compression. Triangles only, CCW front faces. Right-handed, Y-up,
// X-right, Z-forward (the camera looks down -Z). Engine world units.
//
// Layout, from offset 0. Arrays behind a flag are left out when it is clear.
//     AEHeader    header                          64 bytes
//     AEVertex    vertices[vertex_count]          40 bytes each
//     AESkin      skins[vertex_count]             12 bytes each, FLAG_SKINNED
//     AETriangle  triangles[triangle_count]       12 bytes each
//     u32         lod_face_end[lod_count]          4 bytes each, FLAG_LODS
//     AEBone      bones[bone_count]               64 bytes each, FLAG_SKINNED
//     u8          name_blob[name_blob_size]
//     AESubset    subsets[subset_count]           72 bytes each, FLAG_SUBSETS
//     u32         crc32 of bytes [0, size - 4)    zlib CRC-32
//
// A static mesh is a skinned mesh with no bones. Skins hold global bone
// indices; subsets are draw and cluster hints and never remap them.
//
// GpuMesh vertex attributes, interleaved, 76 bytes per vertex:
//     location 0  position  3 x float
//     location 1  normal    3 x float
//     location 2  uv        2 x float
//     location 3  tangent   4 x float        xyz, then the bitangent sign
//     location 4  color     4 x u8           normalized
//     location 5  bone      4 x u16          integer (glVertexAttribIPointer)
//     location 6  weight    4 x float
// Build with AE_MESH_NO_GL and GpuMesh does nothing, so tools need no GL context.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <version>
#if __has_include(<span>)
#include <span>
#endif

// Records are copied in host byte order. Windows only runs little-endian.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "AMESH needs a little-endian host."
#endif
#elif !defined(_WIN32)
#error "AMESH cannot tell this host's byte order, and needs a little-endian host."
#endif

namespace anarchy::amesh {

// ByteSpan under C++20. The engine still builds with a
// Visual Studio 2019 whose library has no <span>, so there it is a view with
// the same members read() uses. Build every file that includes this header
// with the same standard, or the two will disagree about read()'s signature.
#if defined(__cpp_lib_span)
using ByteSpan = ByteSpan;
#else
class ByteSpan {
public:
    constexpr ByteSpan() = default;
    constexpr ByteSpan(const std::byte* data, std::size_t size) : data_(data), size_(size) {}
    ByteSpan(const std::vector<std::byte>& bytes) : data_(bytes.data()), size_(bytes.size()) {}

    constexpr const std::byte* data() const { return data_; }
    constexpr std::size_t size() const { return size_; }
    constexpr bool empty() const { return size_ == 0; }
    constexpr const std::byte* begin() const { return data_; }
    constexpr const std::byte* end() const { return data_ + size_; }
    constexpr const std::byte& operator[](std::size_t i) const { return data_[i]; }
    constexpr ByteSpan first(std::size_t count) const { return {data_, count}; }

private:
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};
#endif

inline constexpr char kMagic[4] = {'A', 'E', 'S', 'H'};
inline constexpr std::uint16_t kVersionMajor = 1;
inline constexpr std::uint16_t kVersionMinor = 0;

inline constexpr std::size_t kHeaderSize = 64;
inline constexpr std::size_t kVertexSize = 40;
inline constexpr std::size_t kSkinSize = 12;
inline constexpr std::size_t kTriangleSize = 12;
inline constexpr std::size_t kBoneSize = 64;
inline constexpr std::size_t kSubsetSize = 72;
inline constexpr std::size_t kCrcSize = 4;

inline constexpr std::uint16_t FLAG_SKINNED = 1u << 0;
inline constexpr std::uint16_t FLAG_LODS = 1u << 1;
inline constexpr std::uint16_t FLAG_SUBSETS = 1u << 2;
inline constexpr std::uint16_t FLAG_VERTEX_COLOR = 1u << 3;
inline constexpr std::uint16_t FLAG_TANGENTS = 1u << 4;
inline constexpr std::uint16_t FLAG_UNORM_UV = 1u << 5;
inline constexpr std::uint16_t kKnownFlags =
    FLAG_SKINNED | FLAG_LODS | FLAG_SUBSETS | FLAG_VERTEX_COLOR | FLAG_TANGENTS | FLAG_UNORM_UV;

inline constexpr std::uint32_t kMaxVertices = 2'000'000;
inline constexpr std::uint32_t kMaxTriangles = 4'000'000;
inline constexpr std::uint32_t kMaxBones = 4096;
inline constexpr std::uint32_t kMaxSubsets = 4096;
inline constexpr std::uint32_t kMaxLods = 16;
inline constexpr std::uint32_t kMaxNameBlob = 1'048'576;
inline constexpr std::uint64_t kMaxFileSize = 512ull * 1024 * 1024;
inline constexpr std::uint32_t kMaxInfluences = 4;
inline constexpr std::uint32_t kMaxSubsetBones = 26;

inline constexpr std::uint16_t kNoBone = 0xFFFF;
inline constexpr std::uint32_t kNoName = 0xFFFFFFFF;

// lod_generator values.
inline constexpr std::uint8_t kLodGeneratorNone = 0;
inline constexpr std::uint8_t kLodGeneratorUnknown = 1;
inline constexpr std::uint8_t kLodGeneratorGrid = 2;
inline constexpr std::uint8_t kLodGeneratorMeshoptimizer = 3;

#pragma pack(push, 1)

struct AEHeader {
    char magic[4];               // 'A','E','S','H'
    std::uint16_t version_major; // 1
    std::uint16_t version_minor; // 0
    std::uint16_t header_size;   // 64
    std::uint16_t flags;
    std::uint32_t vertex_count;
    std::uint32_t triangle_count;
    std::uint16_t lod_count;      // 0 if !FLAG_LODS else >= 2
    std::uint16_t bone_count;     // 0 if !FLAG_SKINNED else >= 1
    std::uint32_t name_blob_size;
    std::uint16_t subset_count;
    std::uint8_t lod_generator;   // 0 none, 1 unknown, 2 grid, 3 meshoptimizer
    std::uint8_t high_quality_lods;
    float bbox_min[3];
    float bbox_max[3];
    std::uint32_t reserved1;      // 0
    std::uint32_t reserved2;      // 0
};
static_assert(sizeof(AEHeader) == kHeaderSize);

struct AEVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    std::int8_t tx, ty, tz;
    std::int8_t ts;
    std::uint8_t r, g, b, a;
};
static_assert(sizeof(AEVertex) == kVertexSize);

struct AESkin {
    std::uint16_t bone_index[4]; // 0xFFFF unused
    std::uint8_t weight[4];
};
static_assert(sizeof(AESkin) == kSkinSize);

struct AETriangle {
    std::uint32_t i0, i1, i2;
};
static_assert(sizeof(AETriangle) == kTriangleSize);

struct AEBone {
    std::uint32_t name_offset;   // 0xFFFFFFFF unnamed
    std::uint32_t name_length;
    std::uint16_t parent;        // 0xFFFF none
    std::uint16_t lod_parent;
    float cull_radius;
    float m00, m01, m02;
    float m10, m11, m12;
    float m20, m21, m22;
    float tx, ty, tz;
};
static_assert(sizeof(AEBone) == kBoneSize);

struct AESubset {
    std::uint32_t tri_begin, tri_count;
    std::uint32_t vert_begin, vert_count;
    std::uint32_t bone_count;
    std::uint16_t bones[26];
};
static_assert(sizeof(AESubset) == kSubsetSize);

#pragma pack(pop)

// A malformed file, or Data that cannot be written. byte_offset is where in
// the file (or the file write() was building) the problem is.
class AEMeshError : public std::runtime_error {
public:
    AEMeshError(std::uint64_t byte_offset, std::string reason);

    std::uint64_t byte_offset;
    std::string reason;
};

// The CPU bake buffer. Not the datamodel Mesh.
struct Vertex {
    float p[3] = {0, 0, 0};
    float n[3] = {0, 0, 0};
    float uv[2] = {0, 0};
    float t[4] = {0, 0, 0, 1};  // t[3] = bitangent sign +/-1. A zero xyz means no tangent.
    std::uint8_t rgba[4] = {255, 255, 255, 255};
    std::uint16_t bone[4] = {kNoBone, kNoBone, kNoBone, kNoBone};
    float weight[4] = {0, 0, 0, 0};  // 0..1, renormalized after read
};

struct Bone {
    std::string name;
    std::uint16_t parent = kNoBone;
    std::uint16_t lod_parent = kNoBone;
    float cull_radius = 0;
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};  // row-major bind rotation
    float t[3] = {0, 0, 0};
};

struct Subset {
    std::uint32_t tri_begin = 0, tri_count = 0, vert_begin = 0, vert_count = 0;
    std::vector<std::uint16_t> bones;
};

struct LodRange {
    std::uint32_t tri_begin = 0, tri_count = 0;
};

struct Data {
    std::uint16_t flags = 0;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;  // 3 * triangle_count
    std::vector<LodRange> lods;          // at least one after read()
    std::vector<Bone> bones;
    std::vector<Subset> subsets;
    float bbox_min[3] = {0, 0, 0};
    float bbox_max[3] = {0, 0, 0};
    // The header's LOD provenance, kept so a read and write round-trips it.
    std::uint8_t lod_generator = kLodGeneratorNone;
    bool high_quality_lods = false;
};

struct ReadOptions {
    bool verify_crc = true;
    bool strict_parents = true;
};

struct WriteOptions {
    bool write_tangents = true;
    bool write_colors = true;
    bool write_uv_unorm_flag = true;
};

// Throws AEMeshError.
Data read(ByteSpan bytes, ReadOptions options = {});
// Throws AEMeshError when data breaks a rule a reader would reject.
std::vector<std::byte> write(const Data& data, WriteOptions options = {});

void compute_aabb(Data& data);
void compute_normals(Data& data);
void compute_tangents(Data& data);

// zlib CRC-32 (poly 0xEDB88320, init and final XOR 0xFFFFFFFF).
std::uint32_t crc32(ByteSpan bytes);

bool amesh_self_test();

// The interleaved vertex GpuMesh uploads. Not an on-disk record.
struct GpuVertex {
    float position[3];
    float normal[3];
    float uv[2];
    float tangent[4];
    std::uint8_t color[4];
    std::uint16_t bone[4];
    float weight[4];
};
static_assert(sizeof(GpuVertex) == 76);
static_assert(offsetof(GpuVertex, color) == 48);
static_assert(offsetof(GpuVertex, bone) == 52);
static_assert(offsetof(GpuVertex, weight) == 60);

inline constexpr unsigned kAttribPosition = 0;
inline constexpr unsigned kAttribNormal = 1;
inline constexpr unsigned kAttribUv = 2;
inline constexpr unsigned kAttribTangent = 3;
inline constexpr unsigned kAttribColor = 4;
inline constexpr unsigned kAttribBone = 5;
inline constexpr unsigned kAttribWeight = 6;

// GPU buffers for one AMESH: a VAO, an interleaved VBO and a u32 EBO. Every
// call but valid() needs the GL context the mesh was uploaded in to be current.
class GpuMesh {
public:
    GpuMesh() = default;
    ~GpuMesh();
    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;
    GpuMesh(GpuMesh&& other) noexcept;
    GpuMesh& operator=(GpuMesh&& other) noexcept;

    // Creates the buffers, or replaces their contents if they exist. Throws
    // std::invalid_argument for an index, LOD or subset outside the mesh.
    void upload(const Data& data, bool dynamic = false);
    void bind() const;
    // Draw with the mesh bound; before an upload they do nothing. Once uploaded,
    // both throw std::out_of_range for a missing LOD or subset.
    void draw(int lod = 0) const;
    void draw_subset(std::size_t subset) const;
    void destroy();
    // Drops the GL names without deleting them, for a context that is already
    // gone (its objects went with it). Needs no GL. valid() is false after.
    void forget();
    bool valid() const;

    std::size_t lod_count() const { return lods_.size(); }
    std::size_t subset_count() const { return subsets_.size(); }
    // The local box around the last upload's vertices, for culling. Zeros before one. Needs no GL.
    const float* bounds_min() const { return bounds_min_; }
    const float* bounds_max() const { return bounds_max_; }

private:
    void keep_bounds(const Data& data);
    void draw_range(std::uint32_t tri_begin, std::uint32_t tri_count) const;

    unsigned vao_ = 0;
    unsigned vbo_ = 0;
    unsigned ebo_ = 0;
    std::vector<LodRange> lods_;
    std::vector<LodRange> subsets_;
    float bounds_min_[3] = {0.f, 0.f, 0.f};
    float bounds_max_[3] = {0.f, 0.f, 0.f};
};

}  // namespace anarchy::amesh
