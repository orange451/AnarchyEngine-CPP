#include "AssetInstances.hpp"
#include "Camera.hpp"
#include "Game.hpp"
#include "SceneService.hpp"
#include "SnapshotPump.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "TerrainTextures.hpp"
#include "TerrainWorld.hpp"
#include "amesh.hpp"
#include "ide/MaterialBall.hpp"
#include "runner/MeshCache.hpp"
#include "runner/RenderMath.hpp"
#include "DraggerMath.hpp"
#include "runner/Renderer.hpp"
#include "runner/TerrainDraws.hpp"
#include "runner/SkyMath.hpp"
#include "runner/TextureCache.hpp"
#include "profiler/Profiler.hpp"
#include "runner/gl.hpp"
#include "runner/ViewCapture.hpp"
#include "terrain/LodNode.hpp"
#include "terrain/SurfaceNets.hpp"
#include "terrain/VoxelVolume.hpp"

// Only GLFW's window calls: the GL names come from runner/gl.hpp.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

// By hand, not ctest (it needs a GL 3.3 context), from the repository root so
// resources/shaders is found: scene-render-check draws what the Scene View
// draws for a GameObject with a Prefab, lit through the legacy pipeline. A cube baked to AMESH in a scratch
// resources folder goes through MeshCache and Renderer, with the fixed
// camera, and the check reads the pixels back. With --save dir or --compare dir
// it also writes, or checks against, the regression scenes' frames. Terrain
// (Surface Nets chunks through the terrain program) is drawn too: always a red
// ball in the window, through the Scene View's own AppendTerrainDraws. With
// --terrain-shots dir, also island, hills, cliff and chunk-seam scenes at 1280
// by 720 offscreen under a DynamicSky, and scenes taken through the snapshot
// path (a Game's Terrain, a TerrainWorld, a SnapshotPump), each written to dir
// as a PNG and its path printed; and terrain LOD on a large island (lod-*.png):
// a grazing and a far shot, a 30-frame descent, and a shot from 1300 units
// off (terrain past 1000 units), each frame checked
// against full detail for cracks, geometry outside the true silhouette and
// wrong materials (TerrainLodShots), plus, with --terrain-lod-colors, the
// still shots tinted by level. They take some seconds (about 15 in Release),
// so a plain run skips them. The flags combine.
namespace {

using namespace anarchy::amesh;

int gFailures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        ++gFailures;
    }
}

// A unit cube around the origin: 6 faces of 2 CCW triangles, normals out.
Data Cube() {
    Data data;
    const float faces[6][4][3] = {
        {{0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}},        // +X
        {{-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}},    // -X
        {{-0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}},        // +Y
        {{-0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}},    // -Y
        {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}},        // +Z
        {{0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}},    // -Z
    };
    for (const auto& face : faces) {
        const auto base = static_cast<std::uint32_t>(data.vertices.size());
        // Corner order runs bottom left, bottom right, top right, top left on the +Z face.
        const float uvs[4][2] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
        for (int corner = 0; corner < 4; ++corner) {
            Vertex v;
            std::copy(face[corner], face[corner] + 3, v.p);
            v.uv[0] = uvs[corner][0];
            v.uv[1] = uvs[corner][1];
            data.vertices.push_back(v);
        }
        data.indices.insert(data.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    compute_normals(data);
    return data;
}

struct Pixel {
    int r = 0, g = 0, b = 0;
};

Pixel ReadPixel(int x, int y) {
    unsigned char rgba[4] = {};
    glReadPixels(x, y, 1, 1, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, rgba);
    return {rgba[0], rgba[1], rgba[2]};
}

bool IsClear(Pixel p) {
    return p.r == 30 && p.g == 30 && p.b == 30;
}

std::string Text(Pixel p) {
    return std::to_string(p.r) + "," + std::to_string(p.g) + "," + std::to_string(p.b);
}

int Sum(Pixel p) { return p.r + p.g + p.b; }

// The legacy pipeline's stand-in sky lights every surface a faint gray, so a
// channel the surface's color has none of still reads a little above 0.
constexpr int kSkyTint = 4;

// An uncompressed 24-bit TGA, 4 by 4, written top row first: the top two rows
// red, the bottom two blue.
std::string StripesTga() {
    std::string bytes(18, '\0');
    bytes[2] = 2;     // true color, uncompressed
    bytes[12] = 4;    // width
    bytes[14] = 4;    // height
    bytes[16] = 24;   // bits per pixel
    bytes[17] = 0x20; // top row first
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            // Blue, green, red.
            bytes += row < 2 ? std::string("\x00\x00\xff", 3) : std::string("\xff\x00\x00", 3);
        }
    }
    return bytes;
}

// A Radiance HDR image, written top row first, uncompressed: color(x, y)
// gives each pixel's linear RGB, which may be brighter than 1.
template <typename Color>
std::string Hdr(int width, int height, Color color) {
    std::string bytes = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(height) + " +X " +
                        std::to_string(width) + "\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::array<float, 3> rgb = color(x, y);
            const float brightest = std::max({rgb[0], rgb[1], rgb[2]});
            if (brightest < 1e-32f) {
                bytes.append(4, '\0');
                continue;
            }
            int exponent = 0;
            const float scale = std::frexp(brightest, &exponent) * 256.f / brightest;
            for (const float channel : rgb) {
                bytes += static_cast<char>(static_cast<unsigned char>(channel * scale));
            }
            bytes += static_cast<char>(exponent + 128);
        }
    }
    return bytes;
}

// The test sky, 16 by 8: above the horizon red (4, brighter than white) on
// the image's left half and green on its right; below it, blue. The image's
// middle is straight down -Z, so a camera looking down -Z sees red on its
// left, green on its right, and blue below.
std::string TestSky() {
    return Hdr(16, 8, [](int x, int y) -> std::array<float, 3> {
        if (y >= 4) {
            return {0.f, 0.f, 1.f};
        }
        return x < 8 ? std::array<float, 3>{4.f, 0.f, 0.f} : std::array<float, 3>{0.f, 4.f, 0.f};
    });
}

// The regression scenes: frames that must not change, beyond 1 per channel,
// when the renderer draws the same meshes another way. 25 cubes in a 5 by 5
// grid, turned and sized differently, one mirrored in X and one stretched in
// Y, colored in turn, on a floor; then under a shadowing sun; then with a
// shadowing spot and three see-through cubes in front.
std::vector<runner::MeshDraw> RegressionDraws(const GpuMesh* cube, bool seeThrough) {
    std::vector<runner::MeshDraw> draws;
    for (int i = 0; i < 25; ++i) {
        const float x = static_cast<float>(i % 5) * 1.6f - 3.2f;
        const float z = 1.f - static_cast<float>(i / 5) * 1.6f;
        engine_core::Matrix4 model = engine_core::matrix4_multiply(
            engine_core::matrix4_translation(x, 0.f, z), engine_core::matrix4_axis_angle({0.f, 1.f, 0.f}, 0.3 * i));
        const float scale = 0.5f + 0.05f * static_cast<float>(i % 7);
        for (int column = 0; column < 3; ++column) {
            for (int axis = 0; axis < 3; ++axis) {
                model.m[column * 4 + axis] *= scale;
            }
        }
        if (i == 7) {
            for (int axis = 0; axis < 3; ++axis) {
                model.m[axis] *= -1.f;
            }
        }
        if (i == 12) {
            for (int axis = 0; axis < 3; ++axis) {
                model.m[4 + axis] *= 2.f;
            }
        }
        runner::MeshDraw draw{cube, model};
        draw.color[0] = i % 3 == 0 ? 1.f : 0.3f;
        draw.color[1] = i % 3 == 1 ? 1.f : 0.3f;
        draw.color[2] = i % 3 == 2 ? 1.f : 0.3f;
        draws.push_back(draw);
    }
    engine_core::Matrix4 floor = engine_core::matrix4_translation(0.f, -0.6f, -2.f);
    floor.m[0] = 12.f;
    floor.m[5] = 0.2f;
    floor.m[10] = 12.f;
    draws.push_back(runner::MeshDraw{cube, floor});
    if (seeThrough) {
        for (int i = 0; i < 3; ++i) {
            runner::MeshDraw glass{cube, engine_core::matrix4_translation(static_cast<float>(i) - 1.f, 0.5f, 3.f)};
            glass.transparency = 0.4f;
            glass.color[i] = 1.f;
            draws.push_back(glass);
        }
    }
    return draws;
}

// The three regression frames, each width * height * 4 bytes, read from the window.
std::vector<std::vector<unsigned char>> RegressionFrames(const GpuMesh* cube, int size, int width, int height) {
    runner::Renderer renderer;
    Expect(renderer.initialize(), "the renderer builds for the regression scenes");
    runner::LightDraw sun;
    sun.kind = runner::LightDraw::Kind::Directional;
    sun.direction[0] = 0.5f;
    sun.direction[1] = -0.8f;
    sun.direction[2] = -0.3f;
    sun.intensity = 2.f;
    sun.shadows = true;
    sun.shadowDistance = 100.f;
    sun.id = 21;
    runner::LightDraw spot;
    spot.kind = runner::LightDraw::Kind::Spot;
    spot.position[0] = -4.f;
    spot.position[1] = 4.f;
    spot.position[2] = 2.f;
    spot.direction[0] = 0.6f;
    spot.direction[1] = -0.7f;
    spot.direction[2] = -0.4f;
    spot.outerFovDegrees = 90.f;
    spot.radius = 20.f;
    spot.intensity = 4.f;
    spot.shadows = true;
    spot.id = 22;
    std::vector<std::vector<unsigned char>> frames;
    const auto capture = [&](const std::vector<runner::MeshDraw>& draws, const runner::LightDraw* light) {
        // Twice, so cached shadow maps are what the frame reads, as in the studio.
        for (int pass = 0; pass < 2; ++pass) {
            renderer.draw(0, 0, size, size, size, size, draws.data(), static_cast<int>(draws.size()), light,
                          light != nullptr ? 1 : 0);
        }
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, pixels.data());
        frames.push_back(std::move(pixels));
    };
    capture(RegressionDraws(cube, false), nullptr);
    capture(RegressionDraws(cube, false), &sun);
    capture(RegressionDraws(cube, true), &spot);
    Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the regression scenes leave no GL error");
    renderer.shutdown();
    return frames;
}

// --save writes each frame to dir/regression-N.rgba; --compare reads them back
// and expects every channel within 1. Both modes validate that frames draw content.
void SaveOrCompareRegression(const std::vector<std::vector<unsigned char>>& frames, const std::string& mode,
                             const std::filesystem::path& dir) {
    for (std::size_t n = 0; n < frames.size(); ++n) {
        // Count pixels that are not the clear color (30, 30, 30).
        int drawn = 0;
        const std::size_t pixels = frames[n].size() / 4;
        for (std::size_t i = 0; i < frames[n].size(); i += 4) {
            const bool clear = frames[n][i] == 30 && frames[n][i + 1] == 30 && frames[n][i + 2] == 30;
            drawn += clear ? 0 : 1;
        }
        Expect(drawn >= static_cast<int>(pixels * 0.05),
               "regression frame " + std::to_string(n) + " draws something (" + std::to_string(drawn) + " of " +
                   std::to_string(pixels) + " pixels)");

        const std::filesystem::path file = dir / ("regression-" + std::to_string(n) + ".rgba");
        if (mode == "--save") {
            std::filesystem::create_directories(dir);
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(frames[n].data()), static_cast<std::streamsize>(frames[n].size()));
            Expect(out.good(), "regression frame " + std::to_string(n) + " saved");
            continue;
        }
        std::ifstream in(file, std::ios::binary);
        std::vector<unsigned char> saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (saved.size() != frames[n].size()) {
            Expect(false, "regression frame " + std::to_string(n) + " has the saved size");
            continue;
        }
        int worst = 0;
        int differing = 0;
        for (std::size_t i = 0; i < saved.size(); ++i) {
            const int difference = std::abs(static_cast<int>(saved[i]) - static_cast<int>(frames[n][i]));
            worst = std::max(worst, difference);
            differing += difference > 0 ? 1 : 0;
        }
        Expect(worst <= 1, "regression frame " + std::to_string(n) + " matches within 1 (worst " +
                               std::to_string(worst) + ", " + std::to_string(differing) + " channels differ)");
    }
}

// Terrain: Surface Nets chunk meshes drawn through the terrain program.

namespace terrain = engine_core::terrain;

// Every chunk of volume that has data, and its 26 neighbors: the chunks a
// surface may cross. As sandbox/terrain_surface_tests.cpp's mesh_all.
std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> ChunksAround(const terrain::VoxelVolume& volume) {
    std::unordered_set<terrain::ChunkCoord, terrain::ChunkCoordHash> coords;
    for (const auto& entry : volume.chunks()) {
        const terrain::ChunkCoord& coord = entry.first;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    coords.insert(terrain::ChunkCoord{coord.x + dx, coord.y + dy, coord.z + dz});
                }
            }
        }
    }
    return coords;
}

// Every chunk ChunksAround names, meshed, as TerrainWorld publishes them: a
// TerrainChunkView each, with revisions from next_revision on; empty chunks
// are left out.
std::shared_ptr<const std::vector<engine_core::TerrainChunkView>> MeshChunks(const terrain::VoxelVolume& volume,
                                                                            std::uint64_t next_revision) {
    auto chunks = std::make_shared<std::vector<engine_core::TerrainChunkView>>();
    for (const terrain::ChunkCoord& coord : ChunksAround(volume)) {
        terrain::ChunkMesh mesh = terrain::surface_nets(terrain::mesh_input(volume, coord));
        if (mesh.render != nullptr) {
            chunks->push_back(engine_core::TerrainChunkView{coord, next_revision++, std::move(mesh.render)});
        }
    }
    return chunks;
}

// chunks as level-0 LOD nodes, as TerrainWorld publishes them before any
// coarser level is built: each its own root, its bounds its chunk's box and
// its mesh's, sorted by key.
std::shared_ptr<const std::vector<engine_core::TerrainNodeView>> ChunkNodes(
    const std::vector<engine_core::TerrainChunkView>& chunks, float voxelSize) {
    auto nodes = std::make_shared<std::vector<engine_core::TerrainNodeView>>();
    for (const engine_core::TerrainChunkView& chunk : chunks) {
        engine_core::TerrainNodeView node;
        node.key = terrain::node_of(chunk.coord, 0);
        node.revision = chunk.revision;
        node.mesh = chunk.mesh;
        terrain::node_bounds(node.key, voxelSize, node.bounds_min, node.bounds_max);
        for (const anarchy::amesh::Vertex& vertex : chunk.mesh->vertices) {
            node.bounds_min = {std::min(node.bounds_min.x, vertex.p[0]),
                               std::min(node.bounds_min.y, vertex.p[1]),
                               std::min(node.bounds_min.z, vertex.p[2])};
            node.bounds_max = {std::max(node.bounds_max.x, vertex.p[0]),
                               std::max(node.bounds_max.y, vertex.p[1]),
                               std::max(node.bounds_max.z, vertex.p[2])};
        }
        nodes->push_back(node);
    }
    std::sort(nodes->begin(), nodes->end(), [](const engine_core::TerrainNodeView& a, const engine_core::TerrainNodeView& b) {
        return std::tie(a.key.level, a.key.x, a.key.y, a.key.z) < std::tie(b.key.level, b.key.x, b.key.y, b.key.z);
    });
    return nodes;
}

// The camera renderer draws from, for selecting terrain nodes on a pane size by size.
runner::TerrainCamera RendererCamera(const runner::Renderer& renderer, int width, int height) {
    runner::TerrainCamera camera;
    camera.world = engine_core::matrix4_inverse(renderer.view());
    camera.fov_y_degrees = renderer.fovYDegrees();
    camera.pane_width = width;
    camera.pane_height = height;
    return camera;
}

// A published look from look-table texels (LookBytes' or TexturedLookFloats'), with revision.
std::shared_ptr<const engine_core::TerrainLook> MakeLook(const std::vector<float>& texels, std::uint64_t revision) {
    auto look = std::make_shared<engine_core::TerrainLook>();
    std::copy(texels.begin(), texels.end(), look->texels.begin());
    look->revision = revision;
    return look;
}

// Every chunk ChunksAround names, meshed and uploaded; empty chunks are left out.
std::vector<std::unique_ptr<GpuMesh>> UploadTerrain(const terrain::VoxelVolume& volume) {
    std::vector<std::unique_ptr<GpuMesh>> out;
    for (const terrain::ChunkCoord& coord : ChunksAround(volume)) {
        const terrain::ChunkMesh mesh = terrain::surface_nets(terrain::mesh_input(volume, coord));
        if (mesh.render == nullptr) {
            continue;
        }
        auto gpu = std::make_unique<GpuMesh>();
        gpu->upload(*mesh.render, false);
        out.push_back(std::move(gpu));
    }
    return out;
}

// Like UploadTerrain, but keeps each chunk's own ChunkCoord alongside its
// mesh (TX-R9 needs each draw's world position by hand, to give it a
// synthetic terrainLevel by distance from a chosen eye -- no LodTree or
// TerrainWorld involved, just UploadTerrain's own full-detail chunks).
std::vector<std::pair<terrain::ChunkCoord, std::unique_ptr<GpuMesh>>> UploadTerrainWithCoords(
    const terrain::VoxelVolume& volume) {
    std::vector<std::pair<terrain::ChunkCoord, std::unique_ptr<GpuMesh>>> out;
    for (const terrain::ChunkCoord& coord : ChunksAround(volume)) {
        const terrain::ChunkMesh mesh = terrain::surface_nets(terrain::mesh_input(volume, coord));
        if (mesh.render == nullptr) {
            continue;
        }
        auto gpu = std::make_unique<GpuMesh>();
        gpu->upload(*mesh.render, false);
        out.emplace_back(coord, std::move(gpu));
    }
    return out;
}

// One MeshDraw per chunk, at the Terrain's Transform model.
std::vector<runner::MeshDraw> TerrainDraws(const std::vector<std::unique_ptr<GpuMesh>>& chunks, unsigned look,
                                           const engine_core::Matrix4& model = engine_core::matrix4_identity()) {
    std::vector<runner::MeshDraw> draws;
    for (const auto& chunk : chunks) {
        runner::MeshDraw draw{chunk.get(), model};
        draw.terrainLook = look;
        draw.owner = 77;
        draws.push_back(draw);
    }
    return draws;
}

// A Terrain look table's floats (the TerrainLook layout: 256 x 4 RGBA32F, row
// 0 color sRGB 0-1, row 1 metalness/roughness/reflectivity, row 2 layer
// index/TextureScale/BlendSharpness/HeightStrength, row 3 reserved): every Id
// rough, plain and untextured (layer 0), colored by colors (Id, then sRGB
// 0-255) and a neutral gray for the rest. These checks draw flat (no array
// pair attached), so terrain.frag's uHasSurface 0 path reads only rows 0 and
// 1 -- TexturedLookFloats below is TX-R1..TX-R5's real, textured look.
std::vector<float> LookBytes(std::initializer_list<std::array<int, 4>> colors) {
    std::vector<float> texels(256 * 4 * 4, 0.f);
    for (int id = 0; id < 256; ++id) {
        float* color = texels.data() + id * 4;
        color[0] = color[1] = color[2] = 150.f / 255.f;
        color[3] = 1.f;
        float* surface = texels.data() + (256 + id) * 4;
        surface[0] = 0.f;            // metalness
        surface[1] = 220.f / 255.f;  // roughness
        surface[2] = 20.f / 255.f;   // reflectivity
        surface[3] = 1.f;
        float* layer = texels.data() + (512 + id) * 4;
        layer[0] = 0.f;   // layer index: 0, the untextured default
        layer[1] = 8.f;   // TextureScale
        layer[2] = 0.5f;  // BlendSharpness
        layer[3] = 1.f;   // HeightStrength
    }
    for (const std::array<int, 4>& entry : colors) {
        float* color = texels.data() + entry[0] * 4;
        for (int channel = 0; channel < 3; ++channel) {
            color[channel] = static_cast<float>(entry[channel + 1]) / 255.f;
        }
    }
    return texels;
}

// --- Task 6 (TX-R1..TX-R5): procedural texture layers, so --terrain-shots
// needs no downloaded textures. Each layer's color and height follow a
// simple periodic pattern (so it tiles without a seam), its normal stays
// flat, and roughness/metalness are fixed; a full box-filtered mip chain is
// built from level 0, exactly as LayerBuilder's own mips are (but without
// going through a source file on disk).

// One level's A (color + height) and B (normal + roughness + metalness)
// bytes, size by size, for ProceduralLayer's kind.
void FillProceduralLevel(int size, int kind, std::vector<std::uint8_t>& a, std::vector<std::uint8_t>& b) {
    a.resize(static_cast<std::size_t>(size) * size * 4);
    b.resize(static_cast<std::size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y) {
        const float fy = static_cast<float>(y) / static_cast<float>(size);
        for (int x = 0; x < size; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(size);
            // Three octaves of a sin*cos product, each a different
            // (integer, so the sum stays exactly periodic over one tile --
            // seamless under the array's GL_REPEAT) frequency pair, summed
            // with falling weight. A single sin*cos term has broad, nearly
            // flat plateaus near its extrema (its own two factors' peaks
            // overlapping) wide enough to fully cover one of TX-R3's coarse
            // triangles -- exactly the flat, pattern-less patch TX-R3 is
            // supposed to catch (a real one, not a shader bug: found by
          // reading back an actual render, not by inspection). Three
            // unrelated frequencies essentially never plateau together, and
            // the finest octave's own cycle is smaller than one triangle at
            // this scene's TextureScale, so there is always visible
            // variation inside any single triangle, however coarse the mesh.
            const auto octaves = [&](float f1u, float f1v, float f2u, float f2v, float f3u, float f3v, float seed) {
                const float o1 = std::sin(fx * 6.28318531f * f1u + seed) * std::cos(fy * 6.28318531f * f1v + seed * 1.6f);
                const float o2 = std::sin(fx * 6.28318531f * f2u + seed * 2.1f) * std::cos(fy * 6.28318531f * f2v + seed * 0.7f);
                const float o3 = std::sin(fx * 6.28318531f * f3u + seed * 3.3f) * std::cos(fy * 6.28318531f * f3v + seed * 1.1f);
                return std::clamp(0.5f + 0.5f * (0.45f * o1 + 0.3f * o2 + 0.25f * o3), 0.f, 1.f);
            };
            float r, g, bl, height, roughness, metalness;
            switch (kind) {
                case 0: {  // stone: gray blotches, cracked at a fine scale
                    const float n = octaves(3.f, 2.f, 7.f, 5.f, 17.f, 13.f, 0.f);
                    r = g = bl = 60.f + 150.f * n;
                    height = 0.2f + 0.6f * n;
                    roughness = 0.82f;
                    metalness = 0.02f;
                    break;
                }
                case 1: {  // grass: mottled bright/dark green tufts
                    const float n = octaves(5.f, 4.f, 11.f, 9.f, 23.f, 19.f, 1.f);
                    r = 25.f + 60.f * n;
                    g = 60.f + 145.f * n;
                    bl = 20.f + 50.f * n;
                    height = 0.25f + 0.55f * n;
                    roughness = 0.9f;
                    metalness = 0.0f;
                    break;
                }
                default: {  // sand: dune-like ripples at a fine scale
                    const float n = octaves(9.f, 2.f, 17.f, 15.f, 31.f, 29.f, 2.f);
                    r = 155.f + 85.f * n;
                    g = 130.f + 80.f * n;
                    bl = 85.f + 65.f * n;
                    height = 0.25f + 0.45f * n;
                    roughness = 0.75f;
                    metalness = 0.0f;
                    break;
                }
            }
            const std::size_t index = (static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)) * 4;
            a[index + 0] = static_cast<std::uint8_t>(std::clamp(r, 0.f, 255.f));
            a[index + 1] = static_cast<std::uint8_t>(std::clamp(g, 0.f, 255.f));
            a[index + 2] = static_cast<std::uint8_t>(std::clamp(bl, 0.f, 255.f));
            a[index + 3] = static_cast<std::uint8_t>(std::clamp(height, 0.f, 1.f) * 255.f);
            b[index + 0] = 128;  // flat tangent-space normal (x = 0)
            b[index + 1] = 128;  // (y = 0; z reconstructed in the shader)
            b[index + 2] = static_cast<std::uint8_t>(std::clamp(roughness, 0.f, 1.f) * 255.f);
            b[index + 3] = static_cast<std::uint8_t>(std::clamp(metalness, 0.f, 1.f) * 255.f);
        }
    }
}

// level's next (half-sized, rounded up) mip, box-filtering 2 x 2 (or the
// single remaining texel on an odd edge) RGBA8 blocks.
std::vector<std::uint8_t> DownsampleRgba8(const std::vector<std::uint8_t>& level, int size, int nextSize) {
    std::vector<std::uint8_t> next(static_cast<std::size_t>(nextSize) * nextSize * 4);
    for (int y = 0; y < nextSize; ++y) {
        for (int x = 0; x < nextSize; ++x) {
            const int x0 = std::min(x * 2, size - 1);
            const int x1 = std::min(x * 2 + 1, size - 1);
            const int y0 = std::min(y * 2, size - 1);
            const int y1 = std::min(y * 2 + 1, size - 1);
            const std::size_t out = (static_cast<std::size_t>(y) * nextSize + static_cast<std::size_t>(x)) * 4;
            for (int channel = 0; channel < 4; ++channel) {
                const int sum = level[(static_cast<std::size_t>(y0) * size + static_cast<std::size_t>(x0)) * 4 +
                                      static_cast<std::size_t>(channel)] +
                               level[(static_cast<std::size_t>(y0) * size + static_cast<std::size_t>(x1)) * 4 +
                                      static_cast<std::size_t>(channel)] +
                               level[(static_cast<std::size_t>(y1) * size + static_cast<std::size_t>(x0)) * 4 +
                                      static_cast<std::size_t>(channel)] +
                               level[(static_cast<std::size_t>(y1) * size + static_cast<std::size_t>(x1)) * 4 +
                                      static_cast<std::size_t>(channel)];
                next[out + static_cast<std::size_t>(channel)] = static_cast<std::uint8_t>(sum / 4);
            }
        }
    }
    return next;
}

// kind -1 is the untextured default layer (white, 0.5 height, flat normal,
// roughness 1, metalness 1), as layer 0 of every TerrainTextureSet.
engine_core::terrain::LayerBytes ProceduralLayer(int size, int kind) {
    engine_core::terrain::LayerBytes layer;
    layer.size = size;
    std::vector<std::uint8_t> a, b;
    if (kind < 0) {
        a.assign(static_cast<std::size_t>(size) * size * 4, 255);
        b.assign(static_cast<std::size_t>(size) * size * 4, 255);
        for (std::size_t i = 0; i < a.size(); i += 4) {
            a[i + 3] = 128;  // height 0.5
            b[i + 0] = 128;  // flat normal
            b[i + 1] = 128;
        }
    } else {
        FillProceduralLevel(size, kind, a, b);
    }
    int levelSize = size;
    layer.a_mips.push_back(a);
    layer.b_mips.push_back(b);
    while (levelSize > 1) {
        const int nextSize = std::max(levelSize / 2, 1);
        a = DownsampleRgba8(a, levelSize, nextSize);
        b = DownsampleRgba8(b, levelSize, nextSize);
        layer.a_mips.push_back(a);
        layer.b_mips.push_back(b);
        levelSize = nextSize;
    }
    return layer;
}

// A published TerrainTextureSet: layer 0 the untextured default, then one
// procedural layer per kind in kinds (stone 0, grass 1, sand 2), at size.
std::shared_ptr<const engine_core::TerrainTextureSet> MakeTextureSet(int size, std::initializer_list<int> kinds,
                                                                      std::uint64_t revision) {
    auto set = std::make_shared<engine_core::TerrainTextureSet>();
    set->size = size;
    set->revision = revision;
    set->layers.push_back(std::make_shared<const engine_core::terrain::LayerBytes>(ProceduralLayer(size, -1)));
    for (int kind : kinds) {
        set->layers.push_back(std::make_shared<const engine_core::terrain::LayerBytes>(ProceduralLayer(size, kind)));
    }
    return set;
}

// A textured look: entry is {Id, r, g, b (sRGB 0-255), layer index, scale}.
// Every other field keeps LookBytes' defaults (rough, metal 0, BlendSharpness
// 0.5, HeightStrength 1).
struct TexturedLookEntry {
    int id;
    int r, g, b;
    int layer;
    float scale;
};

std::vector<float> TexturedLookFloats(std::initializer_list<TexturedLookEntry> entries) {
    std::vector<float> texels = LookBytes({});
    for (const TexturedLookEntry& entry : entries) {
        float* color = texels.data() + entry.id * 4;
        color[0] = static_cast<float>(entry.r) / 255.f;
        color[1] = static_cast<float>(entry.g) / 255.f;
        color[2] = static_cast<float>(entry.b) / 255.f;
        float* layer = texels.data() + (512 + entry.id) * 4;
        layer[0] = static_cast<float>(entry.layer);
        layer[1] = entry.scale;
    }
    return texels;
}

// One MeshDraw per chunk, textured: the look table and the Terrain's array
// pair (built incrementally by renderer.terrainArrays, as AppendTerrainDraws
// does for a real Terrain). Call repeatedly (as --terrain-shots does, once
// per frame it draws) until layerCount settles so every layer has uploaded.
std::vector<runner::MeshDraw> TexturedTerrainDraws(const std::vector<std::unique_ptr<GpuMesh>>& chunks,
                                                    runner::Renderer& renderer, engine_core::InstanceId terrainId,
                                                    unsigned look,
                                                    const std::shared_ptr<const engine_core::TerrainTextureSet>& set,
                                                    const engine_core::Matrix4& model = engine_core::matrix4_identity()) {
    unsigned surfaceA = 0, surfaceB = 0;
    int layerCount = 0;
    renderer.terrainArrays(terrainId, set, surfaceA, surfaceB, layerCount);
    std::vector<runner::MeshDraw> draws;
    for (const auto& chunk : chunks) {
        runner::MeshDraw draw{chunk.get(), model};
        draw.terrainLook = look;
        draw.owner = terrainId;
        draw.terrainSurfaceA = surfaceA;
        draw.terrainSurfaceB = surfaceB;
        draw.terrainLayerCount = layerCount;
        draws.push_back(draw);
    }
    return draws;
}

terrain::Shape Ball(float x, float y, float z, float radius) {
    terrain::Shape shape;
    shape.kind = terrain::Shape::Kind::Ball;
    shape.center = engine_core::Vec3{x, y, z};
    shape.radius = radius;
    return shape;
}

terrain::Shape Block(float x, float y, float z, float sx, float sy, float sz) {
    terrain::Shape shape;
    shape.kind = terrain::Shape::Kind::Block;
    shape.frame = engine_core::matrix4_translation(x, y, z);
    shape.size = engine_core::Vec3{sx, sy, sz};
    return shape;
}

// A cylinder of radius about frame's Y, height long.
terrain::Shape Cylinder(const engine_core::Matrix4& frame, float radius, float height) {
    terrain::Shape shape;
    shape.kind = terrain::Shape::Kind::Cylinder;
    shape.frame = frame;
    shape.size = engine_core::Vec3{2.f * radius, height, 2.f * radius};
    return shape;
}

// Each edit fits VoxelVolume's limit, or the check says which did not.
void Edit(const std::optional<std::string>& refused, const std::string& what) {
    Expect(!refused.has_value(), what + " is within the edit limit" + (refused ? " (" + *refused + ")" : ""));
}

// A framebuffer of its own, width by height RGBA8, that the Renderer draws
// its pane into, so a shot can be larger than the hidden window.
class OffscreenTarget {
public:
    OffscreenTarget(int width, int height) : width_(width), height_(height) {
        glGenTextures(1, &color_);
        glBindTexture(runner::GL_TEXTURE_2D, color_);
        glTexImage2D(runner::GL_TEXTURE_2D, 0, static_cast<runner::GLint>(runner::GL_RGBA8), width, height, 0,
                     runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(runner::GL_TEXTURE_2D, runner::GL_TEXTURE_MIN_FILTER,
                        static_cast<runner::GLint>(runner::RT_GL_NEAREST));
        glTexParameteri(runner::GL_TEXTURE_2D, runner::GL_TEXTURE_MAG_FILTER,
                        static_cast<runner::GLint>(runner::RT_GL_NEAREST));
        glBindTexture(runner::GL_TEXTURE_2D, 0);
        glGenFramebuffers(1, &framebuffer_);
        glBindFramebuffer(runner::RT_GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(runner::RT_GL_FRAMEBUFFER, runner::RT_GL_COLOR_ATTACHMENT0, runner::GL_TEXTURE_2D,
                               color_, 0);
        complete_ = glCheckFramebufferStatus(runner::RT_GL_FRAMEBUFFER) == runner::RT_GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(runner::RT_GL_FRAMEBUFFER, 0);
    }
    ~OffscreenTarget() {
        glDeleteFramebuffers(1, &framebuffer_);
        glDeleteTextures(1, &color_);
    }
    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    bool complete() const { return complete_; }

    // Draws a few times (the DynamicSky's lighting cube and the shadow maps
    // settle over the first frames, as in the studio), then reads the pane
    // back top row first. The window's framebuffer and viewport are put back.
    bool shoot(runner::Renderer& renderer, const std::vector<runner::MeshDraw>& draws, const runner::LightDraw& sun,
               runner::ViewPixels& out) {
        return shoot(renderer, draws, &sun, 1, 4, out);
    }
    // The same with lights (lightCount of them, or none), drawn frames times.
    bool shoot(runner::Renderer& renderer, const std::vector<runner::MeshDraw>& draws, const runner::LightDraw* lights,
               int lightCount, int frames, runner::ViewPixels& out) {
        runner::GLint viewport[4] = {};
        glGetIntegerv(runner::GL_VIEWPORT, viewport);
        glBindFramebuffer(runner::RT_GL_FRAMEBUFFER, framebuffer_);
        glViewport(0, 0, width_, height_);
        const double w = width_;
        const double h = height_;
        bool drawn = false;
        for (int frame = 0; frame < frames; ++frame) {
            drawn = renderer.draw(0, 0, w, h, w, h, draws.empty() ? nullptr : draws.data(),
                                  static_cast<int>(draws.size()), lights, lightCount);
        }
        const bool read = drawn && renderer.read(0, 0, w, h, w, h, out);
        glBindFramebuffer(runner::RT_GL_FRAMEBUFFER, 0);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        return read;
    }

private:
    int width_;
    int height_;
    unsigned color_ = 0;
    unsigned framebuffer_ = 0;
    bool complete_ = false;
};

// One terrain shot: what the camera looks at, from where (degrees around Y
// from +X toward +Z, degrees up, studs away), and its vertical angle.
struct TerrainShot {
    std::string file;
    engine_core::Vec3 target;
    float azimuth = 0.f;
    float elevation = 0.f;
    float distance = 0.f;
    float fov = 50.f;
};

// The camera's Transform for shot.
engine_core::Matrix4 ShotCamera(const TerrainShot& shot) {
    constexpr float kDegrees = 3.14159265f / 180.f;
    const float around = shot.azimuth * kDegrees;
    const float up = shot.elevation * kDegrees;
    const engine_core::Vec3 eye{shot.target.x + shot.distance * std::cos(up) * std::cos(around),
                                shot.target.y + shot.distance * std::sin(up),
                                shot.target.z + shot.distance * std::cos(up) * std::sin(around)};
    return engine_core::matrix4_look_at(eye, shot.target, {0.f, 1.f, 0.f});
}

int PixelSum(const runner::ViewPixels& pixels, int x, int y) {
    const unsigned char* p = pixels.rgba.data() + (static_cast<std::size_t>(y) * pixels.width + x) * 4;
    return p[0] + p[1] + p[2];
}

// Whether shot shows terrain where the sky alone would be: its middle, and a
// good part of the frame, differ from sky's; and what differs is lit, not black.
void ExpectTerrainShown(const std::string& name, const runner::ViewPixels& shot, const runner::ViewPixels& sky) {
    if (shot.rgba.size() != sky.rgba.size() || shot.rgba.empty()) {
        Expect(false, name + " and its sky alone read back at the same size");
        return;
    }
    const int middle = std::abs(PixelSum(shot, shot.width / 2, shot.height / 2) -
                                PixelSum(sky, sky.width / 2, sky.height / 2));
    Expect(middle > 12, name + ": terrain covers the middle (differs from the sky by " + std::to_string(middle) + ")");
    std::int64_t covered = 0;
    std::int64_t brightness = 0;
    for (int y = 0; y < shot.height; ++y) {
        for (int x = 0; x < shot.width; ++x) {
            if (std::abs(PixelSum(shot, x, y) - PixelSum(sky, x, y)) > 12) {
                ++covered;
                brightness += PixelSum(shot, x, y);
            }
        }
    }
    const std::int64_t total = static_cast<std::int64_t>(shot.width) * shot.height;
    Expect(covered * 50 > total, name + ": terrain covers over 2% of the frame (" + std::to_string(covered) + " of " +
                                     std::to_string(total) + " pixels)");
    const std::int64_t mean = covered > 0 ? brightness / covered : 0;
    Expect(mean > 90, name + ": the terrain is lit, not black (mean channel sum " + std::to_string(mean) + ")");
}

// Terrain LOD seen whole (--terrain-shots): a large rolling island through
// TerrainWorld and AppendTerrainDraws, each frame checked against the
// island's full detail (every level-0 chunk mesh, no selection) from the
// same camera, in a flat pass: no sky, no lights, ambient only, no
// antialiasing, each material Id in a color of its own. A pixel the full
// detail covers that LOD leaves empty, with no empty full-detail pixel within
// kLodEdgeBand, is a crack; one LOD covers with no full-detail pixel within
// the band is outside the true silhouette (a fin); one both cover in
// materials that differ, the LOD's found nowhere within kLodMaterialBand in
// the full detail, is a wrong material.

constexpr int kLodEdgeBand = 2;
constexpr int kLodMaterialBand = 6;

// The island: 32 x 2 x 32 chunks (1,024 x 64 x 1,024 cells, a unit each),
// a round island of rolling hills about (512, 512), solid from y = 2 up to its height.
constexpr int kIslandCells = 1024;
constexpr int kIslandRows = 64;
constexpr float kIslandMiddle = 512.f;

float Smooth(float edge0, float edge1, float x) {
    const float t = std::min(std::max((x - edge0) / (edge1 - edge0), 0.f), 1.f);
    return t * t * (3.f - 2.f * t);
}

float IslandHeight(float x, float z) {
    const float dx = x - kIslandMiddle;
    const float dz = z - kIslandMiddle;
    const float r = std::sqrt(dx * dx + dz * dz);
    const float rolling = 20.f + 8.f * std::sin(x / 61.f) * std::cos(z / 47.f) + 6.f * std::sin((x + z) / 33.f) +
                          4.f * std::cos((x - 2.f * z) / 83.f) + 3.f * std::sin(z / 19.f + std::cos(x / 27.f));
    // A ridge across the middle, steep enough for rock.
    const float ridge = 14.f * std::max(0.f, 1.f - std::abs(dx * 0.8f + dz * 0.6f - 60.f) / 22.f) * Smooth(330.f, 120.f, r);
    const float hill = 12.f * Smooth(300.f, 0.f, r);
    return -2.f + Smooth(500.f, 380.f, r) * (rolling + hill + ridge + 2.f);
}

// Material Ids: 1 grass, 2 rock (steep), 3 sand (low), 4 snow (high).
std::uint8_t IslandMaterial(float height, float slope) {
    if (height < 7.f) {
        return 3;
    }
    if (slope > 0.45f) {
        return 2;
    }
    return height > 44.f ? 4 : 1;
}

// Writes the island into volume, a 256 x 64 x 256 block at a time (each within the edit limit).
void WriteIsland(terrain::VoxelVolume& volume) {
    std::vector<float> heights(static_cast<std::size_t>(kIslandCells + 2) * (kIslandCells + 2));
    const auto at = [&](int x, int z) -> float& {
        return heights[static_cast<std::size_t>(z + 1) * (kIslandCells + 2) + static_cast<std::size_t>(x + 1)];
    };
    for (int z = -1; z <= kIslandCells; ++z) {
        for (int x = -1; x <= kIslandCells; ++x) {
            at(x, z) = IslandHeight(static_cast<float>(x), static_cast<float>(z));
        }
    }
    constexpr int kBlock = 256;
    std::vector<float> distances;
    std::vector<std::uint8_t> materials;
    for (int bz = 0; bz < kIslandCells; bz += kBlock) {
        for (int bx = 0; bx < kIslandCells; bx += kBlock) {
            const std::size_t count = static_cast<std::size_t>(kBlock) * kIslandRows * kBlock;
            distances.assign(count, 0.f);
            materials.assign(count, 1);
            for (int z = 0; z < kBlock; ++z) {
                for (int x = 0; x < kBlock; ++x) {
                    const int cx = bx + x;
                    const int cz = bz + z;
                    const float h = at(cx, cz);
                    const float gx = 0.5f * (at(cx + 1, cz) - at(cx - 1, cz));
                    const float gz = 0.5f * (at(cx, cz + 1) - at(cx, cz - 1));
                    const float slope = std::sqrt(gx * gx + gz * gz);
                    // The distance to the surface, not just the height above it.
                    const float scale = 1.f / std::sqrt(1.f + slope * slope);
                    const std::uint8_t id = IslandMaterial(h, slope);
                    for (int y = 0; y < kIslandRows; ++y) {
                        const std::size_t i = static_cast<std::size_t>(x) +
                                              static_cast<std::size_t>(kBlock) *
                                                  (static_cast<std::size_t>(y) + kIslandRows * static_cast<std::size_t>(z));
                        const float top = (static_cast<float>(y) - h) * scale;
                        const float bottom = 2.f - static_cast<float>(y);
                        distances[i] = std::max(top, bottom);
                        materials[i] = id;
                    }
                }
            }
            Edit(volume.write(terrain::CellCoord{bx, 0, bz}, terrain::CellCoord{bx + kBlock - 1, kIslandRows - 1, bz + kBlock - 1},
                              distances, materials),
                 "an island block");
        }
    }
}

// The flat pass's colors per material Id 1 to 4, and every other Id's.
constexpr int kFlatClasses = 5;
constexpr int kFlatColors[kFlatClasses][3] = {
    {240, 40, 40}, {40, 240, 40}, {40, 40, 240}, {240, 240, 40}, {240, 40, 240}};

std::vector<float> FlatLookBytes() {
    std::vector<float> texels(256 * 4 * 4, 0.f);
    for (int id = 0; id < 256; ++id) {
        const int* color = kFlatColors[id >= 1 && id <= 4 ? id - 1 : 4];
        float* out = texels.data() + id * 4;
        for (int channel = 0; channel < 3; ++channel) {
            out[channel] = static_cast<float>(color[channel]) / 255.f;
        }
        out[3] = 1.f;
        float* surface = texels.data() + (256 + id) * 4;
        surface[0] = 0.f;
        surface[1] = 1.f;
        surface[2] = 0.f;
        surface[3] = 1.f;
    }
    return texels;
}

// A flat pass read back: per pixel -1 where it matches the empty pass (no
// terrain), else the material class (kFlatColors' index) whose color, scaled
// to its largest channel, is nearest.
struct FlatFrame {
    int width = 0;
    int height = 0;
    std::vector<std::int8_t> cls;
    int at(int x, int y) const { return cls[static_cast<std::size_t>(y) * width + x]; }
};

FlatFrame Classify(const runner::ViewPixels& pixels, const runner::ViewPixels& empty) {
    FlatFrame out;
    out.width = pixels.width;
    out.height = pixels.height;
    out.cls.assign(static_cast<std::size_t>(pixels.width) * pixels.height, -1);
    if (pixels.rgba.size() != empty.rgba.size()) {
        return out;
    }
    float palette[kFlatClasses][3];
    for (int c = 0; c < kFlatClasses; ++c) {
        for (int channel = 0; channel < 3; ++channel) {
            palette[c][channel] = kFlatColors[c][channel] / 240.f;
        }
    }
    for (std::size_t i = 0; i < out.cls.size(); ++i) {
        const unsigned char* p = pixels.rgba.data() + i * 4;
        const unsigned char* e = empty.rgba.data() + i * 4;
        int difference = 0;
        for (int channel = 0; channel < 3; ++channel) {
            difference = std::max(difference, std::abs(static_cast<int>(p[channel]) - static_cast<int>(e[channel])));
        }
        if (difference <= 3) {
            continue;
        }
        const float top = static_cast<float>(std::max({p[0], p[1], p[2], static_cast<unsigned char>(1)}));
        int best = 0;
        float bestDistance = std::numeric_limits<float>::max();
        for (int c = 0; c < kFlatClasses; ++c) {
            float d = 0.f;
            for (int channel = 0; channel < 3; ++channel) {
                const float v = p[channel] / top - palette[c][channel];
                d += v * v;
            }
            if (d < bestDistance) {
                bestDistance = d;
                best = c;
            }
        }
        out.cls[i] = static_cast<std::int8_t>(best);
    }
    return out;
}

// How a LOD flat frame differs from the full detail's.
struct LodDiff {
    std::int64_t referenceCovered = 0;
    std::int64_t lodCovered = 0;
    std::vector<std::pair<int, int>> crackAt;   // the first few cracks, x then y from the top
    std::int64_t cracks = 0;            // full detail covers, LOD does not, away from its silhouette
    std::int64_t outside = 0;           // LOD covers, away from the full detail's silhouette
    std::int64_t edgeDifferences = 0;   // coverage differs within kLodEdgeBand of the silhouette
    std::int64_t materialDiffers = 0;   // both cover, in different materials
    std::int64_t materialFar = 0;       // of those, the LOD's material nowhere within kLodMaterialBand
};

LodDiff CompareFlat(const FlatFrame& lod, const FlatFrame& reference, runner::ViewPixels* picture) {
    LodDiff diff;
    const int w = reference.width;
    const int h = reference.height;
    if (lod.width != w || lod.height != h || reference.cls.empty()) {
        return diff;
    }
    // Whether reference has, within band of (x, y), a pixel for which test holds.
    const auto near = [&](int x, int y, int band, const auto& test) {
        for (int yy = std::max(0, y - band); yy <= std::min(h - 1, y + band); ++yy) {
            for (int xx = std::max(0, x - band); xx <= std::min(w - 1, x + band); ++xx) {
                if (test(reference.at(xx, yy))) {
                    return true;
                }
            }
        }
        return false;
    };
    if (picture != nullptr) {
        picture->width = w;
        picture->height = h;
        picture->rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int r = reference.at(x, y);
            const int l = lod.at(x, y);
            diff.referenceCovered += r >= 0 ? 1 : 0;
            diff.lodCovered += l >= 0 ? 1 : 0;
            // Black empty, gray agreeing, blue a near silhouette or material difference,
            // red a crack, yellow outside, magenta a wrong material.
            std::array<unsigned char, 3> color = {0, 0, 0};
            if (r >= 0 && l >= 0) {
                color = {90, 90, 90};
            }
            if (r >= 0 && l < 0) {
                if (near(x, y, kLodEdgeBand, [](int c) { return c < 0; })) {
                    ++diff.edgeDifferences;
                    color = {40, 60, 200};
                } else {
                    ++diff.cracks;
                    if (diff.crackAt.size() < 4) {
                        diff.crackAt.emplace_back(x, y);
                    }
                    color = {255, 0, 0};
                }
            } else if (l >= 0 && r < 0) {
                if (near(x, y, kLodEdgeBand, [](int c) { return c >= 0; })) {
                    ++diff.edgeDifferences;
                    color = {40, 60, 200};
                } else {
                    ++diff.outside;
                    color = {255, 230, 0};
                }
            } else if (l >= 0 && r >= 0 && l != r) {
                ++diff.materialDiffers;
                if (near(x, y, kLodMaterialBand, [l](int c) { return c == l; })) {
                    color = {40, 60, 200};
                } else {
                    ++diff.materialFar;
                    color = {255, 0, 255};
                }
            }
            if (picture != nullptr) {
                unsigned char* p = picture->rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
                p[0] = color[0];
                p[1] = color[1];
                p[2] = color[2];
                p[3] = 255;
            }
        }
    }
    return diff;
}

// What LOD drew: draws (not shadow-only) per level, how many fade, and how many cast shadows only.
std::string LodSummary(const std::vector<runner::MeshDraw>& draws, int* fadingOut = nullptr) {
    int levels[8] = {};
    int fading = 0;
    int shadowOnly = 0;
    for (const runner::MeshDraw& draw : draws) {
        if (draw.shadowOnly) {
            ++shadowOnly;
            continue;
        }
        ++levels[std::min(std::max(draw.terrainLevel, 0), 7)];
        fading += draw.terrainFade < 1.f ? 1 : 0;
    }
    std::string text = std::to_string(static_cast<int>(draws.size()) - shadowOnly) + " nodes:";
    for (int level = 0; level < 8; ++level) {
        if (levels[level] > 0) {
            text += " L" + std::to_string(level) + "x" + std::to_string(levels[level]);
        }
    }
    text += ", " + std::to_string(fading) + " fading, " + std::to_string(shadowOnly) + " shadow-only";
    if (fadingOut != nullptr) {
        *fadingOut = fading;
    }
    return text;
}

// The island shots and sequence. lighting and sky are the terrain shots'
// own; dir gets the PNGs; colors adds the level-tinted shots.
void TerrainLodShots(runner::Renderer& renderer, OffscreenTarget& target, int width, int height,
                     const runner::SceneLighting& lighting, const runner::SkyState& sky,
                     const std::filesystem::path& dir, bool colors) {
    using Clock = std::chrono::steady_clock;
    const auto seconds = [](Clock::time_point since) {
        return std::chrono::duration<double>(Clock::now() - since).count();
    };
    const auto write = [&](const runner::ViewPixels& pixels, const std::string& file) {
        std::filesystem::create_directories(dir);
        std::ofstream(dir / file, std::ios::binary) << runner::EncodePng(pixels);
        std::printf("wrote %s\n", (dir / file).string().c_str());
    };
    runner::SceneLighting flat;
    flat.ambient[0] = flat.ambient[1] = flat.ambient[2] = 1.f;
    flat.antialiasing = runner::SceneAntialiasing::None;
    const std::vector<float> flatBytes = FlatLookBytes();
    const unsigned flatLook = runner::MakeTerrainLookTexture(flatBytes.data());

    engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
    {
        const Clock::time_point built = Clock::now();
        engine_core::Game game;
        engine_core::Terrain& placed = game.create<engine_core::Terrain>();
        game.set_parent(placed.id(), game.scene_service("Workspace"));
        const auto material = [&game](const char* name, float r, float g, float b) {
            engine_core::Material& made = game.create<engine_core::Material>();
            game.set_name(made.id(), name);
            game.set_parent(made.id(), game.service("Materials"));
            engine_core::ColorRgb color;
            color.r = r;
            color.g = g;
            color.b = b;
            color.a = 1.f;
            Expect(!made.set_color(color).has_value(), std::string(name) + " takes its color");
            return made.id();
        };
        engine_core::TerrainMaterial* entry = nullptr;
        Expect(!placed.add_material(material("IslandGrass", 0.33f, 0.55f, 0.22f), entry).has_value() &&
                   !placed.add_material(material("IslandRock", 0.47f, 0.43f, 0.39f), entry).has_value() &&
                   !placed.add_material(material("IslandSand", 0.85f, 0.77f, 0.56f), entry).has_value() &&
                   !placed.add_material(material("IslandSnow", 0.95f, 0.96f, 0.98f), entry).has_value() &&
                   entry != nullptr && entry->material_id() == 4,
               "the island's four TerrainMaterials take Ids 1 to 4");
        WriteIsland(placed.volume());
        const double wroteSeconds = seconds(built);

        // The camera TerrainWorld keeps levels 0-1 resident around.
        engine_core::Camera& eye = game.create<engine_core::Camera>();
        game.set_parent(eye.id(), game.scene_service("Workspace"));
        auto* workspace = dynamic_cast<engine_core::Workspace*>(game.instance(game.scene_service("Workspace")));
        Expect(workspace != nullptr && workspace->set_current_camera(eye.id()), "the island's camera is current");

        // A normal pool; the edit at the end makes its own, which can hold jobs.
        engine_core::TerrainWorld world;
        // Until an update after the pool is idle publishes no new node set.
        const auto settleWorld = [&](engine_core::TerrainWorld& pool, int passes) {
            std::uint64_t settled = ~std::uint64_t{0};
            for (int pass = 0; pass < passes; ++pass) {
                pool.update(game);
                pool.wait_idle();
                pool.update(game);
                const std::uint64_t revision = pool.views().empty() ? 0 : pool.views()[0].nodes_revision;
                if (revision == settled) {
                    return pass;
                }
                settled = revision;
            }
            return passes;
        };
        const auto settle = [&](int passes) { return settleWorld(world, passes); };
        const auto lookFrom = [&](const engine_core::Vec3& from, const engine_core::Vec3& to) {
            const engine_core::Matrix4 placedAt = engine_core::matrix4_look_at(from, to, {0.f, 1.f, 0.f});
            (void)eye.set_transform(placedAt);
            return placedAt;
        };
        const auto ground = [](float x, float z) { return std::max(IslandHeight(x, z), 2.f); };

        // The shots: a low grazing look across the island, a high far one, and one from far off.
        const engine_core::Vec3 grazingEye{150.f, ground(150.f, 380.f) + 9.f, 380.f};
        const engine_core::Vec3 grazingAt{760.f, 26.f, 610.f};
        const engine_core::Vec3 farEye{250.f, 450.f, 250.f};
        const engine_core::Vec3 farAt{512.f, 20.f, 512.f};

        lookFrom(grazingEye, grazingAt);
        const int passes = settle(400);
        const engine_core::TerrainView* view = world.views().empty() ? nullptr : &world.views()[0];
        Expect(view != nullptr && view->nodes != nullptr && view->look != nullptr, "the island publishes LOD nodes");
        if (view == nullptr || view->nodes == nullptr || view->look == nullptr) {
            engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
            return;
        }
        int publishedLevels[16] = {};
        for (const engine_core::TerrainNodeView& node : *view->nodes) {
            ++publishedLevels[std::min(node.key.level, 15)];
        }
        std::string published;
        for (int level = 0; level < 16; ++level) {
            if (publishedLevels[level] > 0) {
                published += " L" + std::to_string(level) + "x" + std::to_string(publishedLevels[level]);
            }
        }
        std::printf("lod island: %d published nodes (top level %d):%s; voxels %.2f s, settled in %d passes, %.2f s\n",
                    static_cast<int>(view->nodes->size()), view->top_level, published.c_str(), wroteSeconds, passes,
                    seconds(built));

        // The full detail: every level-0 chunk mesh, meshed here from the voxels, in the flat look.
        const Clock::time_point meshing = Clock::now();
        const auto chunks = UploadTerrain(placed.volume());
        std::vector<runner::MeshDraw> reference = TerrainDraws(chunks, flatLook);
        std::printf("lod island full detail: %d chunk meshes, %.2f s\n", static_cast<int>(chunks.size()),
                    seconds(meshing));

        runner::MeshCache meshes;
        runner::ViewPixels empty;
        // A flat pass of draws (their looks swapped for the flat one), classified.
        const auto flatPass = [&](std::vector<runner::MeshDraw> draws, const engine_core::Matrix4& camera) {
            for (runner::MeshDraw& draw : draws) {
                draw.terrainLook = flatLook;
            }
            renderer.setLighting(flat);
            renderer.setCamera(camera, 50.f);
            runner::ViewPixels pixels;
            if (empty.rgba.empty()) {
                Expect(target.shoot(renderer, {}, nullptr, 0, 2, empty), "the empty flat pass draws");
            }
            Expect(target.shoot(renderer, draws, nullptr, 0, 2, pixels), "a flat pass draws");
            renderer.setLighting(lighting);
            return Classify(pixels, empty);
        };
        // The lit shot of draws, checked against the sky alone.
        const auto litShot = [&](const std::vector<runner::MeshDraw>& draws, const engine_core::Matrix4& camera,
                                 const std::string& file) {
            renderer.setLighting(lighting);
            renderer.setCamera(camera, 50.f);
            runner::LightDraw sun = runner::SkyLightDraw(sky, true);
            sun.shadowDistance = 1000.f;
            runner::ViewPixels skyOnly;
            runner::ViewPixels pixels;
            const bool drawn = target.shoot(renderer, {}, sun, skyOnly) && target.shoot(renderer, draws, sun, pixels);
            Expect(drawn, file + " draws and reads back");
            if (drawn) {
                ExpectTerrainShown(file, pixels, skyOnly);
                write(pixels, file);
            }
        };
        const auto camera = [&](const engine_core::Matrix4& cameraWorld) {
            runner::TerrainCamera out;
            out.world = cameraWorld;
            out.fov_y_degrees = 50.f;
            out.pane_width = width;
            out.pane_height = height;
            return out;
        };
        // Checks one LOD frame against the full detail; picture names a diff PNG to write.
        std::int64_t worstCracks = 0;
        std::int64_t worstOutside = 0;
        double worstMaterialFar = 0.0;
        // Each drawn node of the next check (in draw order), and the largest
        // pixel error of those fading in (or steady) and fading out, with the
        // node and how many of its children are published (fewer than its
        // child_mask asks: selection had to draw it), from describe:
        // selection again on a copy of the frame's fade state.
        double now = 0.0;
        std::vector<std::string> described;
        float worstIncoming = 0.f;
        float worstOutgoing = 0.f;
        std::string worstIncomingNode;
        std::string worstOutgoingNode;
        const auto describe = [&](const engine_core::Matrix4& cameraWorld, runner::TerrainFadeState copy) {
            described.clear();
            worstIncoming = worstOutgoing = 0.f;
            worstIncomingNode = worstOutgoingNode = "none";
            const engine_core::TerrainView& current = world.views()[0];
            const auto keyLess = [](const engine_core::TerrainNodeView& a, const engine_core::TerrainNodeView& b) {
                return std::tie(a.key.level, a.key.x, a.key.y, a.key.z) < std::tie(b.key.level, b.key.x, b.key.y, b.key.z);
            };
            std::vector<runner::NodeChoice> choices;
            runner::SelectTerrainNodes(current, camera(cameraWorld), now, copy, choices);
            const engine_core::Vec3 eyeAt = engine_core::matrix4_position(cameraWorld);
            for (const runner::NodeChoice& choice : choices) {
                const engine_core::TerrainNodeView& node = (*current.nodes)[choice.index];
                const float dx = std::max({node.bounds_min.x - eyeAt.x, 0.f, eyeAt.x - node.bounds_max.x});
                const float dy = std::max({node.bounds_min.y - eyeAt.y, 0.f, eyeAt.y - node.bounds_max.y});
                const float dz = std::max({node.bounds_min.z - eyeAt.z, 0.f, eyeAt.z - node.bounds_max.z});
                const float pixels = runner::NodePixelError(node.error, std::sqrt(dx * dx + dy * dy + dz * dz), 50.f, height);
                // Fix round 1 (point 4): a level-0 node (a chunk) is always a
                // leaf -- nodes_for_view never gives it a child_mask -- so
                // children_of(node.key), which asserts level >= 1, must not
                // run for one; this is where the TerrainLodShots crash came
                // from, a pre-existing bug in this diagnostic lambda alone
                // (every children_of() call in src/ already guards level ==
                // 0), not in anything Task 6 touched.
                int asked = 0;
                int published = 0;
                if (node.key.level >= 1) {
                    const std::array<terrain::NodeKey, 8> children = terrain::children_of(node.key);
                    for (int c = 0; c < 8; ++c) {
                        if ((node.child_mask & (1u << c)) != 0) {
                            ++asked;
                            engine_core::TerrainNodeView probe;
                            probe.key = children[static_cast<std::size_t>(c)];
                            published +=
                                std::binary_search(current.nodes->begin(), current.nodes->end(), probe, keyLess) ? 1 : 0;
                        }
                    }
                }
                char text[128];
                std::snprintf(text, sizeof(text), "L%d(%d,%d,%d) %s %.2f, %.2f px, children %d of %d published",
                              node.key.level, node.key.x, node.key.y, node.key.z, choice.incoming ? "in" : "out",
                              choice.fade, pixels, published, asked);
                described.emplace_back(text);
                float& worst = choice.incoming ? worstIncoming : worstOutgoing;
                if (pixels > worst) {
                    worst = pixels;
                    (choice.incoming ? worstIncomingNode : worstOutgoingNode) = text;
                }
            }
        };
        const auto check = [&](const std::vector<runner::MeshDraw>& draws, const engine_core::Matrix4& cameraWorld,
                               const std::string& name, const std::string& picture) {
            const FlatFrame lod = flatPass(draws, cameraWorld);
            const FlatFrame full = flatPass(reference, cameraWorld);
            runner::ViewPixels diffPicture;
            const LodDiff diff = CompareFlat(lod, full, &diffPicture);
            std::printf("%s: largest pixel error drawn: fading in or steady %.2f (%s); fading out %.2f (%s)\n",
                        name.c_str(), worstIncoming, worstIncomingNode.c_str(), worstOutgoing, worstOutgoingNode.c_str());
            // Which drawn nodes, each alone and whole, cover each crack or the pixels next to it.
            if (!diff.crackAt.empty()) {
                for (std::size_t i = 0; i < draws.size(); ++i) {
                    if (draws[i].shadowOnly) {
                        continue;
                    }
                    runner::MeshDraw whole = draws[i];
                    whole.terrainFade = 1.f;
                    whole.terrainFadeIn = true;
                    const FlatFrame alone = flatPass({whole}, cameraWorld);
                    for (const auto& at : diff.crackAt) {
                        std::string around;
                        for (int oy = -1; oy <= 1; ++oy) {
                            for (int ox = -1; ox <= 1; ++ox) {
                                const int px = std::min(std::max(at.first + ox, 0), width - 1);
                                const int py = std::min(std::max(at.second + oy, 0), height - 1);
                                around += alone.at(px, py) >= 0 ? '#' : '.';
                            }
                        }
                        if (around != ".........") {
                            std::printf("  crack (%d, %d): draw %zu %s covers 3x3 %s\n", at.first, at.second, i,
                                        i < described.size() ? described[i].c_str() : "?", around.c_str());
                        }
                    }
                }
            }
            const double materialFar =
                diff.referenceCovered > 0 ? 100.0 * static_cast<double>(diff.materialFar) / diff.referenceCovered : 0.0;
            std::printf("%s: %s | covered: full %lld, LOD %lld | cracks %lld, outside %lld, edge %lld | material "
                        "differs %lld, far %lld (%.3f%%)\n",
                        name.c_str(), LodSummary(draws).c_str(), static_cast<long long>(diff.referenceCovered),
                        static_cast<long long>(diff.lodCovered), static_cast<long long>(diff.cracks),
                        static_cast<long long>(diff.outside), static_cast<long long>(diff.edgeDifferences),
                        static_cast<long long>(diff.materialDiffers), static_cast<long long>(diff.materialFar),
                        materialFar);
            worstCracks = std::max(worstCracks, diff.cracks);
            worstOutside = std::max(worstOutside, diff.outside);
            worstMaterialFar = std::max(worstMaterialFar, materialFar);
            Expect(diff.referenceCovered * 20 > static_cast<std::int64_t>(width) * height,
                   name + ": the full detail covers over 5% of the frame");
            Expect(diff.cracks == 0, name + ": no cracks (" + std::to_string(diff.cracks) + " pixels)");
            Expect(diff.outside == 0, name + ": nothing outside the true silhouette (" + std::to_string(diff.outside) +
                                          " pixels)");
            Expect(materialFar < 0.05, name + ": no wrong-material triangles (" + std::to_string(diff.materialFar) +
                                           " pixels)");
            if (!picture.empty() || diff.cracks > 0 || diff.outside > 0 || materialFar >= 0.05) {
                write(diffPicture, picture.empty() ? "lod-diff-" + name + ".png" : picture);
            }
        };
        // Selection from a fresh fade state, repeated at the same moment until
        // every out-of-view shadow caster is uploaded (16 a frame).
        const auto still = [&](const engine_core::Matrix4& cameraWorld) {
            runner::TerrainFadeState fresh;
            std::vector<runner::MeshDraw> out;
            std::size_t last = 0;
            for (int warm = 0; warm < 400; ++warm) {
                out.clear();
                runner::AppendTerrainDraws(world.views(), camera(cameraWorld), now, fresh, meshes, renderer, out);
                if (warm > 0 && out.size() == last) {
                    break;
                }
                last = out.size();
            }
            describe(cameraWorld, fresh);
            return out;
        };
        // One still shot: lit, the full detail lit, checked, and level-tinted.
        const auto stillShot = [&](const engine_core::Matrix4& cameraWorld, const std::string& name) {
            const std::vector<runner::MeshDraw> shot = still(cameraWorld);
            const engine_core::TerrainView& current = world.views()[0];
            litShot(shot, cameraWorld, "lod-" + name + ".png");
            litShot(TerrainDraws(chunks, renderer.terrainLookTexture(current.terrain, *current.look)), cameraWorld,
                    "lod-" + name + "-full-detail.png");
            check(shot, cameraWorld, name, "lod-diff-" + name + ".png");
            if (colors) {
                runner::SetTerrainLodColors(true);
                litShot(shot, cameraWorld, "lod-colors-" + name + ".png");
                runner::SetTerrainLodColors(false);
            }
            now += 10.0;
        };
        stillShot(lookFrom(grazingEye, grazingAt), "grazing");
        const engine_core::Matrix4 far = lookFrom(farEye, farAt);
        settle(400);
        stillShot(far, "far");
        std::vector<runner::MeshDraw> draws;

        // 30 frames 0.1 s apart from the far camera down to 4 units over the
        // ground at (endX, endZ), looking at endAt by the end, every one
        // checked, every 10th (and the last) written, and the one with the
        // most nodes cross-fading. label names the frames; returns that most.
        constexpr int kFrames = 30;
        constexpr double kFrameSeconds = 0.1;
        const auto descend = [&](const char* label, float endX, float endZ, const engine_core::Vec3& endAt) {
            runner::TerrainFadeState fades;
            worstCracks = worstOutside = 0;
            worstMaterialFar = 0.0;
            int mostFading = 0;
            int mostFadingFrame = -1;
            std::vector<runner::MeshDraw> mostFadingDraws;
            engine_core::Matrix4 mostFadingCamera = engine_core::matrix4_identity();
            const Clock::time_point sequence = Clock::now();
            for (int frame = 0; frame < kFrames; ++frame) {
                const float s = static_cast<float>(frame) / static_cast<float>(kFrames - 1);
                const float x = farEye.x + (endX - farEye.x) * s;
                const float z = farEye.z + (endZ - farEye.z) * s;
                const float startAbove = farEye.y - ground(farEye.x, farEye.z);
                const float above = startAbove * std::pow(4.f / startAbove, s);
                const engine_core::Vec3 from{x, ground(x, z) + above, z};
                const engine_core::Vec3 to{farAt.x + (endAt.x - farAt.x) * s, farAt.y + (endAt.y - farAt.y) * s,
                                           farAt.z + (endAt.z - farAt.z) * s};
                const engine_core::Matrix4 cameraWorld = lookFrom(from, to);
                // The pool keeps up: what this frame's camera queued is built and published.
                world.update(game);
                world.wait_idle();
                world.update(game);
                draws.clear();
                describe(cameraWorld, fades);
                runner::AppendTerrainDraws(world.views(), camera(cameraWorld), now, fades, meshes, renderer, draws);
                char name[32];
                std::snprintf(name, sizeof(name), "%s-%02d", label, frame);
                int fading = 0;
                LodSummary(draws, &fading);
                const bool written = frame % 10 == 0 || frame == kFrames - 1;
                check(draws, cameraWorld, name, "");
                if (written) {
                    litShot(draws, cameraWorld, std::string("lod-") + name + ".png");
                } else if (fading > mostFading) {
                    mostFading = fading;
                    mostFadingFrame = frame;
                    mostFadingDraws = draws;
                    mostFadingCamera = cameraWorld;
                }
                now += kFrameSeconds;
            }
            std::printf("lod %s: %d frames in %.2f s; worst cracks %lld, outside %lld, wrong material %.3f%%\n", label,
                        kFrames, seconds(sequence), static_cast<long long>(worstCracks),
                        static_cast<long long>(worstOutside), worstMaterialFar);
            if (mostFadingFrame >= 0) {
                char name[48];
                std::snprintf(name, sizeof(name), "lod-%s-fade-%02d.png", label, mostFadingFrame);
                std::printf("the %s descent's most cross-fading frame: %d (%d nodes fading)\n", label, mostFadingFrame,
                            mostFading);
                litShot(mostFadingDraws, mostFadingCamera, name);
            }
            return mostFading;
        };
        // Down onto the rolling grass west of the ridge, looking on west across it.
        const int mostFading = descend("seq", 400.f, 560.f,
                                       engine_core::Vec3{220.f, ground(220.f, 680.f), 680.f});
        Expect(mostFading > 0, "the sequence cross-fades between levels");
        // Down onto the ridge's west flank, 12 units below its crest at (560,
        // 512), looking up into the rock face: where a coarse level-1 node once
        // dithered out near the camera with the wrong material (0.72%).
        lookFrom(farEye, farAt);
        settle(400);
        descend("ridge", 550.4f, 504.8f, engine_core::Vec3{560.f, ground(560.f, 512.f) + 2.f, 512.f});

        // From off the island's corner, 1300 units from its middle: its far
        // half lies past 1000 units, which the far plane (kSceneFar) once cut off.
        const engine_core::Vec3 outerEye{-350.f, 500.f, -350.f};
        const engine_core::Matrix4 outer = lookFrom(outerEye, farAt);
        settle(400);
        stillShot(outer, "far-4000");

        // A bomb by a close camera (rulings R24, R26): a ball of radius 8 out
        // of the ground where its top is in chunk y 1 (so the air chunks
        // queued above it sit in level-1 nodes with no surface), seen from 40
        // units off and 6 above the ground. Frames at 60 fps while the pool
        // works unwaited: none may fade, or draw the edit's 3 x 3 x 3 chunks
        // with a coarser node than before it, and each is checked against the
        // full detail after the edit: no see-through crack in any frame. Its
        // own TerrainWorld, with 64 workers and a hook that can hold air
        // chunk jobs (the rest of the shots use a normal pool); its own mesh
        // cache, since its revisions start again from 1.
        {
            std::atomic<bool> holdAir{false};
            engine_core::TerrainWorld editWorld(
                [&holdAir](const terrain::ChunkMesh& mesh) -> std::shared_ptr<void> {
                    while (mesh.triangles.empty() && holdAir.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    return nullptr;
                },
                64);
            struct Release {
                std::atomic<bool>& hold;
                ~Release() { hold = false; }
            } release{holdAir};
            runner::MeshCache editMeshes;
            runner::TerrainFadeState editFades;
            // The first spot from the middle outwards whose ground is 38 to 56 units high.
            engine_core::Vec3 bomb{kIslandMiddle, ground(kIslandMiddle, kIslandMiddle), kIslandMiddle};
            for (float r = 0.f; r < 300.f && !(bomb.y >= 38.f && bomb.y <= 56.f); r += 8.f) {
                for (float a = 0.f; a < 6.2832f; a += 0.1f) {
                    const float x = kIslandMiddle + r * std::cos(a);
                    const float z = kIslandMiddle + r * std::sin(a);
                    if (ground(x, z) >= 38.f && ground(x, z) <= 56.f) {
                        bomb = engine_core::Vec3{x, ground(x, z), z};
                        break;
                    }
                }
            }
            const engine_core::Vec3 editEye{bomb.x - 40.f, std::max(ground(bomb.x - 40.f, bomb.z), bomb.y) + 6.f,
                                            bomb.z};
            const engine_core::Matrix4 editCamera = lookFrom(editEye, bomb);
            const Clock::time_point editSettle = Clock::now();
            settleWorld(editWorld, 400);
            std::printf("lod edit: its own pool settled in %.2f s\n", seconds(editSettle));
            constexpr float kChunkUnits = static_cast<float>(terrain::kChunkSize);
            const int bombChunk[3] = {static_cast<int>(std::floor(bomb.x / kChunkUnits)),
                                      static_cast<int>(std::floor(bomb.y / kChunkUnits)),
                                      static_cast<int>(std::floor(bomb.z / kChunkUnits))};
            std::printf("lod edit: a bomb at (%.0f, %.0f, %.0f), chunk (%d, %d, %d)\n", bomb.x, bomb.y, bomb.z,
                        bombChunk[0], bombChunk[1], bombChunk[2]);
            Expect(bombChunk[1] == 1, "lod edit: the bomb's ground is in chunk y 1");
            // The coarsest level drawn over the edit's chunks, and how many draws fade.
            const auto editFrame = [&](int& coarsest, int& fadingDraws) {
                runner::TerrainFadeState copy = editFades;
                std::vector<runner::NodeChoice> choices;
                const engine_core::TerrainView& current = editWorld.views()[0];
                runner::SelectTerrainNodes(current, camera(editCamera), now, copy, choices);
                coarsest = -1;
                fadingDraws = 0;
                for (const runner::NodeChoice& choice : choices) {
                    const terrain::NodeKey& key = (*current.nodes)[choice.index].key;
                    fadingDraws += (choice.fade < 1.f || !choice.incoming) ? 1 : 0;
                    const int side = 1 << key.level;
                    bool over = true;
                    const int at[3] = {key.x, key.y, key.z};
                    for (int a = 0; a < 3; ++a) {
                        over = over && at[a] * side <= bombChunk[a] + 1 && (at[a] + 1) * side - 1 >= bombChunk[a] - 1;
                    }
                    if (over) {
                        coarsest = std::max(coarsest, key.level);
                    }
                }
                draws.clear();
                runner::AppendTerrainDraws(editWorld.views(), camera(editCamera), now, editFades, editMeshes, renderer,
                                           draws);
            };
            int before = -1;
            int fadingBefore = 0;
            for (int frame = 0; frame < 8; ++frame) {   // any fade from the camera's move is done
                editWorld.update(game);
                editFrame(before, fadingBefore);
                now += 0.1;
            }
            Expect(fadingBefore == 0 && before >= 0, "lod edit: settled before the bomb (" +
                                                         std::to_string(fadingBefore) + " fading, coarsest L" +
                                                         std::to_string(before) + ")");
            terrain::Shape ball;
            ball.center = bomb;
            ball.radius = 8.f;
            // The air chunk jobs (no triangles) are held until the others have
            // landed (no more for 50 frames): an order the pool may finish them
            // in, made certain. R26: the edit's batch waits for all of them.
            holdAir = true;
            const std::uint64_t meshedBefore = editWorld.meshed_count();
            Expect(!placed.edit_volume([&](terrain::VoxelVolume& v) { return v.subtract(ball); }).has_value(),
                   "lod edit: the bomb subtracts");
            // The full detail after the edit, from the voxels (the pool has not seen the edit yet).
            const Clock::time_point remeshing = Clock::now();
            const auto chunksAfter = UploadTerrain(placed.volume());
            const FlatFrame fullAfter = flatPass(TerrainDraws(chunksAfter, flatLook), editCamera);
            std::printf("lod edit: the full detail after it, %d chunk meshes, %.2f s\n",
                        static_cast<int>(chunksAfter.size()), seconds(remeshing));
            int coarsestDuring = -1;
            int fadingDuring = 0;
            int framesNodesChanged = 0;
            int framesNodesChangedHeld = 0;
            int frames = 0;
            int framesCracked = 0;
            std::int64_t worstEditCracks = 0;
            std::int64_t worstEditOutside = 0;
            int worstEditFrame = -1;
            runner::ViewPixels worstEditDiff;
            std::uint64_t lastRevision = editWorld.views()[0].nodes_revision;
            const auto oneFrame = [&](const char* shot, bool held) {
                editWorld.update(game);
                if (editWorld.views()[0].nodes_revision != lastRevision) {
                    lastRevision = editWorld.views()[0].nodes_revision;
                    ++framesNodesChanged;
                    framesNodesChangedHeld += held && frames > 0 ? 1 : 0;
                }
                int coarsest = -1;
                int fading = 0;
                editFrame(coarsest, fading);
                coarsestDuring = std::max(coarsestDuring, coarsest);
                fadingDuring += fading;
                // See-through: the full detail after the edit covers a pixel this frame leaves empty.
                runner::ViewPixels diffPicture;
                const LodDiff diff = CompareFlat(flatPass(draws, editCamera), fullAfter, &diffPicture);
                framesCracked += diff.cracks > 0 ? 1 : 0;
                worstEditOutside = std::max(worstEditOutside, diff.outside);
                if (diff.cracks > worstEditCracks) {
                    worstEditCracks = diff.cracks;
                    worstEditFrame = frames;
                    worstEditDiff = std::move(diffPicture);
                }
                if (shot != nullptr) {
                    litShot(draws, editCamera, shot);
                }
                now += 1.0 / 60.0;
                ++frames;
            };
            std::uint64_t landed = meshedBefore;
            int heldFrames = 0;
            for (int wait = 0, quiet = 0; wait < 3000 && (landed == meshedBefore || quiet < 50); ++wait) {
                oneFrame(nullptr, true);
                ++heldFrames;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                quiet = editWorld.meshed_count() == landed ? quiet + 1 : 0;
                landed = editWorld.meshed_count();
            }
            std::printf("lod edit: %d chunk jobs with triangles landed while the air ones were held\n",
                        static_cast<int>(landed - meshedBefore));
            Expect(landed > meshedBefore, "lod edit: the bomb's surface chunks landed");
            for (int frame = 0; frame < 10; ++frame) {
                oneFrame(frame == 9 ? "lod-edit-during.png" : nullptr, true);
                ++heldFrames;
            }
            holdAir = false;
            for (int frame = 0; frame < 50; ++frame) {
                if (frame == 40) {
                    editWorld.wait_idle();   // the rest lands; the last frames see the rebuilt nodes
                }
                oneFrame(frame == 49 ? "lod-edit-after.png" : nullptr, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::printf("lod edit: coarsest over the edit L%d before, L%d during; %d draws faded over %d frames; the "
                        "node set changed in %d of them (%d while the air was held, after the edit's own frame)\n",
                        before, coarsestDuring, fadingDuring, frames, framesNodesChanged, framesNodesChangedHeld);
            std::printf("lod edit: against the full detail after it, %d of %d frames see through (worst %lld pixels, "
                        "frame %d); outside %lld at most (the old surface over the crater, while held)\n",
                        framesCracked, frames, static_cast<long long>(worstEditCracks), worstEditFrame,
                        static_cast<long long>(worstEditOutside));
            if (worstEditFrame >= 0) {
                write(worstEditDiff, "lod-diff-edit-worst.png");
            }
            Expect(framesNodesChanged > 0, "lod edit: the bomb re-meshed nodes");
            Expect(framesNodesChangedHeld == 0, "lod edit: nothing of the batch shows while part of it is held (" +
                                                    std::to_string(framesNodesChangedHeld) + " frames changed)");
            Expect(fadingDuring == 0, "lod edit: nothing fades (" + std::to_string(fadingDuring) + " draws)");
            Expect(coarsestDuring <= before, "lod edit: nothing over it drawn coarser (L" +
                                                 std::to_string(coarsestDuring) + " against L" +
                                                 std::to_string(before) + ")");
            Expect(framesCracked == 0, "lod edit: no frame sees through the edit (" + std::to_string(framesCracked) +
                                           " frames, worst " + std::to_string(worstEditCracks) + " pixels)");
            draws.clear();
            runner::TerrainFadeState none;
            runner::AppendTerrainDraws({}, runner::TerrainCamera{}, now, none, editMeshes, renderer, draws);
            editMeshes.clear();
        }

        draws.clear();
        runner::TerrainFadeState none;
        runner::AppendTerrainDraws({}, runner::TerrainCamera{}, now, none, meshes, renderer, draws);
        meshes.clear();
    }
    engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
    runner::GLuint texture = flatLook;
    glDeleteTextures(1, &texture);
    renderer.setLighting(lighting);
    Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the terrain LOD shots leave no GL error");
}

// Task 9's GPU budget (--terrain-budget): the same large island WriteIsland
// builds for TerrainLodShots, meshed once at full detail (no LOD selection --
// a budget measurement wants a steady, repeatable draw, not a background
// selection/fade system settling), textured with four procedural layers
// (grass, rock, sand, and the untextured default layer standing in for
// snow), seen from a wide overview. Each of Low, Medium, High (textured) and
// a flat, untextured look (same geometry, uHasSurface == 0's old path) is
// drawn for a short untimed warm-up, then 60 frames with the profiler's
// per-pass GPU detail on; the "Terrain" RENDER_PASS's GPU-timed scopes
// (GpuTimer, read a frame or more late, so a few extra frames beyond the 60
// help flush the last ones) are averaged into one ms figure. Printed, then
// checked against the brief's budget: High <= 2.0x flat, Low <= 1.2x flat.
void TerrainBudget(runner::Renderer& renderer) {
    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;
    OffscreenTarget target(kWidth, kHeight);
    Expect(target.complete(), "the 1920 by 1080 terrain budget target is complete");

    terrain::VoxelVolume volume;
    WriteIsland(volume);
    const auto chunks = UploadTerrain(volume);
    std::printf("terrain budget: %d chunk meshes (full detail, no LOD)\n", static_cast<int>(chunks.size()));

    // Layer 0 is the texture set's own untextured default (white, 0.5
    // height, flat normal, rough/metal 1) -- reused here as a cheap stand-in
    // for snow instead of a fourth procedural kind, since the budget only
    // needs four distinct layers under load, not a fourth accurate look.
    const auto set = MakeTextureSet(1024, {1, 0, 2}, 9701);
    const unsigned texLook = runner::MakeTerrainLookTexture(TexturedLookFloats({
                                                                 {1, 92, 142, 58, 1, 8.f},    // grass
                                                                 {2, 122, 110, 98, 2, 8.f},   // rock
                                                                 {3, 218, 196, 142, 3, 8.f},  // sand
                                                                 {4, 242, 245, 250, 0, 8.f},  // snow (layer 0)
                                                             })
                                                                 .data());
    std::vector<runner::MeshDraw> texturedDraws;
    for (int frame = 0; frame < 6; ++frame) {
        // Repeated until terrainArrays finishes uploading every layer (at
        // most 4 per frame) and swaps to the new array pair.
        texturedDraws = TexturedTerrainDraws(chunks, renderer, 9701, texLook, set);
    }
    const unsigned flatLook = runner::MakeTerrainLookTexture(LookBytes({
                                                                  {0, 150, 150, 150},
                                                                  {1, 92, 142, 58},
                                                                  {2, 122, 110, 98},
                                                                  {3, 218, 196, 142},
                                                                  {4, 242, 245, 250},
                                                              })
                                                                  .data());
    const std::vector<runner::MeshDraw> flatDraws = TerrainDraws(chunks, flatLook);

    runner::SceneLighting lighting;
    lighting.ambient[0] = lighting.ambient[1] = lighting.ambient[2] = 1.f;
    lighting.antialiasing = runner::SceneAntialiasing::None;
    renderer.setLighting(lighting);
    renderer.setCamera(
        engine_core::matrix4_look_at({250.f, 450.f, 250.f}, {512.f, 20.f, 512.f}, {0.f, 1.f, 0.f}), 50.f);

    // A sanity check that something real is drawn, before trusting the GPU
    // timer's numbers: the textured island must actually cover the view.
    runner::ViewPixels sky;
    runner::ViewPixels check;
    Expect(target.shoot(renderer, {}, nullptr, 0, 2, sky), "the terrain budget sky alone draws");
    Expect(target.shoot(renderer, texturedDraws, nullptr, 0, 2, check), "the terrain budget island draws");
    ExpectTerrainShown("terrain budget", check, sky);

    profiler::register_thread("UI");
    const auto measure = [&](const std::vector<runner::MeshDraw>& draws, const char* name) -> double {
        // Untimed warm-up: the shadow cascades and any remaining array
        // uploads settle before anything is measured.
        for (int frame = 0; frame < 10; ++frame) {
            target.shoot(renderer, draws, nullptr, 0, 1, check);
        }
        profiler::reset_for_testing();
        profiler::set_gpu_detail(true);
        profiler::acquire();
        constexpr int kMeasuredFrames = 60;
        constexpr int kFlushFrames = 4;  // lets the last few GPU queries land
        for (int frame = 0; frame < kMeasuredFrames + kFlushFrames; ++frame) {
            profiler::frame_boundary();
            target.shoot(renderer, draws, nullptr, 0, 1, check);
        }
        profiler::frame_boundary();
        profiler::collect();
        double totalNs = 0.0;
        int samples = 0;
        profiler::with_live([&](const profiler::History& history) {
            for (const profiler::Frame& frame : history.frames) {
                for (const profiler::ScopeRecord& record : frame.scopes) {
                    if (history.rows[record.row] != "GPU") {
                        continue;
                    }
                    const profiler::ScopeInfo& info = history.scopes[record.scope];
                    if (info.name == "Terrain" && info.group == profiler::Group::Gpu) {
                        totalNs += static_cast<double>(record.end_ns - record.start_ns);
                        ++samples;
                    }
                }
            }
        });
        profiler::release();
        profiler::set_gpu_detail(false);
        const double ms = samples > 0 ? (totalNs / samples) / 1e6 : -1.0;
        std::printf("terrain budget %s: %.4f ms (%d GPU samples)\n", name, ms, samples);
        return ms;
    };

    const double flatMs = measure(flatDraws, "flat");
    lighting.terrainQuality = runner::SceneQuality::Low;
    renderer.setLighting(lighting);
    const double lowMs = measure(texturedDraws, "Low");
    lighting.terrainQuality = runner::SceneQuality::Medium;
    renderer.setLighting(lighting);
    const double mediumMs = measure(texturedDraws, "Medium");
    lighting.terrainQuality = runner::SceneQuality::High;
    renderer.setLighting(lighting);
    const double highMs = measure(texturedDraws, "High");

    Expect(flatMs > 0.0 && lowMs > 0.0 && mediumMs > 0.0 && highMs > 0.0,
           "terrain budget: every quality level and flat got at least one GPU sample");
    if (flatMs > 0.0) {
        std::printf("terrain budget ratios: High/flat %.3f, Medium/flat %.3f, Low/flat %.3f\n", highMs / flatMs,
                    mediumMs / flatMs, lowMs / flatMs);
        Expect(highMs <= 2.0 * flatMs, "terrain budget: High is at most 2.0x flat (" + std::to_string(highMs) +
                                            " vs " + std::to_string(flatMs) + " ms)");
        Expect(lowMs <= 1.2 * flatMs, "terrain budget: Low is at most 1.2x flat (" + std::to_string(lowMs) +
                                           " vs " + std::to_string(flatMs) + " ms)");
    }

    runner::GLuint textures[2] = {texLook, flatLook};
    glDeleteTextures(2, textures);
    Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the terrain budget leaves no GL error");
}

}  // namespace

int main(int argc, char** argv) {
    // --save dir or --compare dir (the regression frames), --terrain-shots dir (the terrain PNGs).
    std::string regressionMode;
    std::filesystem::path regressionDir;
    std::filesystem::path terrainShots;
    // --terrain-lod-colors (with --terrain-shots): the LOD shots again, tinted by level.
    bool lodColors = false;
    // --terrain-budget: Task 9's GPU budget measurement, independent of --terrain-shots.
    bool budgetMode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--terrain-lod-colors") {
            lodColors = true;
            continue;
        }
        if (flag == "--terrain-budget") {
            budgetMode = true;
            continue;
        }
        if ((flag == "--save" || flag == "--compare" || flag == "--terrain-shots") && i + 1 < argc) {
            if (flag == "--terrain-shots") {
                terrainShots = argv[++i];
            } else {
                regressionMode = flag;
                regressionDir = argv[++i];
            }
            continue;
        }
        std::fprintf(stderr,
                     "usage: scene-render-check [--save dir | --compare dir] "
                     "[--terrain-shots dir [--terrain-lod-colors]] [--terrain-budget]\n");
        return 2;
    }

    // A scratch project resources folder with the cube and a file that is not AMESH.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "anarchy-scene-render-check";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "meshes");
    {
        const std::vector<std::byte> bytes = write(Cube());
        std::ofstream(root / "meshes" / "cube.amesh", std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::ofstream(root / "meshes" / "junk.amesh", std::ios::binary) << "not a mesh";
    }

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    constexpr int kSize = 128;
    GLFWwindow* window = glfwCreateWindow(kSize, kSize, "scene-render-check", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "no GL 3.3 core context\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    if (!runner::LoadGl([](const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); })) {
        return 1;
    }
    Expect(runner::rt_glTexImage3D != nullptr && runner::rt_glFramebufferTextureLayer != nullptr &&
               runner::rt_glPolygonOffset != nullptr && runner::rt_glReadBuffer != nullptr,
           "the shadow maps' GL calls load");
    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
    glViewport(0, 0, fbWidth, fbHeight);

    {
        runner::Renderer renderer;
        Expect(renderer.initialize(), "the renderer and its mesh program build");
        std::vector<std::string> reports;
        runner::MeshCache meshes([&reports](const std::string& message) { reports.push_back(message); });
        Expect(meshes.get("meshes/cube.amesh") == nullptr, "no root loads nothing");
        meshes.setRoot(root);
        const GpuMesh* cube = meshes.get("meshes/cube.amesh");
        Expect(cube != nullptr && cube->valid(), "the cube loads from the resources folder");
        Expect(meshes.get("meshes/cube.amesh") == cube, "a second get is the same upload");
        Expect(meshes.get("meshes/missing.amesh") == nullptr && meshes.get("meshes/junk.amesh") == nullptr,
               "a missing file and a file that is not AMESH draw nothing");
        Expect(reports.size() == 2, "each says why once (" + std::to_string(reports.size()) + " reports)");
        meshes.get("meshes/missing.amesh");
        Expect(reports.size() == 2, "a file still missing is not reported again");

        // The cube at the origin, under the camera a Renderer starts with, fills the middle of the pane.
        runner::MeshDraw draw{cube, engine_core::matrix4_identity()};
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
        const Pixel middle = ReadPixel(fbWidth / 2, fbHeight / 2);
        const Pixel corner = ReadPixel(2, 2);
        {
            // Profiled, each pass is a Render scope on the CPU. The GPU is timed
            // once a frame, as "3D scene", unless detail per pass is asked for.
            profiler::register_thread("UI");
            bool rooted = false;
            int misplaced = 0;
            auto run = [&](bool detail, int& cpu, int& scene, int& pass) {
                profiler::reset_for_testing();
                profiler::set_gpu_detail(detail);
                profiler::acquire();
                for (int frame = 0; frame < 8; ++frame) {
                    profiler::frame_boundary();
                    renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
                    glfwSwapBuffers(window);
                }
                profiler::frame_boundary();
                profiler::collect();
                cpu = scene = pass = 0;
                profiler::with_live([&](const profiler::History& history) {
                    for (const profiler::Frame& frame : history.frames) {
                        for (const profiler::ScopeRecord& record : frame.scopes) {
                            const profiler::ScopeInfo& info = history.scopes[record.scope];
                            // The 3D draw has one root on its row, 3D scene, as the other rows do.
                            if (history.rows[record.row] == "Render draw" && !rooted) {
                                rooted = true;
                            }
                            if (history.rows[record.row] == "Render draw" &&
                                (info.name == "3D scene") != (record.depth == 0)) {
                                ++misplaced;
                            }
                            const bool gpu = history.rows[record.row] == "GPU";
                            cpu += info.name == "Geometry" && info.group == profiler::Group::Render &&
                                           history.rows[record.row] == "Render draw"
                                       ? 1
                                       : 0;
                            scene += gpu && info.name == "3D scene" ? 1 : 0;
                            pass += gpu && info.name == "Geometry" ? 1 : 0;
                        }
                    }
                });
                profiler::release();
                profiler::set_gpu_detail(false);
            };
            int cpu = 0;
            int scene = 0;
            int pass = 0;
            run(false, cpu, scene, pass);
            Expect(rooted && misplaced == 0, "every 3D pass on the Render draw row is under 3D scene (" +
                                                 std::to_string(misplaced) + " misplaced)");
            Expect(cpu >= 6, "the geometry pass is timed on the CPU, on the Render draw row (" + std::to_string(cpu) + ")");
            Expect(scene >= 3 && pass == 0, "by default the GPU is timed once a frame, as 3D scene (" +
                                                std::to_string(scene) + " frames, " + std::to_string(pass) + " passes)");
            run(true, cpu, scene, pass);
            Expect(pass >= 3 && scene == 0, "with detail, each pass is timed on the GPU instead (" + std::to_string(pass) +
                                                " passes, " + std::to_string(scene) + " frames)");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "timing leaves no GL error");
        }
        Expect(!IsClear(middle), "the cube covers the middle of the view (" + Text(middle) + ")");
        Expect(IsClear(corner), "the corner is the clear color (" + Text(corner) + ")");
        // With no lights, only the ambient: every face the same gray.
        // From (0, 3, 7) the top face is 4 to 9 of 64 half-height pixels above the
        // middle, and the front face runs from 4 above to 10 below.
        const int topY = fbHeight / 2 + fbHeight * 3 / 64;
        const int frontY = fbHeight / 2 - fbHeight * 3 / 64;
        const Pixel ambientTop = ReadPixel(fbWidth / 2, topY);
        const Pixel ambientFront = ReadPixel(fbWidth / 2, frontY);
        Expect(Sum(ambientTop) > 0 && std::abs(Sum(ambientTop) - Sum(ambientFront)) <= 3,
               "the ambient lights every face alike (" + Text(ambientTop) + " and " + Text(ambientFront) + ")");
        // A PointLight above: the top face is brighter than the front face, and both are gray.
        runner::LightDraw above;
        above.position[1] = 3.f;
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &above, 1);
        const Pixel top = ReadPixel(fbWidth / 2, topY);
        const Pixel front = ReadPixel(fbWidth / 2, frontY);
        Expect(top.r > front.r && top.r == top.g && top.g == top.b, "a light above lights the top face brighter (" +
                                                                        Text(top) + " over " + Text(front) + ")");
        Expect(std::abs(Sum(front) - Sum(ambientFront)) <= 3, "and leaves the face turned from it at the ambient");
        Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "drawing leaves no GL error");

        // Moved by its Transform: 3 units right leaves the middle clear.
        runner::MeshDraw moved{cube, engine_core::matrix4_translation(3.f, 0.f, 0.f)};
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &moved, 1);
        Expect(IsClear(ReadPixel(fbWidth / 2, fbHeight / 2)), "a GameObject moved right leaves the middle");
        // x = 3 at that depth is about 0.68 of the half-width right of the middle.
        Expect(!IsClear(ReadPixel(fbWidth * 27 / 32, fbHeight / 2)), "and draws to the right of it");

        // A new place's Camera sees just what the Renderer's own camera does.
        const engine_core::Vec3 up{0.f, 1.f, 0.f};
        runner::ViewPixels fixed;
        runner::ViewPixels followed;
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
        renderer.read(0, 0, kSize, kSize, kSize, kSize, fixed);
        renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                           static_cast<float>(engine_core::Camera::kNewPlaceFieldOfView));
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
        renderer.read(0, 0, kSize, kSize, kSize, kSize, followed);
        Expect(!fixed.empty() && fixed.rgba == followed.rgba, "a new place's Camera draws what the fixed camera drew");

        // A Camera's Transform and FieldOfView. Turned away from the origin, it sees nothing.
        renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 3.f, 14.f}, up), 60.f);
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
        Expect(IsClear(ReadPixel(fbWidth / 2, fbHeight / 2)), "a Camera turned away does not see the cube");
        // Moved 3 right and looking straight ahead, it sees the moved cube in the middle.
        renderer.setCamera(engine_core::matrix4_look_at({3.f, 0.f, 7.f}, {3.f, 0.f, 0.f}, up), 60.f);
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &moved, 1);
        Expect(!IsClear(ReadPixel(fbWidth / 2, fbHeight / 2)), "a Camera over the moved cube sees it in the middle");
        Expect(IsClear(ReadPixel(fbWidth * 7 / 8, fbHeight / 2)), "and at 60 degrees the cube stays in the middle");
        // A narrow FieldOfView magnifies: the cube reaches out to the same pixel.
        renderer.setCamera(engine_core::matrix4_look_at({3.f, 0.f, 7.f}, {3.f, 0.f, 0.f}, up), 10.f);
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &moved, 1);
        Expect(!IsClear(ReadPixel(fbWidth * 7 / 8, fbHeight / 2)), "a narrow FieldOfView fills more of the view");
        // A Transform with no inverse, or an angle out of range, changes nothing.
        renderer.setCamera(engine_core::Matrix4{}, 60.f);
        renderer.setCamera(engine_core::matrix4_identity(), 0.f);
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &moved, 1);
        Expect(!IsClear(ReadPixel(fbWidth * 7 / 8, fbHeight / 2)), "a bad Camera is ignored");
        renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                           runner::Renderer::kCameraFovYDegrees);

        // A draw leaves each texture unit as it found it, as JadeFX's occluder
        // on unit 7 needs, except a name the draw itself deleted.
        {
            const auto boundAt = [](int unit) {
                runner::GLint active = 0;
                runner::GLint texture = 0;
                glGetIntegerv(runner::GL_ACTIVE_TEXTURE, &active);
                glActiveTexture(runner::GL_TEXTURE0 + static_cast<runner::GLenum>(unit));
                glGetIntegerv(runner::GL_TEXTURE_BINDING_2D, &texture);
                glActiveTexture(static_cast<runner::GLenum>(active));
                return static_cast<runner::GLuint>(texture);
            };
            runner::GLuint held = 0;
            glGenTextures(1, &held);
            glActiveTexture(runner::GL_TEXTURE0 + 7);
            glBindTexture(runner::GL_TEXTURE_2D, held);
            glActiveTexture(runner::GL_TEXTURE0);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
            runner::GLint active = 0;
            glGetIntegerv(runner::GL_ACTIVE_TEXTURE, &active);
            Expect(boundAt(7) == held && active == static_cast<runner::GLint>(runner::GL_TEXTURE0),
                   "a draw leaves unit 7's texture bound, and unit 0 active");
            // The scene's depth bound to unit 7, then a draw at another size,
            // which deletes it: the unit is left empty, not bound to a dead name.
            const runner::GLuint depth = renderer.sceneDepth().texture;
            glActiveTexture(runner::GL_TEXTURE0 + 7);
            glBindTexture(runner::GL_TEXTURE_2D, depth);
            glActiveTexture(runner::GL_TEXTURE0);
            renderer.draw(0, 0, kSize / 2, kSize / 2, kSize, kSize, &draw, 1);
            Expect(depth != 0 && runner::rt_glGetError() == runner::GL_NO_ERROR,
                   "a resize that deletes a bound depth texture leaves no GL error");
            Expect(boundAt(7) == 0, "and leaves its unit empty");
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
            glDeleteTextures(1, &held);
        }

        // The legacy pipeline's lights, glow, see-through surfaces, and tone map.
        {
            const int midX = fbWidth / 2;
            const int midY = fbHeight / 2;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
            const Pixel unlit = ReadPixel(midX, midY);

            // A PointLight in front of the cube, within its Radius, lights the front face.
            runner::LightDraw point;
            point.position[2] = 3.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &point, 1);
            const Pixel pointLit = ReadPixel(midX, midY);
            Expect(Sum(pointLit) > Sum(unlit) + 30, "a PointLight lights the face toward it (" + Text(pointLit) +
                                                        " over " + Text(unlit) + ")");
            runner::LightDraw shortReach = point;
            shortReach.radius = 1.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &shortReach, 1);
            Expect(std::abs(Sum(ReadPixel(midX, midY)) - Sum(unlit)) <= 3, "a light does not reach past its Radius");
            runner::LightDraw dark = point;
            dark.intensity = 0.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &dark, 1);
            Expect(std::abs(Sum(ReadPixel(midX, midY)) - Sum(unlit)) <= 3, "a light of Intensity 0 gives none");
            runner::LightDraw red = point;
            red.color[1] = 0.f;
            red.color[2] = 0.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &red, 1);
            const Pixel redLit = ReadPixel(midX, midY);
            Expect(redLit.r > redLit.g + 20 && redLit.g == redLit.b, "a light's Color tints it (" + Text(redLit) + ")");
            // Two lights add.
            const runner::LightDraw pair[2] = {point, point};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, pair, 2);
            Expect(Sum(ReadPixel(midX, midY)) > Sum(pointLit), "two lights are brighter than one");

            // A SpotLight lights what its cone points at, and nothing behind it.
            runner::LightDraw spot = point;
            spot.kind = runner::LightDraw::Kind::Spot;
            spot.direction[2] = -1.f;
            spot.outerFovDegrees = 60.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &spot, 1);
            Expect(Sum(ReadPixel(midX, midY)) > Sum(unlit) + 30, "a SpotLight lights what it points at");
            spot.direction[2] = 1.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &spot, 1);
            Expect(std::abs(Sum(ReadPixel(midX, midY)) - Sum(unlit)) <= 3, "and not what is behind it");
            // Pointed straight down from above the cube's front edge, a narrow cone
            // misses the front face entirely.
            runner::LightDraw down = spot;
            down.position[1] = 3.f;
            down.position[2] = 0.f;
            down.direction[1] = -1.f;
            down.direction[2] = 0.f;
            down.outerFovDegrees = 10.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &down, 1);
            Expect(std::abs(Sum(ReadPixel(midX, frontY)) - Sum(ambientFront)) <= 3,
                   "a narrow cone leaves outside it unlit");
            Expect(Sum(ReadPixel(midX, topY)) > Sum(ambientTop) + 30, "and lights inside it");

            // A DirectionalLight shining straight down lights the top face and not the front.
            runner::LightDraw sun;
            sun.kind = runner::LightDraw::Kind::Directional;
            sun.direction[0] = 0.f;
            sun.direction[1] = -1.f;
            sun.direction[2] = 0.f;
            sun.radius = 0.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &sun, 1);
            const Pixel sunTop = ReadPixel(midX, topY);
            Expect(Sum(sunTop) > Sum(ambientTop) + 60, "a DirectionalLight needs no Radius and lights the face toward it (" +
                                                            Text(sunTop) + ")");
            Expect(std::abs(Sum(ReadPixel(midX, frontY)) - Sum(ambientFront)) <= 3,
                   "and leaves a face square to it at the ambient");
            // Where it is does not matter, only where it points.
            runner::LightDraw farSun = sun;
            farSun.position[0] = 500.f;
            farSun.position[1] = -40.f;
            farSun.position[2] = 90.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &farSun, 1);
            Expect(std::abs(Sum(ReadPixel(midX, topY)) - Sum(sunTop)) <= 3, "a DirectionalLight's position is ignored");
            runner::LightDraw upSun = sun;
            upSun.direction[1] = 1.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &upSun, 1);
            Expect(std::abs(Sum(ReadPixel(midX, topY)) - Sum(ambientTop)) <= 3,
                   "a DirectionalLight shining up leaves the top face unlit");
            // Toward the camera's side, it lights the front face, and adds to a PointLight.
            runner::LightDraw frontSun = sun;
            frontSun.direction[1] = 0.f;
            frontSun.direction[2] = -1.f;
            const runner::LightDraw sunAndPoint[2] = {point, frontSun};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, sunAndPoint, 2);
            Expect(Sum(ReadPixel(midX, midY)) > Sum(pointLit) + 30, "a DirectionalLight adds to the other lights");
            // See-through surfaces take it too.
            runner::MeshDraw clearCube = draw;
            clearCube.transparency = 0.3f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &clearCube, 1);
            const Pixel clearUnlit = ReadPixel(midX, topY);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &clearCube, 1, &sun, 1);
            Expect(Sum(ReadPixel(midX, topY)) > Sum(clearUnlit) + 30, "a DirectionalLight lights see-through surfaces");

            // Emissive glows with no light at all.
            runner::MeshDraw glowing = draw;
            glowing.emissive[0] = 1.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &glowing, 1);
            const Pixel glow = ReadPixel(midX, midY);
            // Green and blue gain nothing (Saturation above 1 even pulls them down a little).
            Expect(glow.r > unlit.r + 60 && glow.g <= unlit.g + 3 && glow.g == glow.b,
                   "Emissive glows (" + Text(glow) + ")");
            // Emissive scales an EmissiveTexture: white leaves the glow as it
            // is, black puts it out, on opaque and see-through surfaces alike.
            {
                const auto solid = [](std::uint8_t value) {
                    const std::uint8_t texel[4] = {value, value, value, 255};
                    runner::GLuint texture = 0;
                    glGenTextures(1, &texture);
                    glBindTexture(runner::GL_TEXTURE_2D, texture);
                    glTexImage2D(runner::GL_TEXTURE_2D, 0, static_cast<runner::GLint>(runner::GL_RGBA8), 1, 1, 0,
                                 runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, texel);
                    glTexParameteri(runner::GL_TEXTURE_2D, runner::GL_TEXTURE_MIN_FILTER,
                                    static_cast<runner::GLint>(runner::RT_GL_NEAREST));
                    glTexParameteri(runner::GL_TEXTURE_2D, runner::GL_TEXTURE_MAG_FILTER,
                                    static_cast<runner::GLint>(runner::RT_GL_NEAREST));
                    glBindTexture(runner::GL_TEXTURE_2D, 0);
                    return texture;
                };
                const runner::GLuint white = solid(255);
                const runner::GLuint black = solid(0);
                runner::MeshDraw mapped = glowing;
                mapped.emissiveTexture = white;
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, &mapped, 1);
                Expect(std::abs(Sum(ReadPixel(midX, midY)) - Sum(glow)) <= 3, "a white EmissiveTexture keeps the glow");
                mapped.emissiveTexture = black;
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, &mapped, 1);
                const Pixel masked = ReadPixel(midX, midY);
                Expect(std::abs(Sum(masked) - Sum(unlit)) <= 3,
                       "a black EmissiveTexture puts the glow out (" + Text(masked) + ")");
                runner::MeshDraw clearGlow = glowing;
                clearGlow.transparency = 0.3f;
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, &clearGlow, 1);
                const Pixel clearGlowing = ReadPixel(midX, midY);
                clearGlow.emissiveTexture = black;
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, &clearGlow, 1);
                Expect(Sum(ReadPixel(midX, midY)) + 30 < Sum(clearGlowing),
                       "and on a see-through surface (" + Text(clearGlowing) + ")");
                glDeleteTextures(1, &white);
                glDeleteTextures(1, &black);
            }

            // Exposure and Saturation, from Lighting.
            runner::SceneLighting dim;
            dim.exposure = 0.4f;
            renderer.setLighting(dim);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &point, 1);
            Expect(Sum(ReadPixel(midX, midY)) + 20 < Sum(pointLit), "a lower Exposure is darker");
            runner::SceneLighting gray;
            gray.saturation = 0.f;
            renderer.setLighting(gray);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1, &red, 1);
            const Pixel grayLit = ReadPixel(midX, midY);
            Expect(grayLit.r == grayLit.g && grayLit.g == grayLit.b, "Saturation 0 is gray (" + Text(grayLit) + ")");
            renderer.setLighting(runner::SceneLighting{});
            // Bloom. A chain made this frame may not draw until the next on
            // macOS, so each look draws twice and reads the second.
            {
                const auto drawTwice = [&](const runner::MeshDraw& mesh) {
                    for (int pass = 0; pass < 2; ++pass) {
                        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &mesh, 1);
                    }
                };
                const auto snap = [&] {
                    runner::ViewPixels pixels;
                    renderer.read(0, 0, kSize, kSize, kSize, kSize, pixels);
                    return pixels.rgba;
                };
                // These read pixels at the cube's edge, which FXAA smooths, so
                // they measure bloom with hard edges.
                runner::SceneLighting hardEdges;
                hardEdges.antialiasing = runner::SceneAntialiasing::None;
                renderer.setLighting(hardEdges);
                // Bright, but under the tone map's white, so moving light out of it shows.
                runner::MeshDraw bright = draw;
                bright.emissive[0] = bright.emissive[1] = bright.emissive[2] = 1.0f;
                drawTwice(bright);
                const std::vector<unsigned char> plain = snap();
                // The cube's right edge along the middle row, and pixels just inside and past it.
                int edge = fbWidth / 2;
                while (edge < fbWidth - 1 && !IsClear(ReadPixel(edge + 1, midY))) {
                    ++edge;
                }
                const int outside = std::min(edge + 3, fbWidth - 1);
                const Pixel plainInside = ReadPixel(edge - 1, midY);
                Expect(edge < fbWidth - 8 && IsClear(ReadPixel(outside, midY)),
                       "without bloom the pane is clear past the cube's edge");

                runner::SceneLighting haze = hardEdges;
                haze.bloom.enabled = true;
                haze.bloom.intensity = 0.5f;
                haze.bloom.size = 56.f;
                renderer.setLighting(haze);
                const bool first = renderer.draw(0, 0, kSize, kSize, kSize, kSize, &bright, 1);
                Expect(first, "the first frame with bloom still draws, with or without the bloom");
                drawTwice(bright);
                const Pixel halo = ReadPixel(outside, midY);
                Expect(!IsClear(halo) && Sum(halo) > 90 + 6,
                       "bloom spreads light past a bright cube's edge (" + Text(halo) + ")");
                Expect(Sum(ReadPixel(edge - 1, midY)) < Sum(plainInside),
                       "at Threshold 0 the light moves out of the cube, not only added (" +
                           Text(ReadPixel(edge - 1, midY)) + " was " + Text(plainInside) + ")");
                Expect(IsClear(ReadPixel(2, 2)), "and the far corner stays the clear color");

                // Off three ways, the frame is exactly the one without bloom.
                for (int way = 0; way < 3; ++way) {
                    runner::SceneLighting off = haze;
                    if (way == 0) {
                        off.bloom.enabled = false;
                    } else if (way == 1) {
                        off.bloom.intensity = 0.f;
                    } else {
                        off.bloom.size = 0.f;
                    }
                    renderer.setLighting(off);
                    drawTwice(bright);
                    Expect(snap() == plain, "Enabled false, Intensity 0, or Size 0 draws no bloom (way " +
                                                std::to_string(way) + ")");
                }

                // Above a Threshold, only bright light blooms.
                runner::SceneLighting glowOnly = haze;
                glowOnly.bloom.threshold = 1.f;
                renderer.setLighting(glowOnly);
                drawTwice(draw);
                Expect(IsClear(ReadPixel(outside, midY)), "with a Threshold, a dim cube gets no halo");
                runner::MeshDraw hot = draw;
                hot.emissive[0] = hot.emissive[1] = hot.emissive[2] = 4.f;
                drawTwice(hot);
                Expect(!IsClear(ReadPixel(outside, midY)), "and a bright one still does");

                // Light past half float's range is stored as infinity. Bloom must
                // not spread that into a black or invalid patch around it.
                runner::MeshDraw blinding = draw;
                blinding.emissive[0] = blinding.emissive[1] = blinding.emissive[2] = 1e5f;
                renderer.setLighting(haze);
                drawTwice(blinding);
                const Pixel blindingHalo = ReadPixel(outside, midY);
                const Pixel blindingInside = ReadPixel(edge - 1, midY);
                Expect(!IsClear(blindingHalo) && Sum(blindingHalo) > 90 + 6 && Sum(blindingInside) > 600,
                       "an overflowing highlight still blooms, not a dark patch (halo " + Text(blindingHalo) +
                           ", inside " + Text(blindingInside) + ")");

                // A pane of another size makes the chain again; one too small for a level skips bloom quietly.
                renderer.setLighting(haze);
                for (int pass = 0; pass < 2; ++pass) {
                    renderer.draw(0, 0, kSize / 2.0, kSize / 2.0, kSize, kSize, &bright, 1);
                }
                for (int pass = 0; pass < 2; ++pass) {
                    renderer.draw(0, 0, 3, 3, fbWidth, fbHeight, &bright, 1);
                }
                drawTwice(bright);
                Expect(!IsClear(ReadPixel(outside, midY)), "after the pane changes size, it still blooms");
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "bloom leaves no GL error");
                renderer.setLighting(runner::SceneLighting{});
            }
            // FXAA. A cube rolled 30 degrees has slanted edges; FXAA softens
            // the steps along them and leaves flat areas exactly as they were.
            {
                const auto drawTwice = [&](const runner::MeshDraw& mesh) {
                    for (int pass = 0; pass < 2; ++pass) {
                        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &mesh, 1);
                    }
                };
                const auto snap = [&] {
                    runner::ViewPixels pixels;
                    renderer.read(0, 0, kSize, kSize, kSize, kSize, pixels);
                    return pixels.rgba;
                };
                // The largest step between neighbours along each row through the cube, averaged.
                const auto sharpness = [&] {
                    long total = 0;
                    int rows = 0;
                    for (int y = fbHeight * 3 / 10; y < fbHeight * 7 / 10; y += 2) {
                        int largest = 0;
                        Pixel previous = ReadPixel(0, y);
                        for (int x = 1; x < fbWidth; ++x) {
                            const Pixel here = ReadPixel(x, y);
                            largest = std::max(largest, std::abs(Sum(here) - Sum(previous)));
                            previous = here;
                        }
                        total += largest;
                        ++rows;
                    }
                    return static_cast<double>(total) / rows;
                };
                runner::MeshDraw rolled = glowing;
                rolled.model = engine_core::matrix4_axis_angle({0.f, 0.f, 1.f}, 30.0 * 3.14159265358979 / 180.0);
                runner::SceneLighting hard;
                hard.antialiasing = runner::SceneAntialiasing::None;
                runner::SceneLighting smooth;
                smooth.antialiasing = runner::SceneAntialiasing::FXAA;

                renderer.setLighting(hard);
                drawTwice(rolled);
                const std::vector<unsigned char> hardPixels = snap();
                const double hardSharpness = sharpness();
                const Pixel hardMiddle = ReadPixel(midX, midY);

                renderer.setLighting(smooth);
                const bool first = renderer.draw(0, 0, kSize, kSize, kSize, kSize, &rolled, 1);
                Expect(first && !IsClear(ReadPixel(midX, midY)), "the first frame with FXAA draws the cube");
                drawTwice(rolled);
                const double smoothSharpness = sharpness();
                Expect(smoothSharpness < hardSharpness * 0.85,
                       "FXAA softens the steps along slanted edges (" + std::to_string(smoothSharpness) + " vs " +
                           std::to_string(hardSharpness) + ")");
                Expect(Sum(ReadPixel(midX, midY)) == Sum(hardMiddle), "and leaves the cube's flat middle as it was");
                Expect(IsClear(ReadPixel(2, 2)), "and the empty corner exactly the clear color");

                // Back and forth: each mode draws what a fresh draw of it draws.
                renderer.setLighting(hard);
                drawTwice(rolled);
                Expect(snap() == hardPixels, "switching back to None draws exactly the hard edges again");

                // A see-through cube over the empty pane blends the same with and without FXAA in its middle.
                runner::MeshDraw seeThrough = glowing;
                seeThrough.transparency = 0.5f;
                drawTwice(seeThrough);
                const Pixel hardClear = ReadPixel(midX, midY);
                renderer.setLighting(smooth);
                drawTwice(seeThrough);
                Expect(Sum(ReadPixel(midX, midY)) == Sum(hardClear),
                       "a see-through surface blends over the pane the same with FXAA (" +
                           Text(ReadPixel(midX, midY)) + " vs " + Text(hardClear) + ")");

                // The grid draws after FXAA, so it is identical with and without it.
                renderer.setGridVisible(true);
                renderer.setLighting(hard);
                drawTwice(rolled);
                std::vector<Pixel> hardGrid;
                for (int x = 0; x < fbWidth; ++x) {
                    hardGrid.push_back(ReadPixel(x, 3));
                }
                renderer.setLighting(smooth);
                drawTwice(rolled);
                bool gridSame = true;
                for (int x = 0; x < fbWidth; ++x) {
                    const Pixel p = ReadPixel(x, 3);
                    gridSame = gridSame && p.r == hardGrid[x].r && p.g == hardGrid[x].g && p.b == hardGrid[x].b;
                }
                Expect(gridSame, "the grid near the bottom edge is the same with and without FXAA");
                renderer.setGridVisible(false);

                // Under a parent's clip, FXAA draws only inside it.
                glClearColor(0.f, 0.f, 1.f, 1.f);
                glClear(runner::GL_COLOR_BUFFER_BIT);
                glEnable(runner::GL_SCISSOR_TEST);
                glScissor(0, 0, fbWidth / 2, fbHeight);
                for (int pass = 0; pass < 2; ++pass) {
                    renderer.draw(0, 0, kSize, kSize, kSize, kSize, &rolled, 1);
                }
                glDisable(runner::GL_SCISSOR_TEST);
                const Pixel outsideClip = ReadPixel(fbWidth * 3 / 4, fbHeight / 2);
                Expect(outsideClip.b == 255 && outsideClip.r == 0, "FXAA leaves what is outside the parent's clip alone (" +
                                                                       Text(outsideClip) + ")");

                // Another size makes the target again.
                for (int pass = 0; pass < 2; ++pass) {
                    renderer.draw(0, 0, kSize / 2.0, kSize / 2.0, kSize, kSize, &rolled, 1);
                }
                drawTwice(rolled);
                Expect(!IsClear(ReadPixel(midX, midY)), "after the pane changes size, FXAA still draws the cube");
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "FXAA leaves no GL error");
                renderer.setLighting(runner::SceneLighting{});
            }

            // Screen-space reflections: a mirror floor under a red glowing cube.
            {
                runner::SceneLighting plain;
                plain.antialiasing = runner::SceneAntialiasing::None;
                const auto drawScene = [&](const runner::MeshDraw* scene, int count) {
                    for (int pass = 0; pass < 2; ++pass) {
                        renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, count);
                    }
                };
                const auto snap = [&] {
                    runner::ViewPixels pixels;
                    renderer.read(0, 0, kSize, kSize, kSize, kSize, pixels);
                    return pixels.rgba;
                };
                runner::MeshDraw floor = draw;
                // A wide flat slab, its top at y = -0.6, just under the cube.
                floor.model = engine_core::matrix4_identity();
                floor.model.m[0] = 8.f;
                floor.model.m[5] = 0.1f;
                floor.model.m[10] = 8.f;
                floor.model.m[13] = -0.65f;
                floor.metalness = 1.f;
                floor.roughness = 0.f;
                floor.color[0] = floor.color[1] = floor.color[2] = 0.9f;
                runner::MeshDraw red = draw;
                red.emissive[0] = 3.f;
                red.color[1] = red.color[2] = 0.f;
                const runner::MeshDraw scene[] = {floor, red};

                renderer.setLighting(plain);
                drawScene(scene, 2);
                const std::vector<unsigned char> unreflected = snap();
                const auto redAt = [](const std::vector<unsigned char>& rgba, int index) {
                    return static_cast<int>(rgba[index * 4]) - static_cast<int>(rgba[index * 4 + 2]);
                };

                runner::SceneLighting mirror = plain;
                mirror.reflections.enabled = true;
                renderer.setLighting(mirror);
                const bool first = renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, 2);
                Expect(first, "the first frame with reflections still draws");
                drawScene(scene, 2);
                const std::vector<unsigned char> reflected = snap();
                // Rows are top first. Count floor pixels below the cube that turned redder,
                // and pixels in the top quarter (sky and cube top) that changed at all.
                const int width = static_cast<int>(std::sqrt(reflected.size() / 4));
                int redder = 0;
                int changedAbove = 0;
                for (int i = 0; i < static_cast<int>(reflected.size() / 4); ++i) {
                    const int row = i / width;
                    if (row > width * 6 / 10 && redAt(reflected, i) > redAt(unreflected, i) + 20) {
                        ++redder;
                    }
                    if (row < width / 4 && std::memcmp(&reflected[i * 4], &unreflected[i * 4], 3) != 0) {
                        ++changedAbove;
                    }
                }
                Expect(redder > 20, "the mirror floor below the cube reflects its red (" + std::to_string(redder) +
                                        " pixels)");
                Expect(changedAbove == 0, "nothing above the floor changes (" + std::to_string(changedAbove) + ")");
                // The reflection is solid, not stippled: on each row through it, few
                // pixels between its first and last redder pixel miss.
                int inside = 0;
                int holes = 0;
                for (int row = width * 6 / 10 + 1; row < width; ++row) {
                    int first = -1;
                    int last = -1;
                    for (int x = 0; x < width; ++x) {
                        if (redAt(reflected, row * width + x) > redAt(unreflected, row * width + x) + 20) {
                            first = first < 0 ? x : first;
                            last = x;
                        }
                    }
                    for (int x = first; first >= 0 && x <= last; ++x) {
                        ++inside;
                        holes += redAt(reflected, row * width + x) > redAt(unreflected, row * width + x) + 20 ? 0 : 1;
                    }
                }
                Expect(inside > 0 && holes * 10 <= inside, "the reflection has no stipple of missed rays (" +
                                                               std::to_string(holes) + " of " + std::to_string(inside) +
                                                               " missed)");

                // A rough floor reads the blurred levels of the lit image: under
                // a red cube beside a green one, its reflection takes in some of
                // the green, where the mirror's stays red.
                {
                    runner::MeshDraw left = red;
                    left.model = engine_core::matrix4_translation(-0.5f, 0.f, 0.f);
                    left.emissive[0] = 1.f;
                    runner::MeshDraw right = draw;
                    right.model = engine_core::matrix4_translation(0.5f, 0.f, 0.f);
                    right.color[0] = right.color[2] = 0.f;
                    right.emissive[1] = 1.f;
                    // The green that floor's reflection adds under the red cube, the left half's floor.
                    const auto greenUnderRed = [&](float roughness) {
                        runner::MeshDraw pairFloor = floor;
                        pairFloor.roughness = roughness;
                        const runner::MeshDraw pair[] = {pairFloor, left, right};
                        runner::SceneLighting glossy = mirror;
                        glossy.reflections.maxRoughness = 1.f;
                        renderer.setLighting(plain);
                        drawScene(pair, 3);
                        const std::vector<unsigned char> without = snap();
                        renderer.setLighting(glossy);
                        drawScene(pair, 3);
                        const std::vector<unsigned char> with = snap();
                        int green = 0;
                        for (int row = width * 6 / 10 + 1; row < width; ++row) {
                            for (int x = 0; x < width / 2 - 2; ++x) {
                                const int i = (row * width + x) * 4 + 1;
                                green += std::max(static_cast<int>(with[i]) - static_cast<int>(without[i]), 0);
                            }
                        }
                        return green;
                    };
                    const int sharp = greenUnderRed(0.f);
                    const int blurred = greenUnderRed(0.3f);
                    // Plain box-filtered mips blur about a fifth as much (920 to 5103 at 128 pixels).
                    Expect(blurred > sharp + 2500, "a rough floor's reflection blurs the cubes' colors together (" +
                                                         std::to_string(blurred) + " green under the red cube, against " +
                                                         std::to_string(sharp) + " for a mirror)");
                    Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "rough reflections leave no GL error");
                    renderer.setLighting(mirror);
                }

                // Off four ways, and a floor rougher than MaxRoughness: exactly the frame without reflections.
                for (int way = 0; way < 5; ++way) {
                    runner::SceneLighting off = mirror;
                    runner::MeshDraw changedScene[] = {floor, red};
                    if (way == 0) {
                        off.reflections.enabled = false;
                    } else if (way == 1) {
                        off.reflections.intensity = 0.f;
                    } else if (way == 2) {
                        off.reflections.maxDistance = 0.f;
                    } else if (way == 3) {
                        off = plain;
                    } else {
                        changedScene[0].roughness = 0.7f;
                    }
                    renderer.setLighting(off);
                    drawScene(changedScene, 2);
                    const std::vector<unsigned char> got = snap();
                    if (way == 4) {
                        renderer.setLighting(plain);
                        drawScene(changedScene, 2);
                        Expect(got == snap(), "a floor rougher than MaxRoughness draws as without reflections");
                    } else {
                        Expect(got == unreflected, "off draws exactly as without reflections (way " +
                                                       std::to_string(way) + ")");
                    }
                }

                // The cube far off screen: nothing to reflect, and no hole.
                runner::MeshDraw away = red;
                away.model = engine_core::matrix4_translation(200.f, 0.f, 0.f);
                const runner::MeshDraw lonely[] = {floor, away};
                renderer.setLighting(plain);
                drawScene(lonely, 2);
                const std::vector<unsigned char> lonelyPlain = snap();
                renderer.setLighting(mirror);
                drawScene(lonely, 2);
                Expect(snap() == lonelyPlain, "with nothing on screen to reflect, the floor keeps its sky reflection");

                // Glass over the floor: the reflection sits under it, so the glass's own color still shows.
                runner::MeshDraw glass = draw;
                glass.transparency = 0.5f;
                glass.color[0] = glass.color[1] = 0.f;
                glass.model = engine_core::matrix4_translation(0.f, -0.3f, 1.5f);
                const runner::MeshDraw covered[] = {floor, red, glass};
                drawScene(covered, 3);
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "reflections under glass leave no GL error");

                // Another size makes the buffers again.
                for (int pass = 0; pass < 2; ++pass) {
                    renderer.draw(0, 0, kSize / 2.0, kSize / 2.0, kSize, kSize, scene, 2);
                }
                drawScene(scene, 2);
                Expect(snap() == reflected, "after the pane changes size and back, it reflects the same");
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "reflections leave no GL error");
                renderer.setLighting(runner::SceneLighting{});
            }

            // Ambient occlusion: a cube resting on a wide floor slab.
            {
                runner::SceneLighting plain;
                plain.antialiasing = runner::SceneAntialiasing::None;
                // Occlusion shades only the ambient and sky light, which the stand-in
                // sky makes dim; brighter ambient gives the checks room to measure.
                plain.ambient[0] = plain.ambient[1] = plain.ambient[2] = 3.f;
                runner::MeshDraw floor = draw;
                floor.model = engine_core::matrix4_identity();
                floor.model.m[0] = 20.f;
                floor.model.m[5] = 0.1f;
                floor.model.m[10] = 20.f;
                floor.model.m[13] = -0.55f;
                // Mid-gray: multi-bounce rightly gives a white surface back most of
                // what occlusion takes, which would hide the shade.
                floor.color[0] = floor.color[1] = floor.color[2] = 0.5f;
                const runner::MeshDraw scene[] = {draw, floor};
                const auto drawScene = [&](const runner::MeshDraw* meshes, int count) {
                    for (int pass = 0; pass < 2; ++pass) {
                        renderer.draw(0, 0, kSize, kSize, kSize, kSize, meshes, count);
                    }
                };
                const auto snap = [&] {
                    runner::ViewPixels pixels;
                    renderer.read(0, 0, kSize, kSize, kSize, kSize, pixels);
                    return pixels.rgba;
                };
                // Where a world point lands, with the default camera, in framebuffer pixels.
                const engine_core::Vec3 up{0.f, 1.f, 0.f};
                const engine_core::Matrix4 view = engine_core::matrix4_inverse(engine_core::matrix4_look_at(
                    {runner::Renderer::kCameraEye[0], runner::Renderer::kCameraEye[1], runner::Renderer::kCameraEye[2]},
                    {0.f, 0.f, 0.f}, up));
                const float focal = 1.f / std::tan(0.5f * runner::Renderer::kCameraFovYDegrees * 0.01745329252f);
                const auto at = [&](float x, float y, float z) {
                    const engine_core::Vec3 p = engine_core::matrix4_point(view, {x, y, z});
                    const int px = static_cast<int>((focal * p.x / -p.z * 0.5f + 0.5f) * static_cast<float>(fbWidth));
                    const int py = static_cast<int>((focal * p.y / -p.z * 0.5f + 0.5f) * static_cast<float>(fbHeight));
                    return ReadPixel(px, py);
                };
                renderer.setLighting(plain);
                drawScene(scene, 2);
                const std::vector<unsigned char> unshaded = snap();
                const Pixel contactOff = at(0.f, -0.5f, 0.58f);
                const Pixel openOff = at(2.2f, -0.5f, 2.2f);
                const Pixel pastTopOff = at(0.f, -0.5f, -4.f);

                runner::SceneLighting shaded = plain;
                shaded.occlusion.enabled = true;
                renderer.setLighting(shaded);
                const bool first = renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, 2);
                Expect(first, "the first frame with ambient occlusion still draws");
                drawScene(scene, 2);
                Expect(Sum(at(0.f, -0.5f, 0.58f)) + 10 < Sum(contactOff),
                       "the floor where the cube stands on it is shaded (" + Text(at(0.f, -0.5f, 0.58f)) + " vs " +
                           Text(contactOff) + ")");
                Expect(std::abs(Sum(at(2.2f, -0.5f, 2.2f)) - Sum(openOff)) <= 2,
                       "the open floor is not shaded (" + Text(at(2.2f, -0.5f, 2.2f)) + ")");
                Expect(std::abs(Sum(at(0.f, -0.5f, -4.f)) - Sum(pastTopOff)) <= 2,
                       "the floor seen just past the cube's top edge, far behind it, is not shaded (" +
                           Text(at(0.f, -0.5f, -4.f)) + ")");

                // Each Quality shades the contact; High and Medium agree on it.
                int contact[3] = {};
                for (int quality = 0; quality < 3; ++quality) {
                    runner::SceneLighting q = shaded;
                    q.occlusion.quality = static_cast<runner::SceneQuality>(quality);
                    renderer.setLighting(q);
                    drawScene(scene, 2);
                    contact[quality] = Sum(at(0.f, -0.5f, 0.58f));
                    Expect(contact[quality] + 10 < Sum(contactOff),
                           "Quality " + std::to_string(quality) + " shades the contact");
                }
                Expect(std::abs(contact[2] - contact[1]) <= 12, "High and Medium agree on the contact (" +
                                                                    std::to_string(contact[2]) + " vs " +
                                                                    std::to_string(contact[1]) + ")");

                // Off four ways: exactly the frame without it.
                for (int way = 0; way < 4; ++way) {
                    runner::SceneLighting off = shaded;
                    if (way == 0) {
                        off.occlusion.enabled = false;
                    } else if (way == 1) {
                        off.occlusion.intensity = 0.f;
                    } else if (way == 2) {
                        off.occlusion.radius = 0.f;
                    } else {
                        off = plain;
                    }
                    renderer.setLighting(off);
                    drawScene(scene, 2);
                    Expect(snap() == unshaded, "off draws exactly as without occlusion (way " + std::to_string(way) + ")");
                }

                // A camera almost at the floor: no black (NaN) pixels that were not black without it.
                renderer.setCamera(engine_core::matrix4_look_at({0.f, -0.45f, 1.5f}, {0.f, -0.5f, 0.f}, up),
                                   runner::Renderer::kCameraFovYDegrees);
                renderer.setLighting(plain);
                drawScene(scene, 2);
                const std::vector<unsigned char> closeOff = snap();
                renderer.setLighting(shaded);
                drawScene(scene, 2);
                const std::vector<unsigned char> closeOn = snap();
                int newBlack = 0;
                for (std::size_t i = 0; i + 3 < closeOn.size(); i += 4) {
                    const bool black = closeOn[i] == 0 && closeOn[i + 1] == 0 && closeOn[i + 2] == 0;
                    const bool wasBlack = closeOff[i] == 0 && closeOff[i + 1] == 0 && closeOff[i + 2] == 0;
                    newBlack += black && !wasBlack ? 1 : 0;
                }
                Expect(newBlack == 0, "a camera almost at a surface leaves no black pixels (" +
                                          std::to_string(newBlack) + ")");
                renderer.setCamera(engine_core::matrix4_inverse(view), runner::Renderer::kCameraFovYDegrees);

                // With SSR too, a mirror floor where rays find nothing matches occlusion alone.
                runner::MeshDraw mirror = floor;
                mirror.metalness = 1.f;
                mirror.roughness = 0.f;
                const runner::MeshDraw mirrored[] = {draw, mirror};
                renderer.setLighting(shaded);
                drawScene(mirrored, 2);
                const Pixel besideAlone = at(-0.62f, -0.5f, 0.f);
                runner::SceneLighting both = shaded;
                both.reflections.enabled = true;
                renderer.setLighting(both);
                drawScene(mirrored, 2);
                const Pixel besideBoth = at(-0.62f, -0.5f, 0.f);
                Expect(std::abs(Sum(besideBoth) - Sum(besideAlone)) <= 1,
                       "where SSR misses, the floor matches occlusion alone (" + Text(besideBoth) + " vs " +
                           Text(besideAlone) + ")");

                // Size and Quality changes make the buffers again.
                renderer.setLighting(shaded);
                for (int pass = 0; pass < 2; ++pass) {
                    renderer.draw(0, 0, kSize / 2.0, kSize / 2.0, kSize, kSize, scene, 2);
                }
                runner::SceneLighting high = shaded;
                high.occlusion.quality = runner::SceneQuality::High;
                renderer.setLighting(high);
                drawScene(scene, 2);
                renderer.setLighting(shaded);
                drawScene(scene, 2);
                Expect(Sum(at(0.f, -0.5f, 0.58f)) + 10 < Sum(contactOff), "after size and Quality changes it still shades");
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "ambient occlusion leaves no GL error");
                renderer.setLighting(runner::SceneLighting{});
            }

            // Transparency 1 draws nothing; between 0 and 1 the pane shows through.
            runner::MeshDraw gone = draw;
            gone.transparency = 1.f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &gone, 1);
            Expect(IsClear(ReadPixel(midX, midY)), "Transparency 1 draws nothing");
            runner::MeshDraw half = glowing;
            half.transparency = 0.5f;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &half, 1);
            const Pixel halfOverClear = ReadPixel(midX, midY);
            Expect(!IsClear(halfOverClear) && halfOverClear.r < glow.r && halfOverClear.r > 30,
                   "half Transparency blends with the pane (" + Text(halfOverClear) + ")");
            Expect(IsClear(ReadPixel(2, 2)), "and the corner stays the clear color");
            // A half see-through red cube in front of an opaque green one: both
            // colors show. Blue is the baseline: the light's white highlight
            // adds to every channel alike, and neither cube has blue of its own.
            runner::MeshDraw greenCube = draw;
            greenCube.color[0] = 0.f;
            greenCube.color[2] = 0.f;
            runner::MeshDraw seeThrough = half;
            seeThrough.color[1] = 0.f;
            seeThrough.color[2] = 0.f;
            seeThrough.model = engine_core::matrix4_translation(0.f, 0.f, 1.5f);
            const runner::MeshDraw layered[2] = {seeThrough, greenCube};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, layered, 2, &point, 1);
            const Pixel mixed = ReadPixel(midX, midY);
            Expect(mixed.r > mixed.b + 30 && mixed.g > mixed.b + 30,
                   "a see-through surface blends over an opaque one behind it (" + Text(mixed) + ")");
            // Behind the opaque one, it is hidden: no red over the baseline.
            runner::MeshDraw hidden = seeThrough;
            hidden.model = engine_core::matrix4_translation(0.f, 0.f, -1.5f);
            const runner::MeshDraw occluded[2] = {hidden, greenCube};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, occluded, 2, &point, 1);
            const Pixel front2 = ReadPixel(midX, midY);
            Expect(front2.r <= front2.b + 2 && front2.g > front2.b + 30,
                   "the opaque surface hides one behind it (" + Text(front2) + ")");

            // The UI pass after a draw finds its GL state as it left it.
            runner::rt_glEnable(runner::GL_BLEND);
            runner::rt_glBlendFunc(runner::RT_GL_SRC_ALPHA, runner::RT_GL_ONE_MINUS_SRC_ALPHA);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, layered, 2, &point, 1);
            runner::GLint source = 0;
            runner::GLint framebuffer = -1;
            glGetIntegerv(runner::RT_GL_BLEND_SRC_RGB, &source);
            glGetIntegerv(runner::RT_GL_FRAMEBUFFER_BINDING, &framebuffer);
            Expect(runner::rt_glIsEnabled(runner::GL_BLEND) == runner::GL_TRUE &&
                       source == static_cast<runner::GLint>(runner::RT_GL_SRC_ALPHA) && framebuffer == 0,
                   "drawing restores blending and the framebuffer");
            runner::rt_glDisable(runner::GL_BLEND);
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the pipeline leaves no GL error");

        // Shadows. A floor under the cube, lit from the -X side and down, so
        // the cube's shadow falls on the floor at +X: (1, -0.5, 0) is in it,
        // and (1, -0.5, 2) is open floor at about the same distance.
        {
            engine_core::Matrix4 floorModel = engine_core::matrix4_identity();
            floorModel.m[0] = 8.f;
            floorModel.m[5] = 0.2f;
            floorModel.m[10] = 8.f;
            floorModel.m[13] = -0.6f;
            runner::MeshDraw scene[2] = {runner::MeshDraw{cube, engine_core::matrix4_identity()},
                                         runner::MeshDraw{cube, floorModel}};
            // Where a world point lands in the pane, through the camera a Renderer starts with.
            const auto pixelOf = [&](engine_core::Vec3 world) {
                const engine_core::Matrix4 view =
                    runner::LookAtView({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
                const engine_core::Matrix4 projection = runner::Perspective(
                    runner::Renderer::kCameraFovYDegrees,
                    static_cast<float>(fbWidth) / static_cast<float>(fbHeight), 0.1f, 1000.f);
                const engine_core::Vec3 ndc =
                    engine_core::matrix4_point(engine_core::matrix4_multiply(projection, view), world);
                return std::array<int, 2>{static_cast<int>((ndc.x * 0.5f + 0.5f) * fbWidth),
                                          static_cast<int>((ndc.y * 0.5f + 0.5f) * fbHeight)};
            };
            const std::array<int, 2> inShadow = pixelOf({1.f, -0.5f, 0.f});
            const std::array<int, 2> openFloor = pixelOf({1.f, -0.5f, 2.f});
            const auto sample = [&](const std::array<int, 2>& at) { return Sum(ReadPixel(at[0], at[1])); };
            // The two floor points' brightness under one light.
            const auto lit = [&](const runner::LightDraw& light, const runner::MeshDraw* meshes, int count) {
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, meshes, count, &light, 1);
                return std::array<int, 2>{sample(inShadow), sample(openFloor)};
            };
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, 2);
            const int ambientFloor = sample(inShadow);

            runner::LightDraw spotLight;
            spotLight.kind = runner::LightDraw::Kind::Spot;
            spotLight.position[0] = -4.f;
            spotLight.position[1] = 2.f;
            spotLight.position[2] = 0.f;
            spotLight.direction[0] = 0.894f;
            spotLight.direction[1] = -0.447f;
            spotLight.direction[2] = 0.f;
            spotLight.outerFovDegrees = 90.f;
            spotLight.radius = 12.f;
            spotLight.intensity = 4.f;
            spotLight.id = 7;
            const std::array<int, 2> spotOpen = lit(spotLight, scene, 2);
            Expect(spotOpen[0] > ambientFloor + 20, "with no shadow, the SpotLight reaches the floor behind the cube (" +
                                                        std::to_string(spotOpen[0]) + " over " +
                                                        std::to_string(ambientFloor) + ")");
            spotLight.shadows = true;
            const std::array<int, 2> spotShadowed = lit(spotLight, scene, 2);
            Expect(std::abs(spotShadowed[0] - ambientFloor) <= 4,
                   "a SpotLight's shadow leaves the floor behind the cube at the ambient (" +
                       std::to_string(spotShadowed[0]) + ")");
            Expect(std::abs(spotShadowed[1] - spotOpen[1]) <= 3,
                   "and the open floor lit as before, with no acne (" + std::to_string(spotShadowed[1]) + " and " +
                       std::to_string(spotOpen[1]) + ")");

            // SH1: the spot's one tile draws its casters, all one mesh, in one call,
            // and a frame that reuses the tile draws none. Visible: the cube and the
            // floor, two runs of slot 0.
            runner::LightDraw cachedSpot = spotLight;
            cachedSpot.id = 31;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, 2, &cachedSpot, 1);
            const int firstCalls = renderer.stats().instancedCalls;
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, 2, &cachedSpot, 1);
            const int reusedCalls = renderer.stats().instancedCalls;
            Expect(firstCalls == 3 && reusedCalls == 2,
                   "SH1 one shadow call for the tile, none when it is reused (" + std::to_string(firstCalls) +
                       " then " + std::to_string(reusedCalls) + ")");

            Expect(lit(spotLight, scene, 2) == spotShadowed, "a second frame reuses the cached map and draws the same");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "drawing shadows leaves no GL error");

            // Moving the cube away moves its shadow: the cache sees the change.
            runner::MeshDraw movedScene[2] = {runner::MeshDraw{cube, engine_core::matrix4_translation(0.f, 0.f, -3.f)},
                                              scene[1]};
            Expect(std::abs(lit(spotLight, movedScene, 2)[0] - spotOpen[0]) <= 4, "moving the cube moves its shadow");
            Expect(std::abs(lit(spotLight, scene, 2)[0] - ambientFloor) <= 4, "and moving it back brings it back");

            // Geometry uploaded again in place, as MeshCache does when a mesh
            // file changes: the same GpuMesh, so the same address, but the
            // map is drawn again from the new shape.
            {
                GpuMesh rebaked;
                rebaked.upload(Cube());
                runner::MeshDraw rebakedScene[2] = {runner::MeshDraw{&rebaked, engine_core::matrix4_identity()},
                                                    scene[1]};
                Expect(std::abs(lit(spotLight, rebakedScene, 2)[0] - ambientFloor) <= 4,
                       "a second upload of the cube shadows the same");
                Data away = Cube();
                for (Vertex& vertex : away.vertices) {
                    vertex.p[2] -= 3.f;
                }
                rebaked.upload(away);
                Expect(std::abs(lit(spotLight, rebakedScene, 2)[0] - spotOpen[0]) <= 4,
                       "a mesh uploaded again in place, away from the light's path, takes its shadow with it");
            }

            // A see-through cube casts nothing.
            runner::MeshDraw glassScene[2] = {scene[0], scene[1]};
            glassScene[0].transparency = 0.5f;
            Expect(lit(spotLight, glassScene, 2)[0] > ambientFloor + 20, "a see-through mesh casts no shadow");

            // A light's own Prefab does not shadow it: a box around the light, owned by it.
            runner::MeshDraw lampScene[3] = {scene[0], scene[1],
                                             runner::MeshDraw{cube, engine_core::matrix4_translation(-4.f, 2.f, 0.f)}};
            lampScene[2].owner = 7;
            Expect(std::abs(lit(spotLight, lampScene, 3)[1] - spotOpen[1]) <= 3,
                   "a light's own Prefab does not shadow it");
            lampScene[2].owner = 8;
            Expect(lit(spotLight, lampScene, 3)[1] < spotOpen[1] - 20, "but anyone else's box around it does");

            // A PointLight in the same place: its cube faces in the atlas shadow the same way.
            runner::LightDraw pointLight;
            pointLight.position[0] = -4.f;
            pointLight.position[1] = 2.f;
            pointLight.radius = 7.5f;
            pointLight.intensity = 8.f;
            pointLight.id = 9;
            const std::array<int, 2> pointOpen = lit(pointLight, scene, 2);
            pointLight.shadows = true;
            const std::array<int, 2> pointShadowed = lit(pointLight, scene, 2);
            Expect(pointOpen[0] > ambientFloor + 20 && std::abs(pointShadowed[0] - ambientFloor) <= 4,
                   "a PointLight's shadow falls behind the cube (" + std::to_string(pointShadowed[0]) + ")");
            Expect(std::abs(pointShadowed[1] - pointOpen[1]) <= 3,
                   "and leaves the open floor lit, with no seam or acne (" + std::to_string(pointShadowed[1]) + ")");

            // Two 1024 pages. A SpotLight the camera is inside the Radius of
            // wants a 1024 tile, which fills page 0, so the PointLight's faces
            // go on page 1. The SpotLight points up, away from the floor and
            // the cube: its map holds nothing, so a PointLight that read page 0
            // instead would light the floor behind the cube.
            {
                runner::ShadowSettings paged;
                paged.atlasMinSize = 1024;
                paged.atlasMaxSize = 1024;
                paged.maxTile = 1024;
                paged.atlasMaxPages = 2;
                // The PointLight, outside its Radius, looks small enough for a 64.
                paged.texelsPerPixel = 1.f / 64.f;
                renderer.setShadowSettings(paged);
                runner::LightDraw upward = spotLight;
                upward.direction[0] = 0.f;
                upward.direction[1] = 1.f;
                upward.direction[2] = 0.f;
                const runner::LightDraw both[2] = {upward, pointLight};
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, scene, 2, both, 2);
                const std::array<int, 2> pageOne{sample(inShadow), sample(openFloor)};
                Expect(renderer.shadowAtlasPages() == 2,
                       "a SpotLight filling a page puts the PointLight on a second (" +
                           std::to_string(renderer.shadowAtlasPages()) + " pages)");
                Expect(std::abs(pageOne[0] - ambientFloor) <= 4,
                       "a shadow on the second page falls behind the cube (" + std::to_string(pageOne[0]) + ")");
                Expect(std::abs(pageOne[1] - pointOpen[1]) <= 3,
                       "and leaves the open floor lit (" + std::to_string(pageOne[1]) + " and " +
                           std::to_string(pointOpen[1]) + ")");
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "two atlas pages leave no GL error");
                renderer.setShadowSettings(runner::ShadowSettings{});
            }

            // An atlas page bigger than any GPU makes: the SpotLight wants it,
            // the texture cannot be made, and the atlas stays at the size that
            // works, with the SpotLight fitted to it, rather than every shadow going.
            {
                runner::ShadowSettings huge;
                huge.atlasMaxSize = 1 << 17;
                huge.maxTile = 1 << 17;
                huge.atlasMaxPages = 1;
                renderer.setShadowSettings(huge);
                const std::array<int, 2> capped = lit(spotLight, scene, 2);
                Expect(renderer.shadowAtlasPages() == 1, "an atlas the GPU cannot make falls back to one it can (" +
                                                             std::to_string(renderer.shadowAtlasPages()) + " pages)");
                Expect(std::abs(capped[0] - ambientFloor) <= 4 && std::abs(capped[1] - spotOpen[1]) <= 3,
                       "and the SpotLight still casts its shadow (" + std::to_string(capped[0]) + " and " +
                           std::to_string(capped[1]) + ")");
                Expect(lit(spotLight, scene, 2) == capped, "a second frame draws the same, without trying again");
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the failed atlas leaves no GL error behind");
                renderer.setShadowSettings(runner::ShadowSettings{});
            }

            // A DirectionalLight shining from the -X side and down, as the others did.
            runner::LightDraw sunLight;
            sunLight.kind = runner::LightDraw::Kind::Directional;
            sunLight.direction[0] = 0.894f;
            sunLight.direction[1] = -0.447f;
            sunLight.direction[2] = 0.f;
            sunLight.intensity = 2.f;
            sunLight.id = 11;
            const std::array<int, 2> sunOpen = lit(sunLight, scene, 2);
            sunLight.shadows = true;
            const std::array<int, 2> sunShadowed = lit(sunLight, scene, 2);
            Expect(sunOpen[0] > ambientFloor + 20 && std::abs(sunShadowed[0] - ambientFloor) <= 4,
                   "a DirectionalLight's shadow falls behind the cube (" + std::to_string(sunShadowed[0]) + ")");
            Expect(std::abs(sunShadowed[1] - sunOpen[1]) <= 3,
                   "and leaves the open floor lit (" + std::to_string(sunShadowed[1]) + ")");
            Expect(lit(sunLight, scene, 2) == sunShadowed, "a still camera reuses the cascades and draws the same");
            // The floor is about 7.6 from the camera: past a ShadowDistance of 2, unshadowed.
            sunLight.shadowDistance = 2.f;
            Expect(std::abs(lit(sunLight, scene, 2)[0] - sunOpen[0]) <= 3, "past ShadowDistance there is no shadow");
            // Straight down: the cube's shadow is under it, hidden, and nothing is NaN.
            sunLight.shadowDistance = 100.f;
            sunLight.direction[0] = 0.f;
            sunLight.direction[1] = -1.f;
            Expect(lit(sunLight, scene, 2)[0] > ambientFloor + 20, "a sun straight down lights the floor beside the cube");
            // CL4: a caster the camera cannot see still shadows what it can. A cube
            // 12 studs above the open floor point is far above the view.
            const runner::MeshDraw withHigh[3] = {scene[0], scene[1],
                                                  runner::MeshDraw{cube, engine_core::matrix4_translation(1.f, 12.f, 2.f)}};
            const int openUnder = lit(sunLight, scene, 2)[1];
            const std::array<int, 2> high = lit(sunLight, withHigh, 3);
            Expect(renderer.stats().culled == 1, "CL4 the high cube is out of view (" +
                                                     std::to_string(renderer.stats().culled) + " culled)");
            Expect(openUnder > ambientFloor + 20 && std::abs(high[1] - ambientFloor) <= 4,
                   "CL4 and its shadow still falls on the open floor (" + std::to_string(high[1]) + " against " +
                       std::to_string(ambientFloor) + ")");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "cascades leave no GL error");

            // Shadows off in the settings draws every light unshadowed.
            runner::ShadowSettings off;
            off.enabled = false;
            renderer.setShadowSettings(off);
            Expect(lit(spotLight, scene, 2)[0] > ambientFloor + 20, "ShadowSettings::enabled false draws no shadows");
            renderer.setShadowSettings(runner::ShadowSettings{});
        }
        }

        // A Material's DiffuseTexture, read from the resources folder, and its Color.
        {
            std::filesystem::create_directories(root / "textures");
            std::ofstream(root / "textures" / "stripes.tga", std::ios::binary) << StripesTga();
            std::ofstream(root / "textures" / "junk.png", std::ios::binary) << "not an image";
            const std::string tga = StripesTga();
            runner::TexturePixels pixels;
            std::string why;
            Expect(runner::DecodeTexture(reinterpret_cast<const std::uint8_t*>(tga.data()), tga.size(), pixels, why) &&
                       pixels.width == 4 && pixels.height == 4,
                   "a TGA decodes to 4 by 4 (" + why + ")");
            Expect(pixels.rgba.size() == 64 && pixels.rgba[2] == 255 && pixels.rgba[0] == 0 && pixels.rgba[60] == 255,
                   "bottom row first, as OpenGL takes it: blue, then red at the top");

            std::vector<std::string> said;
            runner::TextureCache textures([&said](const std::string& message) { said.push_back(message); });
            Expect(textures.get("textures/stripes.tga") == 0, "no root loads no texture");
            textures.setRoot(root);
            const unsigned stripes = textures.get("textures/stripes.tga");
            Expect(stripes != 0, "the texture loads from the resources folder");
            Expect(textures.get("textures/stripes.tga") == stripes, "a second get is the same upload");
            Expect(textures.get("textures/missing.png") == 0 && textures.get("textures/junk.png") == 0,
                   "a missing file and a file that is not an image have no texture");
            Expect(said.size() == 2, "each says why once (" + std::to_string(said.size()) + " reports)");

            // The front face's v runs from 0 at its bottom, 10 of 128 rows below the middle, to 1 at its
            // top, 4 above: the middle is in the image's top half, 6 rows below in its bottom half.
            runner::MeshDraw textured{cube, engine_core::matrix4_identity(), stripes};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &textured, 1);
            const Pixel upper = ReadPixel(fbWidth / 2, fbHeight / 2);
            const Pixel lower = ReadPixel(fbWidth / 2, fbHeight / 2 - fbHeight * 3 / 64);
            Expect(upper.r > 30 && upper.b < kSkyTint && upper.g < kSkyTint,
                   "the image's top is at the face's top (" + Text(upper) + ")");
            Expect(lower.b > 30 && lower.r < kSkyTint && lower.g < kSkyTint,
                   "and its bottom at the bottom (" + Text(lower) + ")");

            // Color tints the texture, and draws alone with none.
            runner::MeshDraw tinted{cube, engine_core::matrix4_identity(), stripes, {0.f, 1.f, 1.f, 1.f}};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &tinted, 1);
            Expect(ReadPixel(fbWidth / 2, fbHeight / 2).r < kSkyTint, "a cyan Color takes out the red");
            runner::MeshDraw green{cube, engine_core::matrix4_identity(), 0, {0.f, 1.f, 0.f, 1.f}};
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &green, 1);
            const Pixel plain = ReadPixel(fbWidth / 2, fbHeight / 2);
            Expect(plain.g > 30 && plain.r < kSkyTint && plain.b < kSkyTint,
                   "no texture draws the Color alone (" + Text(plain) + ")");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "textures leave no GL error");
            textures.clear();
        }

        // A Skybox: drawn behind everything, and lighting what it surrounds.
        {
            std::filesystem::create_directories(root / "textures");
            std::ofstream(root / "textures" / "sky.hdr", std::ios::binary) << TestSky();
            std::ofstream(root / "textures" / "white.hdr", std::ios::binary)
                << Hdr(16, 8, [](int, int) { return std::array<float, 3>{1.f, 1.f, 1.f}; });
            std::ofstream(root / "textures" / "sky.exr", std::ios::binary) << "v/1\x01 not really";

            // Decoded linear, brighter than white, bottom row first, and halved when too wide.
            const std::string hdr = TestSky();
            runner::LinearPixels linear;
            std::string why;
            Expect(runner::DecodeLinearTexture(reinterpret_cast<const std::uint8_t*>(hdr.data()), hdr.size(), linear,
                                               why) &&
                       linear.width == 16 && linear.height == 8,
                   "an HDR decodes to 16 by 8 (" + why + ")");
            Expect(linear.rgb.size() == 16 * 8 * 3 && linear.rgb[2] > 0.9f && linear.rgb[0] == 0.f,
                   "bottom row first: blue");
            Expect(linear.rgb.size() == 16 * 8 * 3 && linear.rgb[(7 * 16) * 3] > 3.9f,
                   "the top row keeps light brighter than white");
            runner::LinearPixels halved;
            runner::DecodeLinearTexture(reinterpret_cast<const std::uint8_t*>(hdr.data()), hdr.size(), halved, why, 8);
            Expect(halved.width == 8 && halved.height == 4 && halved.rgb[(3 * 8) * 3] > 3.9f,
                   "an image wider than the limit is halved");
            const std::string tga = StripesTga();
            runner::DecodeLinearTexture(reinterpret_cast<const std::uint8_t*>(tga.data()), tga.size(), linear, why);
            Expect(linear.width == 4 && std::abs(linear.rgb[2] - 1.f) < 1e-3f,
                   "an ordinary image decodes too, sRGB made linear");

            std::vector<std::string> said;
            runner::TextureCache textures([&said](const std::string& message) { said.push_back(message); });
            textures.setRoot(root);
            const runner::EnvironmentTexture skyImage = textures.getEnvironment("textures/sky.hdr");
            const runner::EnvironmentTexture white = textures.getEnvironment("textures/white.hdr");
            Expect(skyImage.texture != 0 && skyImage.revision != 0 && white.revision != skyImage.revision,
                   "a sky uploads with a revision of its own");
            Expect(textures.getEnvironment("textures/sky.hdr").revision == skyImage.revision,
                   "an unchanged file keeps its revision");
            Expect(textures.get("textures/sky.hdr") != skyImage.texture,
                   "a Material's upload of the same file is a separate texture");
            Expect(textures.getEnvironment("textures/sky.exr").texture == 0 && !said.empty() &&
                       said.back().find("OpenEXR") != std::string::npos,
                   "an .exr says it is OpenEXR, which cannot be read (" + (said.empty() ? "" : said.back()) + ")");

            runner::SceneLighting lit;
            lit.ambient[0] = lit.ambient[1] = lit.ambient[2] = 0.f;
            lit.sky.image = skyImage.texture;
            lit.sky.imageRevision = skyImage.revision;
            renderer.setLighting(lit);
            // On macOS the cubes cannot be drawn into the frame they are made in: draw again.
            const auto drawSky = [&](const runner::MeshDraw* meshes, int count) {
                bool drawn = false;
                for (int attempt = 0; attempt < 3 && !drawn; ++attempt) {
                    drawn = renderer.draw(0, 0, kSize, kSize, kSize, kSize, meshes, count);
                }
                return drawn;
            };

            // Looking straight down -Z, level: the sky fills the pane even with nothing in it.
            renderer.setCamera(engine_core::matrix4_look_at({0.f, 0.f, 7.f}, {0.f, 0.f, 0.f}, up), 60.f);
            Expect(drawSky(nullptr, 0), "a sky with no meshes draws");
            const Pixel upperLeft = ReadPixel(fbWidth / 4, fbHeight * 3 / 4);
            const Pixel upperRight = ReadPixel(fbWidth * 3 / 4, fbHeight * 3 / 4);
            const Pixel below = ReadPixel(fbWidth / 2, fbHeight / 4);
            Expect(upperLeft.r > 150 && upperLeft.g < 30 && upperLeft.b < 30,
                   "the image's left half is on the left, above the horizon (" + Text(upperLeft) + ")");
            Expect(upperRight.g > 150 && upperRight.r < 30 && upperRight.b < 30,
                   "and its right half on the right (" + Text(upperRight) + ")");
            Expect(below.b > 60 && below.r < 30 && below.g < 30, "and below the horizon, blue (" + Text(below) + ")");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "making the cubes leaves no GL error");

            // Rotation turns it about Y: half a turn swaps left and right.
            runner::SceneLighting turned = lit;
            turned.sky.rotationDegrees = 180.f;
            renderer.setLighting(turned);
            Expect(drawSky(nullptr, 0), "a turned sky draws at once, with the cubes it has");
            const Pixel turnedLeft = ReadPixel(fbWidth / 4, fbHeight * 3 / 4);
            Expect(turnedLeft.g > 150 && turnedLeft.r < 30, "Rotation 180 puts green on the left (" +
                                                                Text(turnedLeft) + ")");
            // Exposure 0 is black, not the pane's clear color; Tint multiplies.
            runner::SceneLighting dark = lit;
            dark.sky.exposure = 0.f;
            renderer.setLighting(dark);
            drawSky(nullptr, 0);
            Expect(Sum(ReadPixel(fbWidth / 4, fbHeight * 3 / 4)) < 6, "Exposure 0 is a black sky");
            runner::SceneLighting cyan = lit;
            cyan.sky.tint[0] = 0.f;
            renderer.setLighting(cyan);
            drawSky(nullptr, 0);
            const Pixel tinted = ReadPixel(fbWidth / 4, fbHeight * 3 / 4);
            const Pixel tintedRight = ReadPixel(fbWidth * 3 / 4, fbHeight * 3 / 4);
            Expect(Sum(tinted) < 10 && tintedRight.g > 150,
                   "a cyan Tint takes the red out and leaves the green (" + Text(tinted) + ")");

            // The sky lights a white cube: its top takes the red and green above,
            // its front some of the blue below too.
            renderer.setLighting(lit);
            renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                               runner::Renderer::kCameraFovYDegrees);
            runner::MeshDraw matte{cube, engine_core::matrix4_identity()};
            matte.roughness = 1.f;
            Expect(drawSky(&matte, 1), "a cube under a sky draws");
            const int topY = fbHeight / 2 + fbHeight * 3 / 64;
            const int frontY = fbHeight / 2 - fbHeight * 3 / 64;
            const Pixel skyTop = ReadPixel(fbWidth / 2, topY);
            const Pixel skyFront = ReadPixel(fbWidth / 2, frontY);
            Expect(skyTop.r > 40 && skyTop.g > 40 && skyTop.b + 20 < skyTop.r,
                   "the top face takes the sky above it (" + Text(skyTop) + ")");
            Expect(skyFront.b > skyTop.b + 10, "the front face takes more of the blue below (" + Text(skyFront) +
                                                   " against " + Text(skyTop) + ")");

            // LightScale dims the light the sky gives surfaces, and leaves the sky behind them alone.
            const Pixel skyCorner = ReadPixel(fbWidth / 8, fbHeight * 7 / 8);
            runner::SceneLighting dimLight = lit;
            dimLight.sky.lightScale = 0.f;
            renderer.setLighting(dimLight);
            drawSky(&matte, 1);
            Expect(Sum(ReadPixel(fbWidth / 2, topY)) < 6,
                   "LightScale 0 takes the sky's light off the cube (" + Text(ReadPixel(fbWidth / 2, topY)) + ")");
            const Pixel dimCorner = ReadPixel(fbWidth / 8, fbHeight * 7 / 8);
            Expect(dimCorner.r == skyCorner.r && dimCorner.g == skyCorner.g && dimCorner.b == skyCorner.b,
                   "and leaves the sky behind it as it was (" + Text(dimCorner) + " and " + Text(skyCorner) + ")");
            dimLight.sky.lightScale = 0.5f;
            renderer.setLighting(dimLight);
            drawSky(&matte, 1);
            const int halfTop = Sum(ReadPixel(fbWidth / 2, topY));
            Expect(halfTop > 6 && halfTop < Sum(skyTop), "LightScale 0.5 is between (" + std::to_string(halfTop) +
                                                             " under " + std::to_string(Sum(skyTop)) + ")");
            renderer.setLighting(lit);
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the sky leaves no GL error");

            // A DynamicSky: drawn from its shader, and lighting what it surrounds.
            {
                const auto skyAt = [&](double hours, double latitude, int quality) {
                    runner::SceneLighting out;
                    out.ambient[0] = out.ambient[1] = out.ambient[2] = 0.f;
                    const runner::SkyState state = runner::ComputeSky(hours, latitude, 3.0, 0.0, 0.0);
                    runner::SceneDynamicSky& sky = out.dynamicSky;
                    sky.enabled = true;
                    runner::SetSkyState(sky, state);
                    sky.cloudCover = 0.f;
                    sky.cloudDensity = 0.f;
                    sky.reflectionQuality = static_cast<runner::SceneQuality>(quality);
                    sky.key = {static_cast<float>(hours), static_cast<float>(latitude), 0.f, 0.f, quality};
                    return std::make_pair(out, runner::SkyLightDraw(state, false));
                };
                // A second apart: LightingDue holds back a change that comes sooner
                // than kLightingChangeSeconds after the last drawing of the cube.
                double clock = 0.0;
                const auto drawDynamic = [&](std::pair<runner::SceneLighting, runner::LightDraw> sky,
                                             const runner::MeshDraw* meshes, int count) {
                    clock += 1.0;
                    sky.first.dynamicSky.seconds = clock;
                    renderer.setLighting(sky.first);
                    bool drawn = false;
                    for (int attempt = 0; attempt < 3 && !drawn; ++attempt) {
                        drawn = renderer.draw(0, 0, kSize, kSize, kSize, kSize, meshes, count, &sky.second, 1);
                    }
                    return drawn;
                };
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 0.f, 7.f}, {0.f, 0.f, 0.f}, up), 60.f);
                const auto noon = skyAt(12.0, 0.0, 1);
                Expect(drawDynamic(noon, nullptr, 0), "a DynamicSky with no meshes draws");
                const Pixel noonSky = ReadPixel(fbWidth / 2, fbHeight * 7 / 8);
                Expect(noonSky.b > noonSky.r && Sum(noonSky) > 60,
                       "the noon sky is bright and blue (" + Text(noonSky) + ")");
                const auto midnight = skyAt(0.0, 0.0, 1);
                drawDynamic(midnight, nullptr, 0);
                const Pixel midnightSky = ReadPixel(fbWidth / 2, fbHeight * 7 / 8);
                Expect(Sum(midnightSky) * 4 < Sum(noonSky),
                       "the midnight sky is dark (" + Text(midnightSky) + " against " + Text(noonSky) + ")");
                // Straight down is dark ground, not NaN (which tone maps to black or garbage).
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 5.f, 0.f}, {0.f, 0.f, 0.001f}, up), 60.f);
                drawDynamic(noon, nullptr, 0);
                const Pixel ground = ReadPixel(fbWidth / 2, fbHeight / 2);
                Expect(Sum(ground) > 3 && Sum(ground) < Sum(noonSky),
                       "straight down is dark ground, not NaN (" + Text(ground) + ")");
                // At sunset the sun sits on the horizon, due west: the ground below it
                // is the ground beside it, not the disc smeared down the column.
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 0.f, 7.f}, {-10.f, 0.f, 7.f}, up), 60.f);
                drawDynamic(skyAt(18.0, 0.0, 1), nullptr, 0);
                const Pixel belowSun = ReadPixel(fbWidth / 2, fbHeight / 4);
                const Pixel besideSun = ReadPixel(fbWidth / 2 + fbWidth / 32, fbHeight / 4);
                Expect(std::abs(Sum(belowSun) - Sum(besideSun)) <= 9,
                       "the sun does not streak down the ground below it (" + Text(belowSun) + " against " +
                           Text(besideSun) + ")");

                // The sun lights a cube at noon far more than the moon at midnight.
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                                   runner::Renderer::kCameraFovYDegrees);
                runner::MeshDraw skyCube{cube, engine_core::matrix4_identity()};
                skyCube.roughness = 1.f;
                const int cubeTopY = fbHeight / 2 + fbHeight * 3 / 64;
                Expect(drawDynamic(noon, &skyCube, 1), "a cube under a DynamicSky draws");
                const Pixel noonTop = ReadPixel(fbWidth / 2, cubeTopY);
                drawDynamic(midnight, &skyCube, 1);
                const Pixel midnightTop = ReadPixel(fbWidth / 2, cubeTopY);
                Expect(Sum(noonTop) > 2 * Sum(midnightTop) + 10,
                       "noon lights the cube's top more than midnight (" + Text(noonTop) + " against " +
                           Text(midnightTop) + ")");
                // A new ReflectionQuality makes the cubes again, and draws at once.
                Expect(drawDynamic(skyAt(12.0, 0.0, 2), &skyCube, 1), "ReflectionQuality High draws");
                Expect(drawDynamic(skyAt(12.0, 0.0, 0), &skyCube, 1), "and Low");
                // At a pole the sun rides the horizon: no light, but the scene still draws.
                Expect(drawDynamic(skyAt(12.0, 90.0, 1), &skyCube, 1), "a polar sky draws");
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the DynamicSky leaves no GL error");

                // Back to the image sky: its own cubes again, not the procedural ones.
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 0.f, 7.f}, {0.f, 0.f, 0.f}, up), 60.f);
                renderer.setLighting(lit);
                Expect(drawSky(nullptr, 0), "the image sky draws again after a DynamicSky");
                const Pixel back = ReadPixel(fbWidth / 4, fbHeight * 3 / 4);
                Expect(back.r > 150 && back.g < 30 && back.b < 30,
                       "the image sky comes back (" + Text(back) + ")");
                renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                                   runner::Renderer::kCameraFovYDegrees);
            }

            // With no image, today's stand-in again, and the corner the clear color.
            renderer.setLighting(runner::SceneLighting{});
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &matte, 1);
            Expect(IsClear(ReadPixel(2, 2)), "no Skybox leaves the pane's clear color around the cube");
            textures.clear();
        }

        // The floor grid: from (0, 3, 7), the X axis crosses the view through
        // its middle, red, and the Z axis runs down from the middle, blue.
        {
            renderer.setCamera(engine_core::matrix4_look_at({0.f, 3.f, 7.f}, {0.f, 0.f, 0.f}, up),
                               runner::Renderer::kCameraFovYDegrees);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, nullptr, 0);
            Expect(IsClear(ReadPixel(fbWidth / 4, fbHeight / 2)), "the grid is off until set");
            renderer.setGridVisible(true);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, nullptr, 0);
            const Pixel xAxis = ReadPixel(fbWidth / 4, fbHeight / 2);
            const Pixel zAxis = ReadPixel(fbWidth / 2, fbHeight / 4);
            Expect(xAxis.r > xAxis.g + 60 && xAxis.r > xAxis.b + 40, "the X axis is red (" + Text(xAxis) + ")");
            Expect(zAxis.b > zAxis.r + 60 && zAxis.b > zAxis.g + 20, "the Z axis is blue (" + Text(zAxis) + ")");
            Expect(IsClear(ReadPixel(2, fbHeight - 3)), "above the horizon there is no grid");
            // The cube hides the floor behind it, and stands on nothing it lets show.
            renderer.setGridVisible(false);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
            const Pixel bare = ReadPixel(fbWidth / 2, fbHeight / 2);
            renderer.setGridVisible(true);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, &draw, 1);
            const Pixel gridded = ReadPixel(fbWidth / 2, fbHeight / 2);
            Expect(std::abs(Sum(gridded) - Sum(bare)) <= 3,
                   "a surface in front of the floor hides the grid (" + Text(gridded) + " and " + Text(bare) + ")");
            Expect(ReadPixel(fbWidth / 4, fbHeight / 2).r > xAxis.r - 10, "and the axis beside it still shows");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the grid leaves no GL error");
            renderer.setGridVisible(false);
        }

        // A file that changes is read again after the recheck interval: this one stops drawing.
        std::ofstream(root / "meshes" / "cube.amesh", std::ios::binary | std::ios::trunc) << "broken now";
        std::filesystem::last_write_time(root / "meshes" / "cube.amesh",
                                         std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        Expect(meshes.get("meshes/cube.amesh") == nullptr, "a mesh whose file broke stops drawing");
        Expect(reports.size() == 3, "and says why");

        // A play session's geometry: uploaded from memory, again only when its revision changes.
        const Data session = Cube();
        const GpuMesh* live = meshes.getSession(9, session, 1);
        Expect(live != nullptr && live->valid(), "a session mesh uploads from memory");
        Expect(meshes.getSession(9, session, 1) == live, "the same revision is the same upload");
        runner::MeshDraw from_session{live, engine_core::matrix4_identity()};
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, &from_session, 1);
        Expect(!IsClear(ReadPixel(fbWidth / 2, fbHeight / 2)), "a session mesh draws like a file's");
        Expect(meshes.getSession(9, Data{}, 2) == nullptr, "a session Cleared to nothing draws nothing");
        Expect(meshes.getSession(9, session, 3) != nullptr, "a new revision uploads again");
        meshes.sweepSessions();
        Expect(live->valid(), "a sweep keeps the uploads this frame asked for");
        meshes.sweepSessions();
        Expect(meshes.getSession(9, session, 3) != nullptr, "one swept away uploads again when asked");
        Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "session uploads leave no GL error");

        {
            // CL1–CL3: culling. From (0, 0, 8) down -Z with a 60 degree view, a cube
            // at the origin is in view, one at (0, 0, 20) is behind the camera, and
            // one at (40, 0, 0) is far past the right edge.
            // A fresh mesh of its own: by here the file-backed cube above was broken
            // on purpose (the recheck test) and is no longer valid.
            GpuMesh cullCube;
            cullCube.upload(Cube());
            runner::Renderer culler;
            Expect(culler.initialize(), "the renderer builds for culling");
            culler.setCamera(engine_core::matrix4_translation(0.f, 0.f, 8.f), 60.f);
            const runner::MeshDraw three[3] = {runner::MeshDraw{&cullCube, engine_core::matrix4_identity()},
                                               runner::MeshDraw{&cullCube, engine_core::matrix4_translation(0.f, 0.f, 20.f)},
                                               runner::MeshDraw{&cullCube, engine_core::matrix4_translation(40.f, 0.f, 0.f)}};
            culler.draw(0, 0, kSize, kSize, kSize, kSize, three, 3);
            const runner::RenderStats culled = culler.stats();
            Expect(culled.draws == 3 && culled.visible == 1 && culled.culled == 2 && culled.runs == 1,
                   "CL1 two of three cubes are culled (" + std::to_string(culled.visible) + " visible, " +
                       std::to_string(culled.culled) + " culled)");
            std::vector<unsigned char> withCulling(static_cast<std::size_t>(fbWidth) * fbHeight * 4);
            glReadPixels(0, 0, fbWidth, fbHeight, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, withCulling.data());
            culler.setCulling(false);
            culler.draw(0, 0, kSize, kSize, kSize, kSize, three, 3);
            Expect(culler.stats().visible == 3 && culler.stats().culled == 0, "CL2 culling off draws all three");
            std::vector<unsigned char> without(withCulling.size());
            glReadPixels(0, 0, fbWidth, fbHeight, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, without.data());
            Expect(withCulling == without, "CL3 culling changes no pixel");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "culling leaves no GL error");
            culler.shutdown();
            cullCube.destroy();
        }

        {
            // IN1–IN4: instancing. From (0, 0, 8) down -Z, white cubes of one
            // slot, left tinted red and right green, draw as one run.
            // A fresh mesh of its own: by here the file-backed cube above was
            // broken on purpose (the recheck test) and is no longer valid.
            GpuMesh instanceCube;
            instanceCube.upload(Cube());
            runner::Renderer batcher;
            Expect(batcher.initialize(), "the renderer builds for instancing");
            batcher.setCamera(engine_core::matrix4_translation(0.f, 0.f, 8.f), 60.f);
            runner::MeshDraw pair[2] = {
                runner::MeshDraw{&instanceCube, engine_core::matrix4_translation(-1.5f, 0.f, 0.f)},
                runner::MeshDraw{&instanceCube, engine_core::matrix4_translation(1.5f, 0.f, 0.f)}};
            pair[0].slot = pair[1].slot = 1;
            pair[0].tint[1] = pair[0].tint[2] = 0.f;
            pair[1].tint[0] = pair[1].tint[2] = 0.f;
            batcher.draw(0, 0, kSize, kSize, kSize, kSize, pair, 2);
            const Pixel left = ReadPixel(fbWidth * 40 / kSize, fbHeight / 2);
            const Pixel right = ReadPixel(fbWidth * 88 / kSize, fbHeight / 2);
            Expect(batcher.stats().runs == 1 && batcher.stats().instancedCalls == 1,
                   "IN1 two cubes of one slot draw in one call (" + std::to_string(batcher.stats().runs) + " runs)");
            Expect(left.r > left.g + 30 && right.g > right.r + 30,
                   "IN2 each keeps its own tint (" + Text(left) + " and " + Text(right) + ")");

            // A mirrored cube draws the same as an unmirrored one, in a run of its own.
            runner::MeshDraw plain{&instanceCube, engine_core::matrix4_identity()};
            plain.slot = 1;
            batcher.draw(0, 0, kSize, kSize, kSize, kSize, &plain, 1);
            const Pixel unmirrored = ReadPixel(fbWidth / 2, fbHeight / 2);
            runner::MeshDraw both[2] = {plain, plain};
            both[1].model.m[0] = -1.f;
            both[0].model = engine_core::matrix4_translation(0.f, 0.f, -30.f);  // hidden behind it, same slot
            batcher.draw(0, 0, kSize, kSize, kSize, kSize, both, 2);
            const Pixel mirrored = ReadPixel(fbWidth / 2, fbHeight / 2);
            Expect(batcher.stats().runs == 2, "IN3 a mirrored cube draws in a run of its own");
            Expect(std::abs(Sum(mirrored) - Sum(unmirrored)) <= 3,
                   "IN3 and shows its outside (" + Text(mirrored) + " against " + Text(unmirrored) + ")");

            // Slot 0 never batches.
            const runner::MeshDraw loose[3] = {
                runner::MeshDraw{&instanceCube, engine_core::matrix4_translation(-1.5f, 0.f, 0.f)},
                runner::MeshDraw{&instanceCube, engine_core::matrix4_identity()},
                runner::MeshDraw{&instanceCube, engine_core::matrix4_translation(1.5f, 0.f, 0.f)}};
            batcher.draw(0, 0, kSize, kSize, kSize, kSize, loose, 3);
            Expect(batcher.stats().runs == 3, "IN4 slot 0 draws each alone");
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "instancing leaves no GL error");
            batcher.shutdown();
            instanceCube.destroy();
        }

        if (!regressionMode.empty()) {
            GpuMesh regressionCube;
            regressionCube.upload(Cube());
            SaveOrCompareRegression(RegressionFrames(&regressionCube, kSize, fbWidth, fbHeight), regressionMode,
                                    regressionDir);
            regressionCube.destroy();
        }

        meshes.clear();
        renderer.shutdown();
    }
    Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "shutdown leaves no GL error");

    // Terrain: Surface Nets chunk meshes, each vertex's material Id in its
    // color, drawn through the terrain program with a look table.
    {
        runner::Renderer renderer;
        Expect(renderer.initialize(), "the renderer builds its terrain program");

        // A ball of Id 1, with Id 1 pure red: the middle of the view is red.
        // Through AppendTerrainDraws, as the Scene View draws a snapshot's
        // Terrains: SelectTerrainNodes, MeshCache::getTerrainNode and
        // Renderer::terrainLookTexture. The chunks are level-0 nodes, each a root.
        {
            terrain::VoxelVolume volume(0.25f);
            Edit(volume.fill(Ball(0.f, 0.f, 0.f, 1.5f), 1), "the red ball");
            engine_core::TerrainView view;
            view.terrain = 77;
            view.chunks = MeshChunks(volume, 1);
            view.nodes = ChunkNodes(*view.chunks, 0.25f);
            view.look = MakeLook(LookBytes({{1, 255, 0, 0}}), 1);
            Expect(!view.chunks->empty(), "the red ball meshes (" + std::to_string(view.chunks->size()) + " chunks)");
            runner::MeshCache chunkMeshes;
            runner::TerrainFadeState fades;
            const runner::TerrainCamera camera = RendererCamera(renderer, kSize, kSize);
            std::vector<runner::MeshDraw> draws;
            runner::AppendTerrainDraws({view}, camera, 0.0, fades, chunkMeshes, renderer, draws);
            Expect(draws.size() == view.chunks->size() && draws.front().terrainLook != 0 &&
                       draws.front().owner == 77 && draws.front().slot == 0,
                   "each chunk is a MeshDraw with the Terrain's look, owned by it, drawn alone");
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, draws.data(), static_cast<int>(draws.size()));
            const Pixel middle = ReadPixel(fbWidth / 2, fbHeight / 2);
            Expect(middle.r > middle.g + 40 && middle.r > middle.b + 40,
                   "a terrain ball whose Id 1 is red draws red (" + Text(middle) + ")");
            Expect(IsClear(ReadPixel(2, 2)), "and the corner is the clear color");
            Expect(renderer.stats().runs == static_cast<int>(draws.size()),
                   "each terrain chunk is a run of its own (" + std::to_string(renderer.stats().runs) + " runs, " +
                       std::to_string(draws.size()) + " chunks)");
            // R7: SetTerrainLodColors tints each draw by its level: level 5's
            // blue over the red ball while on, and the red again once off.
            {
                Expect(!runner::TerrainLodColors(), "terrain LOD colors are off by default");
                Expect(draws.front().terrainLevel == 0, "a chunk's node draws as level 0");
                std::vector<runner::MeshDraw> tinted = draws;
                for (runner::MeshDraw& draw : tinted) {
                    draw.terrainLevel = 5;
                }
                runner::SetTerrainLodColors(true);
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, tinted.data(), static_cast<int>(tinted.size()));
                const Pixel blue = ReadPixel(fbWidth / 2, fbHeight / 2);
                runner::SetTerrainLodColors(false);
                Expect(blue.b > blue.r + 40 && blue.b > blue.g + 40,
                       "with LOD colors on, a level-5 node draws in level 5's blue (" + Text(blue) + ")");
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, tinted.data(), static_cast<int>(tinted.size()));
                const Pixel red = ReadPixel(fbWidth / 2, fbHeight / 2);
                Expect(red.r > red.g + 40 && red.r > red.b + 40,
                       "and in its look's red once they are off (" + Text(red) + ")");
            }
            // The same draws as shadow casters only (terrain out of view, R15): nothing shows.
            {
                std::vector<runner::MeshDraw> casting = draws;
                for (runner::MeshDraw& draw : casting) {
                    draw.shadowOnly = true;
                }
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, casting.data(), static_cast<int>(casting.size()));
                const Pixel hidden = ReadPixel(fbWidth / 2, fbHeight / 2);
                Expect(IsClear(hidden), "the ball's draws as shadow casters only are not seen (" + Text(hidden) + ")");
            }

            // A new look (Id 1 green) on the same chunks: the same uploads and
            // texture name, the new texels.
            engine_core::TerrainView green = view;
            green.look = MakeLook(LookBytes({{1, 0, 255, 0}}), 2);
            std::vector<runner::MeshDraw> greenDraws;
            runner::AppendTerrainDraws({green}, camera, 0.0, fades, chunkMeshes, renderer, greenDraws);
            Expect(greenDraws.size() == draws.size() && greenDraws.front().mesh == draws.front().mesh &&
                       greenDraws.front().terrainLook == draws.front().terrainLook,
                   "a new look revision keeps the chunk uploads and the texture");
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, greenDraws.data(), static_cast<int>(greenDraws.size()));
            const Pixel turned = ReadPixel(fbWidth / 2, fbHeight / 2);
            Expect(turned.g > turned.r + 40 && turned.g > turned.b + 40,
                   "and draws in the new look's green (" + Text(turned) + ")");
            // Back to red (another revision change), for the frame with a cube.
            draws.clear();
            runner::AppendTerrainDraws({view}, camera, 0.0, fades, chunkMeshes, renderer, draws);

            // Behind a cube: the cube's material program and the terrain's in one frame.
            GpuMesh mixCube;
            mixCube.upload(Cube());
            std::vector<runner::MeshDraw> mixed = draws;
            runner::MeshDraw blue{&mixCube, engine_core::matrix4_translation(2.5f, 0.f, 0.f)};
            blue.color[0] = 0.f;
            blue.color[1] = 0.f;
            blue.color[2] = 1.f;
            // First in the list, so sorting is what puts the terrain after it.
            mixed.insert(mixed.begin(), blue);
            renderer.draw(0, 0, kSize, kSize, kSize, kSize, mixed.data(), static_cast<int>(mixed.size()));
            const Pixel stillRed = ReadPixel(fbWidth / 2, fbHeight / 2);
            const Pixel cubeBlue = ReadPixel(fbWidth / 2 + fbWidth * 36 / kSize, fbHeight / 2);
            Expect(stillRed.r > stillRed.g + 40 && stillRed.r > stillRed.b + 40 && cubeBlue.b > cubeBlue.r + 40,
                   "with a cube beside it, the ball stays red and the cube blue (" + Text(stillRed) + " and " +
                       Text(cubeBlue) + ")");
            mixCube.destroy();

            // A cross-fade halfway: the red ball fading in and the same meshes
            // in green fading out cover the ball's pixels between them, each
            // about half, and nothing shows the clear color through.
            {
                const std::vector<float> greenBytes = LookBytes({{1, 0, 255, 0}});
                const unsigned greenLook = runner::MakeTerrainLookTexture(greenBytes.data());
                std::vector<runner::MeshDraw> fading;
                for (const runner::MeshDraw& draw : draws) {
                    runner::MeshDraw in = draw;
                    in.terrainFade = 0.5f;
                    in.terrainFadeIn = true;
                    runner::MeshDraw out = draw;
                    out.terrainLook = greenLook;
                    out.terrainFade = 0.5f;
                    out.terrainFadeIn = false;
                    fading.push_back(in);
                    fading.push_back(out);
                }
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, fading.data(), static_cast<int>(fading.size()));
                int reds = 0;
                int greens = 0;
                int clear = 0;
                for (int dy = -4; dy < 4; ++dy) {
                    for (int dx = -4; dx < 4; ++dx) {
                        const Pixel p = ReadPixel(fbWidth / 2 + dx, fbHeight / 2 + dy);
                        if (p.r > p.g + 10) {
                            ++reds;
                        } else if (p.g > p.r + 10) {
                            ++greens;
                        } else if (IsClear(p)) {
                            ++clear;
                        }
                    }
                }
                // The pattern is 4 x 4, so 8 x 8 holds it four times; antialiasing
                // blends neighbors toward each other (a pixel leans red or green
                // rather than being pure), but none is left empty.
                Expect(reds >= 8 && greens >= 8 && clear == 0,
                       "a half cross-fade dithers the two levels over every pixel (" + std::to_string(reds) +
                           " red, " + std::to_string(greens) + " green, " + std::to_string(clear) +
                           " clear of 64; the middle " + Text(ReadPixel(fbWidth / 2, fbHeight / 2)) + ", beside it " +
                           Text(ReadPixel(fbWidth / 2 + 1, fbHeight / 2)) + ")");
                // The one fading in alone, at half: the dither leaves holes.
                std::vector<runner::MeshDraw> half;
                for (std::size_t i = 0; i < fading.size(); i += 2) {
                    half.push_back(fading[i]);
                }
                renderer.draw(0, 0, kSize, kSize, kSize, kSize, half.data(), static_cast<int>(half.size()));
                int holes = 0;
                for (int dy = -4; dy < 4; ++dy) {
                    for (int dx = -4; dx < 4; ++dx) {
                        const Pixel p = ReadPixel(fbWidth / 2 + dx, fbHeight / 2 + dy);
                        holes += p.r > p.g + 10 ? 0 : 1;
                    }
                }
                Expect(holes >= 8, "one level alone at half a fade dithers away part of it (" + std::to_string(holes) +
                                       " of 64 not red)");
                runner::GLuint texture = greenLook;
                glDeleteTextures(1, &texture);
            }

            // No Terrains: the looks go at once; the uploads stay for the
            // grace period, then go.
            draws.clear();
            runner::AppendTerrainDraws({}, camera, 1.0, fades, chunkMeshes, renderer, draws);
            Expect(draws.empty(), "no Terrains draw nothing");
            Expect(chunkMeshes.terrainNodeCount() == view.chunks->size(),
                   "the node uploads stay a while after they stop drawing (" +
                       std::to_string(chunkMeshes.terrainNodeCount()) + ")");
            runner::AppendTerrainDraws({}, camera, 1.0 + runner::MeshCache::kTerrainNodeGraceSeconds, fades, chunkMeshes,
                                       renderer, draws);
            Expect(chunkMeshes.terrainNodeCount() == 0,
                   "and are deleted once not drawn for the grace period (" +
                       std::to_string(chunkMeshes.terrainNodeCount()) + " left)");
            chunkMeshes.clear();
            Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the red terrain ball leaves no GL error");
        }

        // The scenes, only with --terrain-shots dir: they take some seconds.
        if (!terrainShots.empty()) {
            // 1280 by 720 under a DynamicSky and its shadowing sun, each written to dir as a PNG.
            constexpr int kShotWidth = 1280;
            constexpr int kShotHeight = 720;
            OffscreenTarget target(kShotWidth, kShotHeight);
            Expect(target.complete(), "the 1280 by 720 offscreen target is complete");

            constexpr double kHours = 15.5;
            constexpr double kLatitude = 25.0;
            constexpr double kCloudCover = 0.35;
            constexpr double kCloudDensity = 0.45;
            const runner::SkyState state = runner::ComputeSky(kHours, kLatitude, 3.0, kCloudCover, kCloudDensity);
            runner::SceneLighting lighting;
            lighting.ambient[0] = lighting.ambient[1] = lighting.ambient[2] = 0.3f;
            lighting.dynamicSky.enabled = true;
            runner::SetSkyState(lighting.dynamicSky, state);
            lighting.dynamicSky.cloudCover = static_cast<float>(kCloudCover);
            lighting.dynamicSky.cloudDensity = static_cast<float>(kCloudDensity);
            lighting.dynamicSky.reflectionQuality = runner::SceneQuality::High;
            lighting.dynamicSky.key = {static_cast<float>(kHours), static_cast<float>(kLatitude),
                                       static_cast<float>(kCloudCover), static_cast<float>(kCloudDensity), 2};
            renderer.setLighting(lighting);
            runner::ShadowSettings shadowSettings;
            shadowSettings.cascadeSize = 4096;
            renderer.setShadowSettings(shadowSettings);
            std::printf("terrain sun: toward (%.2f, %.2f, %.2f), %.0f degrees around Y from +X toward +Z\n",
                        state.sun.x, state.sun.y, state.sun.z, std::atan2(state.sun.z, state.sun.x) * 180.0 / 3.14159265);

            const std::vector<float> lookBytes = LookBytes({
                {0, 150, 150, 150},  // a neutral default
                {1, 122, 110, 98},   // rock, gray-brown
                {2, 92, 142, 58},    // grass
                {3, 218, 196, 142},  // sand
                {4, 242, 245, 250},  // snow
                {5, 182, 92, 64},    // terracotta, for the seam's octants
                {6, 78, 112, 160},   // slate blue
            });
            const unsigned look = runner::MakeTerrainLookTexture(lookBytes.data());

            // draws (one per chunk) from shot, checked against the sky alone
            // and written to dir; returns the shot's pixels so a caller can
            // look closer (TX-R1..TX-R5's own pixel checks, below).
            const auto shootDraws = [&](const std::vector<runner::MeshDraw>& draws,
                                        const TerrainShot& shot) -> runner::ViewPixels {
                renderer.setCamera(ShotCamera(shot), shot.fov);
                runner::LightDraw sun = runner::SkyLightDraw(state, true);
                // The cascades reach past the subject, however far the camera stands.
                sun.shadowDistance = shot.distance * 2.f + 60.f;
                runner::ViewPixels sky;
                runner::ViewPixels pixels;
                const bool skyShot = target.shoot(renderer, {}, sun, sky);
                const bool drawn = target.shoot(renderer, draws, sun, pixels);
                Expect(skyShot && drawn, shot.file + " draws and reads back");
                Expect(pixels.width == kShotWidth && pixels.height == kShotHeight,
                       shot.file + " is 1280 by 720 (" + std::to_string(pixels.width) + " by " +
                           std::to_string(pixels.height) + ")");
                ExpectTerrainShown(shot.file, pixels, sky);
                if (drawn) {
                    std::filesystem::create_directories(terrainShots);
                    const std::filesystem::path file = terrainShots / shot.file;
                    std::ofstream(file, std::ios::binary) << runner::EncodePng(pixels);
                    std::printf("wrote %s (%d chunks)\n", file.string().c_str(), static_cast<int>(draws.size()));
                }
                return pixels;
            };
            const auto shoot = [&](const terrain::VoxelVolume& volume, std::initializer_list<TerrainShot> shots) {
                const auto chunks = UploadTerrain(volume);
                const std::vector<runner::MeshDraw> draws = TerrainDraws(chunks, look);
                for (const TerrainShot& shot : shots) {
                    shootDraws(draws, shot);
                }
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the terrain shots leave no GL error");
            };

            // The sun shines from about 160 degrees around Y (toward -X, a little
            // +Z) and 33 degrees up; each camera stands to one side of it.

            // (1) and (5): a floating rock ball island, grass on its crown and a
            // grassy knoll on top, over a wide sand flat that takes its shadow.
            {
                terrain::VoxelVolume volume;
                Edit(volume.fill(Block(0.f, -47.f, 0.f, 640.f, 4.f, 640.f), 3), "the sand flat");
                Edit(volume.fill(Ball(0.f, 0.f, 0.f, 22.f), 1), "the island");
                Edit(volume.paint(Ball(0.f, 16.f, 0.f, 17.f), 2), "the island's grass");
                Edit(volume.fill(Ball(5.f, 19.f, -3.f, 9.f), 2), "the knoll");
                shoot(volume, {
                                  TerrainShot{"terrain-1-ball-island.png", {0.f, 0.f, 0.f}, 115.f, 16.f, 85.f, 50.f},
                                  TerrainShot{"terrain-5-island-far.png", {10.f, -10.f, 0.f}, 200.f, 10.f, 210.f, 40.f},
                              });
            }

            // (2) Rolling hills: a wide rock slab with broad, low, overlapping
            // rises, grass on top, and a sandy hollow.
            {
                terrain::VoxelVolume volume;
                Edit(volume.fill(Block(0.f, -3.f, 0.f, 640.f, 6.f, 640.f), 1), "the slab");
                // x, y, z, radius: deep, wide balls, so only a low, gentle cap of each shows.
                const float hills[][4] = {{-40.f, -70.f, -30.f, 82.f}, {30.f, -76.f, -60.f, 90.f}, {60.f, -64.f, 30.f, 72.f},
                                          {-20.f, -66.f, 50.f, 74.f},  {-110.f, -70.f, 60.f, 84.f}, {120.f, -72.f, -90.f, 88.f},
                                          {-120.f, -74.f, -110.f, 90.f}, {130.f, -66.f, 110.f, 80.f}, {0.f, -76.f, -170.f, 90.f}};
                for (const auto& hill : hills) {
                    Edit(volume.fill(Ball(hill[0], hill[1], hill[2], hill[3]), 1), "a hill");
                }
                Edit(volume.paint(Block(0.f, 13.f, 0.f, 644.f, 30.f, 644.f), 2), "the grass");
                Edit(volume.subtract(Ball(18.f, 4.f, -6.f, 8.f)), "the hollow");
                Edit(volume.paint(Ball(18.f, 4.f, -6.f, 10.f), 3), "the hollow's sand");
                shoot(volume, {
                                  TerrainShot{"terrain-2-rolling-hills.png", {0.f, 0.f, 0.f}, 115.f, 14.f, 130.f, 50.f},
                                  TerrainShot{"terrain-2b-rolling-hills-low.png", {0.f, 8.f, 0.f}, 215.f, 8.f, 90.f, 55.f},
                              });
            }

            // (3) A cliff on a sand beach, its face toward -X (the sun's side):
            // an overhang along its top, a tunnel through it, a hollow in its
            // face, and snow on its crown.
            {
                terrain::VoxelVolume volume;
                Edit(volume.fill(Block(0.f, -2.f, 0.f, 120.f, 4.f, 120.f), 3), "the beach");
                Edit(volume.fill(Block(6.f, 18.f, 0.f, 24.f, 40.f, 44.f), 1), "the cliff");
                Edit(volume.fill(Block(-10.f, 34.f, 0.f, 12.f, 8.f, 44.f), 1), "the overhang");
                // Along X: the cylinder's Y axis turned onto X.
                const engine_core::Matrix4 tunnel = engine_core::matrix4_multiply(
                    engine_core::matrix4_translation(4.f, 8.f, 6.f),
                    engine_core::matrix4_axis_angle({0.f, 0.f, 1.f}, 3.14159265 / 2.0));
                Edit(volume.subtract(Cylinder(tunnel, 6.f, 40.f)), "the tunnel");
                Edit(volume.subtract(Ball(-6.f, 18.f, -12.f, 7.f)), "a hollow in the face");
                Edit(volume.paint(Block(0.f, 38.f, 0.f, 60.f, 8.f, 60.f), 4), "the snow");
                shoot(volume, {
                                  TerrainShot{"terrain-3-cliff-tunnel.png", {0.f, 16.f, 0.f}, 195.f, 10.f, 85.f, 50.f},
                                  TerrainShot{"terrain-3b-cliff-overhang-side.png", {0.f, 20.f, 0.f}, 125.f, 6.f, 80.f, 50.f},
                              });
            }

            // (4) A ball centered on a chunk corner, so it spans 8 chunks: each
            // octant (each chunk) painted its own Id, so the seams show as color
            // edges, and any crack as sky.
            {
                terrain::VoxelVolume volume;
                Edit(volume.fill(Ball(32.f, 32.f, 32.f, 11.f), 1), "the corner ball");
                const int ids[8] = {1, 2, 3, 5, 6, 4, 2, 1};
                for (int octant = 0; octant < 8; ++octant) {
                    const float sx = (octant & 1) != 0 ? 1.f : -1.f;
                    const float sy = (octant & 2) != 0 ? 1.f : -1.f;
                    const float sz = (octant & 4) != 0 ? 1.f : -1.f;
                    // Cells 32 to 43 or 20 to 31 on each axis: exactly one chunk's side.
                    Edit(volume.paint(Block(sx > 0 ? 37.5f : 25.5f, sy > 0 ? 37.5f : 25.5f, sz > 0 ? 37.5f : 25.5f,
                                            11.5f, 11.5f, 11.5f),
                                      static_cast<std::uint8_t>(ids[octant])),
                         "an octant's paint");
                }
                shoot(volume, {
                                   TerrainShot{"terrain-4-chunk-corner-seam.png", {32.f, 32.f, 32.f}, 100.f, 22.f, 32.f, 45.f},
                               });
            }

            // TX-R1..TX-R5 (Task 6): real triplanar, height-blended,
            // textured terrain, drawn with procedural stone/grass/sand
            // layers (ProceduralLayer, above) standing in for downloaded
            // textures, through the same renderer.terrainArrays incremental
            // upload a real Terrain uses (TerrainDraws.cpp).
            const auto Luma = [](const runner::ViewPixels& pixels, int x, int y) -> int {
                const unsigned char* p = pixels.rgba.data() + (static_cast<std::size_t>(y) * pixels.width + x) * 4;
                return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
            };
            // The average luma of a column over a band of rows centered on
            // centerY, smoothing out the procedural texture's own per-pixel
            // noise so only the material-level difference across a border shows.
            const auto ColumnLuma = [&](const runner::ViewPixels& pixels, int x, int centerY, int band) -> int {
                long sum = 0;
                int count = 0;
                for (int y = std::max(centerY - band, 0); y < std::min(centerY + band, pixels.height); ++y) {
                    sum += Luma(pixels, x, y);
                    ++count;
                }
                return count > 0 ? static_cast<int>(sum / count) : 0;
            };
            // The longest run of columns whose (smoothed) luma sits strictly
            // between the row's low and high plateaus: a border that blends
            // (TX-R1, TX-R3) has one spanning several pixels; a hard,
            // unblended step has none (span 0 or 1).
            const auto WidestTransition = [&](const runner::ViewPixels& pixels, int centerY, int band) -> int {
                int lowLuma = 255, highLuma = 0;
                std::vector<int> columns(static_cast<std::size_t>(pixels.width));
                for (int x = 0; x < pixels.width; ++x) {
                    columns[static_cast<std::size_t>(x)] = ColumnLuma(pixels, x, centerY, band);
                    lowLuma = std::min(lowLuma, columns[static_cast<std::size_t>(x)]);
                    highLuma = std::max(highLuma, columns[static_cast<std::size_t>(x)]);
                }
                const int lowThresh = lowLuma + (highLuma - lowLuma) / 10;
                const int highThresh = highLuma - (highLuma - lowLuma) / 10;
                int widest = 0;
                int runStart = -1;
                for (int x = 0; x < pixels.width; ++x) {
                    const bool inBand = columns[static_cast<std::size_t>(x)] > lowThresh &&
                                        columns[static_cast<std::size_t>(x)] < highThresh;
                    if (inBand) {
                        if (runStart < 0) {
                            runStart = x;
                        }
                        widest = std::max(widest, x - runStart + 1);
                    } else {
                        runStart = -1;
                    }
                }
                return widest;
            };
            // The smallest luma range (max - min) found in any window x
            // window block that lies inside the terrain silhouette (differs
            // from sky at its center by PixelSum): a real texture never
            // shows a window this flat, so a small result catches a locally
            // flat, pattern-less patch -- the "flat-shaded triangle" TX-R3
            // guards against -- wherever it falls, not just at a border.
            // Relative (range / mean), not absolute, contrast: a window in
            // shadow legitimately has a smaller absolute luma range than the
            // same texture lit -- shading multiplies albedo, so it scales
            // every value in the window by about the same factor, leaving
            // the *ratio* close to what full light would show. A window
            // near black (mean < 12) is skipped: too close to zero for a
            // ratio to mean anything, and a real texture does go briefly
            // dark at a true shadow edge or crevice.
            const auto SmallestLocalRange = [&](const runner::ViewPixels& pixels, const runner::ViewPixels& sky,
                                                int window) {
                double worst = 1.0e9;
                for (int y = 0; y + window <= pixels.height; y += window) {
                    for (int x = 0; x + window <= pixels.width; x += window) {
                        // Every corner (not just the center) differs from sky:
                        // keeps a window straddling the silhouette's own edge
                        // (part terrain, part background -- legitimately
                        // low-contrast where antialiasing blends the two) out
                        // of consideration, so only windows solidly inside the
                        // textured surface are checked.
                        const int corners[4][2] = {{x, y},
                                                   {x + window - 1, y},
                                                   {x, y + window - 1},
                                                   {x + window - 1, y + window - 1}};
                        bool allTerrain = true;
                        for (const auto& corner : corners) {
                            if (std::abs(PixelSum(pixels, corner[0], corner[1]) - PixelSum(sky, corner[0], corner[1])) <=
                                12) {
                                allTerrain = false;
                                break;
                            }
                        }
                        if (!allTerrain) {
                            continue;   // not solidly terrain here (sky, background, or the silhouette's own edge)
                        }
                        int lo = 255, hi = 0;
                        long sum = 0;
                        for (int wy = 0; wy < window; ++wy) {
                            for (int wx = 0; wx < window; ++wx) {
                                const int l = Luma(pixels, x + wx, y + wy);
                                lo = std::min(lo, l);
                                hi = std::max(hi, l);
                                sum += l;
                            }
                        }
                        const double mean = static_cast<double>(sum) / (window * window);
                        if (mean < 12.0) {
                            continue;   // too close to black for a ratio to mean anything
                        }
                        worst = std::min(worst, static_cast<double>(hi - lo) / mean);
                    }
                }
                return worst;
            };

            // TX-R1/TX-R2: three equal thirds of one floor -- untextured
            // (Id 3, layer 0), grass (Id 1, layer 1) and sand (Id 2, layer
            // 2) -- so the grass/sand border (TX-R1) and the untextured/
            // grass border (TX-R2) both show in one shot.
            {
                terrain::VoxelVolume volume;
                Edit(volume.fill(Block(0.f, -2.f, 0.f, 192.f, 4.f, 64.f), 3), "TX-R1/R2's floor");
                Edit(volume.paint(Block(-64.f, 1.f, 0.f, 64.f, 2.f, 64.f), 3), "its untextured third");
                Edit(volume.paint(Block(0.f, 1.f, 0.f, 64.f, 2.f, 64.f), 1), "its grass third");
                Edit(volume.paint(Block(64.f, 1.f, 0.f, 64.f, 2.f, 64.f), 2), "its sand third");
                const auto set = MakeTextureSet(256, {0, 1, 2}, 9101);
                unsigned texLook = runner::MakeTerrainLookTexture(
                    TexturedLookFloats({{1, 85, 150, 60, 2, 8.f}, {2, 215, 190, 150, 3, 8.f}, {3, 150, 150, 150, 0, 8.f}})
                        .data());
                const auto chunks = UploadTerrain(volume);
                std::vector<runner::MeshDraw> draws;
                for (int frame = 0; frame < 4; ++frame) {
                    draws = TexturedTerrainDraws(chunks, renderer, 9001, texLook, set);
                }
                const TerrainShot shot{"terrain-tx1-tx2-border.png", {0.f, 0.f, 0.f}, 90.f, 68.f, 95.f, 45.f};
                const runner::ViewPixels pixels = shootDraws(draws, shot);
                const int span = WidestTransition(pixels, pixels.height / 2, 60);
                Expect(span >= 6, "TX-R1: the grass/sand border blends over at least 6 px, not a staircase (" +
                                      std::to_string(span) + " px)");
                int blackCount = 0;
                for (int y = pixels.height * 3 / 10; y < pixels.height * 7 / 10; ++y) {
                    for (int x = 0; x < pixels.width; ++x) {
                        const unsigned char* p = pixels.rgba.data() + (static_cast<std::size_t>(y) * pixels.width + x) * 4;
                        if (p[0] < 4 && p[1] < 4 && p[2] < 4) {
                            ++blackCount;
                        }
                    }
                }
                Expect(blackCount == 0,
                       "TX-R2: an untextured Material beside a textured one draws no black pixels (" +
                           std::to_string(blackCount) + " found)");
                runner::DeleteTexture(texLook);
            }

            // TX-R3: three materials (stone floor, a grass ball and a sand
            // ball close enough to overlap it) meet inside one coarse
            // triangle: a smooth blend, not a flat-shaded triangle.
            {
                terrain::VoxelVolume volume(1.5f);
                Edit(volume.fill(Block(0.f, -4.f, 0.f, 48.f, 6.f, 48.f), 1), "TX-R3's stone floor");
                Edit(volume.fill(Ball(-2.f, 1.5f, 0.f, 5.f), 2), "a grass ball");
                Edit(volume.fill(Ball(3.f, 1.5f, 1.f, 5.f), 3), "a sand ball overlapping it");
                const auto set = MakeTextureSet(256, {0, 1, 2}, 9201);
                unsigned texLook = runner::MakeTerrainLookTexture(
                    TexturedLookFloats({{1, 122, 110, 98, 1, 10.f}, {2, 85, 150, 60, 2, 10.f}, {3, 215, 190, 150, 3, 10.f}})
                        .data());
                const auto chunks = UploadTerrain(volume);
                std::vector<runner::MeshDraw> draws;
                for (int frame = 0; frame < 4; ++frame) {
                    draws = TexturedTerrainDraws(chunks, renderer, 9002, texLook, set);
                }
                const TerrainShot shot{"terrain-tx3-blend.png", {0.f, 1.f, 0.f}, 60.f, 35.f, 22.f, 40.f};
                const runner::ViewPixels pixels = shootDraws(draws, shot);
                const int span = WidestTransition(pixels, pixels.height / 2, 30);
                Expect(span >= 6,
                       "TX-R3: three materials meeting in one triangle blend smoothly, not a flat-shaded step (" +
                           std::to_string(span) + " px)");
                // No locally flat, pattern-less patch anywhere on the ball
                // (fix round 1): catches it wherever the camera finds it,
                // not only at the border WidestTransition samples.
                renderer.setCamera(ShotCamera(shot), shot.fov);
                runner::LightDraw sun3 = runner::SkyLightDraw(state, true);
                sun3.shadowDistance = shot.distance * 2.f + 60.f;
                runner::ViewPixels sky3;
                target.shoot(renderer, {}, sun3, sky3);
                const double flatness = SmallestLocalRange(pixels, sky3, 28);
                Expect(flatness >= 0.07,
                       "TX-R3: no locally flat, pattern-less patch anywhere on the textured ball (worst 28x28 "
                       "relative contrast " +
                           std::to_string(flatness) + ")");
                runner::DeleteTexture(texLook);
            }

            // TX-R4: a vertical cliff's texture is not stretched: a patch of
            // flat floor and a patch of the cliff face, both the same stone
            // Material and about as far from the camera, show about the
            // same texel density (a triplanar projection, unlike a single
            // world-XZ UV, never stretches a vertical face).
            {
                terrain::VoxelVolume volume;
                Edit(volume.fill(Block(0.f, -2.f, 20.f, 60.f, 4.f, 40.f), 1), "TX-R4's floor");
                Edit(volume.fill(Block(0.f, 18.f, 0.f, 60.f, 40.f, 4.f), 1), "TX-R4's cliff");
                const auto set = MakeTextureSet(256, {0}, 9301);
                unsigned texLook =
                    runner::MakeTerrainLookTexture(TexturedLookFloats({{1, 122, 110, 98, 1, 8.f}}).data());
                const auto chunks = UploadTerrain(volume);
                std::vector<runner::MeshDraw> draws;
                for (int frame = 0; frame < 4; ++frame) {
                    draws = TexturedTerrainDraws(chunks, renderer, 9003, texLook, set);
                }
                const TerrainShot shot{"terrain-tx4-cliff-density.png", {0.f, 6.f, 8.f}, 90.f, 18.f, 26.f, 45.f};
                const runner::ViewPixels pixels = shootDraws(draws, shot);
                // A local-contrast proxy (mean |difference| between horizontal
                // neighbors) over a small window: higher for more texels per
                // pixel, for the same noise amplitude and scale.
                const auto Contrast = [&](int cx, int cy, int half) {
                    long total = 0;
                    int count = 0;
                    for (int y = cy - half; y < cy + half; ++y) {
                        for (int x = cx - half; x < cx + half - 1; ++x) {
                            total += std::abs(Luma(pixels, x + 1, y) - Luma(pixels, x, y));
                            ++count;
                        }
                    }
                    return count > 0 ? static_cast<double>(total) / count : 0.0;
                };
                const double floorContrast = Contrast(pixels.width / 2, pixels.height * 4 / 5, 24);
                const double cliffContrast = Contrast(pixels.width / 2, pixels.height / 3, 24);
                const double largest = std::max(floorContrast, cliffContrast);
                const double relativeDiff = largest > 1e-6 ? std::abs(floorContrast - cliffContrast) / largest : 0.0;
                Expect(relativeDiff <= 0.25,
                       "TX-R4: the cliff's texel density is within 25% of the floor's (floor " +
                           std::to_string(floorContrast) + ", cliff " + std::to_string(cliffContrast) + ")");
                runner::DeleteTexture(texLook);
            }

            // TX-R5: changing TextureSize mid-run never draws a frame
            // without terrain: the old array pair (padded to several layers,
            // so the new one takes a few frames to finish) keeps drawing
            // through every frame of the swap to a different size.
            {
                terrain::VoxelVolume volume;
                Edit(volume.fill(Block(0.f, -2.f, 0.f, 64.f, 4.f, 64.f), 1), "TX-R5's floor");
                unsigned texLook =
                    runner::MakeTerrainLookTexture(TexturedLookFloats({{1, 85, 150, 60, 1, 8.f}}).data());
                const auto chunks = UploadTerrain(volume);
                const auto setLarge = MakeTextureSet(256, {0, 1, 2, 0, 1, 2, 0, 1, 2}, 9401);
                const auto setSmall = MakeTextureSet(64, {0, 1, 2, 0, 1, 2, 0, 1, 2}, 9402);
                const TerrainShot shot{"terrain-tx5-texturesize-change.png", {0.f, 0.f, 0.f}, 90.f, 60.f, 70.f, 45.f};
                int frameIndex = 0;
                runner::ViewPixels lastFrame;
                const auto drawAndCheck = [&](const std::shared_ptr<const engine_core::TerrainTextureSet>& set) {
                    const std::vector<runner::MeshDraw> draws = TexturedTerrainDraws(chunks, renderer, 9004, texLook, set);
                    renderer.setCamera(ShotCamera(shot), shot.fov);
                    runner::LightDraw sun = runner::SkyLightDraw(state, true);
                    sun.shadowDistance = shot.distance * 2.f + 60.f;
                    runner::ViewPixels sky;
                    const bool skyShot = target.shoot(renderer, {}, sun, sky);
                    const bool drawn = target.shoot(renderer, draws, sun, lastFrame);
                    Expect(skyShot && drawn, "TX-R5 frame " + std::to_string(frameIndex) + " draws and reads back");
                    if (skyShot && drawn) {
                        ExpectTerrainShown("TX-R5 frame " + std::to_string(frameIndex), lastFrame, sky);
                    }
                    ++frameIndex;
                };
                for (int frame = 0; frame < 4; ++frame) {
                    drawAndCheck(setLarge);
                }
                for (int frame = 0; frame < 6; ++frame) {
                    drawAndCheck(setSmall);
                }
                std::filesystem::create_directories(terrainShots);
                std::ofstream(terrainShots / shot.file, std::ios::binary) << runner::EncodePng(lastFrame);
                std::printf("wrote %s\n", (terrainShots / shot.file).string().c_str());
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "TX-R5 leaves no GL error");
                runner::DeleteTexture(texLook);
            }

            // TX-R6/TX-R8 (Task 7, TX-R8 updated for the detail-fade smooth-
            // ramp fix): Lighting.TerrainQuality caps how many triplanar
            // projections a pixel uses (High <= 3, Medium <= 2, Low <= 1,
            // TX-R6), and far pixels cap at 2 even at High (TX-R8, forced
            // via SetTerrainDetailFadeOverride rather than a draw's
            // terrainLevel now that the per-node step is a per-pixel
            // view-distance ramp) -- both read through a test-only debug mode
            // (SetTerrainProjectionDebug) that, instead of the usual shaded
            // color, writes the pixel's active projection count as a flat
            // color into the emissive target alone (red 1, green 2, blue
            // 3), independent of lighting/shadows/tonemapping. A stone ball
            // on a stone floor gives every normal direction (so every
            // projection count) somewhere on one shot. FXAA is turned off
            // for every debug-mode shot in this scene: it would blend a
            // silhouette edge's debug color with the sky behind it, and
            // that blend can land on a third, wrong count by accident; the
            // real (non-debug) shaded render TX-R6 also takes is unaffected
            // either way, so it is left under the scene's usual FXAA.
            {
                terrain::VoxelVolume volume;
                Edit(volume.fill(Block(0.f, -4.f, 0.f, 48.f, 6.f, 48.f), 1), "TX-R6/R8's stone floor");
                Edit(volume.fill(Ball(0.f, 6.f, 0.f, 8.f), 1), "TX-R6/R8's stone ball");
                const auto set = MakeTextureSet(256, {0}, 9501);
                unsigned texLook =
                    runner::MakeTerrainLookTexture(TexturedLookFloats({{1, 122, 110, 98, 1, 8.f}}).data());
                const auto chunks = UploadTerrain(volume);
                std::vector<runner::MeshDraw> draws;
                for (int frame = 0; frame < 4; ++frame) {
                    draws = TexturedTerrainDraws(chunks, renderer, 9501, texLook, set);
                }
                const TerrainShot shot{"terrain-tx6-quality.png", {0.f, 6.f, 0.f}, 35.f, 22.f, 20.f, 50.f};

                const runner::SceneAntialiasing originalAA = lighting.antialiasing;

                // The real shaded render at each quality, exactly like any
                // other TX-R scene: never missing-texture black.
                struct QualityCase {
                    runner::SceneQuality quality;
                    const char* name;
                    int maxProjections;
                };
                const QualityCase cases[] = {
                    {runner::SceneQuality::Low, "Low", 1},
                    {runner::SceneQuality::Medium, "Medium", 2},
                    {runner::SceneQuality::High, "High", 3},
                };
                for (const QualityCase& qc : cases) {
                    lighting.terrainQuality = qc.quality;
                    lighting.antialiasing = originalAA;
                    renderer.setLighting(lighting);
                    renderer.setCamera(ShotCamera(shot), shot.fov);
                    runner::LightDraw realSun = runner::SkyLightDraw(state, true);
                    realSun.shadowDistance = shot.distance * 2.f + 60.f;
                    runner::ViewPixels realSky, realPixels;
                    const bool realSkyShot = target.shoot(renderer, {}, realSun, realSky);
                    const bool realDrawn = target.shoot(renderer, draws, realSun, realPixels);
                    Expect(realSkyShot && realDrawn, std::string("TX-R6 ") + qc.name + " draws and reads back");
                    if (realSkyShot && realDrawn) {
                        ExpectTerrainShown(std::string("TX-R6 ") + qc.name, realPixels, realSky);
                        if (qc.quality == runner::SceneQuality::High) {
                            std::filesystem::create_directories(terrainShots);
                            std::ofstream(terrainShots / shot.file, std::ios::binary) << runner::EncodePng(realPixels);
                            std::printf("wrote %s\n", (terrainShots / shot.file).string().c_str());
                        }
                    }

                    // The debug count render: every terrain pixel's active
                    // projection count, read back directly.
                    lighting.antialiasing = runner::SceneAntialiasing::None;
                    renderer.setLighting(lighting);
                    runner::SetTerrainProjectionDebug(true);
                    renderer.setCamera(ShotCamera(shot), shot.fov);
                    runner::LightDraw sun = runner::SkyLightDraw(state, true);
                    sun.shadowDistance = shot.distance * 2.f + 60.f;
                    runner::ViewPixels sky, pixels;
                    const bool skyShot = target.shoot(renderer, {}, sun, sky);
                    const bool drawn = target.shoot(renderer, draws, sun, pixels);
                    runner::SetTerrainProjectionDebug(false);
                    Expect(skyShot && drawn, std::string("TX-R6 ") + qc.name + " debug count draws and reads back");
                    if (skyShot && drawn) {
                        int maxCount = 0;
                        int terrainPixels = 0;
                        int blackCount = 0;
                        for (int y = 0; y < pixels.height; ++y) {
                            for (int x = 0; x < pixels.width; ++x) {
                                if (std::abs(PixelSum(pixels, x, y) - PixelSum(sky, x, y)) <= 12) {
                                    continue;   // sky/background, not terrain
                                }
                                ++terrainPixels;
                                const unsigned char* p =
                                    pixels.rgba.data() + (static_cast<std::size_t>(y) * pixels.width + x) * 4;
                                const int count = p[0] >= p[1] && p[0] >= p[2] ? 1 : (p[1] >= p[2] ? 2 : 3);
                                maxCount = std::max(maxCount, count);
                                if (p[0] < 8 && p[1] < 8 && p[2] < 8) {
                                    ++blackCount;
                                }
                            }
                        }
                        Expect(terrainPixels > 1000, std::string("TX-R6 ") + qc.name + " debug shot covers real " +
                                                          "terrain (" + std::to_string(terrainPixels) + " px)");
                        Expect(maxCount <= qc.maxProjections,
                               std::string("TX-R6 ") + qc.name + " uses at most " +
                                   std::to_string(qc.maxProjections) + " projections per pixel (saw " +
                                   std::to_string(maxCount) + ")");
                        Expect(blackCount == 0, std::string("TX-R6 ") + qc.name +
                                                     " never draws missing-texture black (" +
                                                     std::to_string(blackCount) + " black px)");
                    }
                }

                // TX-R8: the same debug mode, with the detail fade pinned
                // fully far (runner::SetTerrainDetailFadeOverride(1.0f)):
                // even at High quality's usual 3, it caps at 2. Before the
                // fix this forced a draw's terrainLevel to 2 instead -- a
                // far LOD node's level, the thing the old per-node step
                // read. The fix replaced that step with a per-pixel
                // view-distance ramp (uDetailFade0/uDetailFade1), which this
                // shot (a fixed 20 units away, well inside the ramp's near
                // end) would never reach on its own, so the override pins
                // the ramp itself instead -- same shot, same intent ("far
                // caps at 2 even at High"), now read through the mechanism
                // that replaced terrainLevel.
                lighting.terrainQuality = runner::SceneQuality::High;
                lighting.antialiasing = runner::SceneAntialiasing::None;
                renderer.setLighting(lighting);
                const std::vector<runner::MeshDraw>& lodDraws = draws;
                runner::SetTerrainProjectionDebug(true);
                runner::SetTerrainDetailFadeOverride(1.f);
                renderer.setCamera(ShotCamera(shot), shot.fov);
                runner::LightDraw lodSun = runner::SkyLightDraw(state, true);
                lodSun.shadowDistance = shot.distance * 2.f + 60.f;
                runner::ViewPixels lodSky, lodPixels;
                const bool lodSkyShot = target.shoot(renderer, {}, lodSun, lodSky);
                const bool lodDrawn = target.shoot(renderer, lodDraws, lodSun, lodPixels);
                runner::SetTerrainProjectionDebug(false);
                runner::SetTerrainDetailFadeOverride(-1.f);
                Expect(lodSkyShot && lodDrawn, "TX-R8 debug count (detail fade pinned far) draws and reads back");
                if (lodSkyShot && lodDrawn) {
                    int maxCount = 0;
                    int terrainPixels = 0;
                    for (int y = 0; y < lodPixels.height; ++y) {
                        for (int x = 0; x < lodPixels.width; ++x) {
                            if (std::abs(PixelSum(lodPixels, x, y) - PixelSum(lodSky, x, y)) <= 12) {
                                continue;
                            }
                            ++terrainPixels;
                            const unsigned char* p =
                                lodPixels.rgba.data() + (static_cast<std::size_t>(y) * lodPixels.width + x) * 4;
                            const int count = p[0] >= p[1] && p[0] >= p[2] ? 1 : (p[1] >= p[2] ? 2 : 3);
                            maxCount = std::max(maxCount, count);
                        }
                    }
                    Expect(terrainPixels > 1000,
                           "TX-R8 debug shot covers real terrain (" + std::to_string(terrainPixels) + " px)");
                    Expect(maxCount <= 2,
                           "TX-R8: fully far on the detail fade uses at most 2 projections even at High quality "
                           "(saw " +
                               std::to_string(maxCount) + ")");
                }

                lighting.terrainQuality = runner::SceneQuality::High;
                lighting.antialiasing = originalAA;
                renderer.setLighting(lighting);
                runner::DeleteTexture(texLook);
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "TX-R6/TX-R8 leave no GL error");
            }

            // TX-R7 (Task 7): anti-tiling breaks up the color array's own
            // repeat -- the fixed TextureScale period at which the whole
            // texture tile repeats -- without changing anything else: a
            // far, narrow-FOV ("telephoto") shot of one flat stone wall,
            // nearly orthographic across one row, has no strong narrow peak
            // in a simple DFT of that row at the tile's repeat frequency
            // with anti-tiling forced on; forcing it off (same scene, same
            // High quality otherwise) restores that peak, proving the check
            // would have caught it if anti-tiling did not work. Forced
            // through the test-only uAntiTilingOverride (SetTerrainAntiTilingOverride)
            // rather than toggling quality, so projections/height
            // blend/normals all stay fixed between the two shots -- only
            // anti-tiling differs.
            {
                terrain::VoxelVolume volume;
                Edit(volume.fill(Block(0.f, 20.f, 0.f, 4.f, 80.f, 300.f), 1), "TX-R7's stone wall");
                constexpr float kScale = 5.f;
                const auto set = MakeTextureSet(256, {0}, 9601);
                unsigned texLook =
                    runner::MakeTerrainLookTexture(TexturedLookFloats({{1, 122, 110, 98, 1, kScale}}).data());
                const auto chunks = UploadTerrain(volume);
                std::vector<runner::MeshDraw> draws;
                for (int frame = 0; frame < 4; ++frame) {
                    draws = TexturedTerrainDraws(chunks, renderer, 9601, texLook, set);
                }
                const TerrainShot shot{"terrain-tx7-antitiling.png", {0.f, 20.f, 0.f}, 0.f, 0.f, 400.f, 10.f};

                const runner::SceneAntialiasing originalAA = lighting.antialiasing;
                const runner::SceneQuality originalQuality = lighting.terrainQuality;
                lighting.terrainQuality = runner::SceneQuality::High;
                lighting.antialiasing = runner::SceneAntialiasing::None;
                renderer.setLighting(lighting);

                // A simple DFT (the brief's "FFT, or a simple DFT over one
                // row") of kWindow samples: magnitude per frequency bin.
                constexpr int kWindow = 512;
                constexpr double kPi = 3.14159265358979323846;
                const auto RowMagnitudes = [&](const runner::ViewPixels& pixels, int x0, int y) {
                    std::vector<double> samples(kWindow);
                    for (int i = 0; i < kWindow; ++i) {
                        samples[static_cast<std::size_t>(i)] = static_cast<double>(Luma(pixels, x0 + i, y));
                    }
                    std::vector<double> magnitude(kWindow / 2, 0.0);
                    for (int k = 1; k < kWindow / 2; ++k) {
                        double re = 0.0, im = 0.0;
                        for (int n = 0; n < kWindow; ++n) {
                            const double angle = 2.0 * kPi * k * n / kWindow;
                            re += samples[static_cast<std::size_t>(n)] * std::cos(angle);
                            im += samples[static_cast<std::size_t>(n)] * std::sin(angle);
                        }
                        magnitude[static_cast<std::size_t>(k)] = std::sqrt(re * re + im * im);
                    }
                    return magnitude;
                };
                // The fundamental's expected bin: one array-texture repeat
                // (TextureScale world units), at this near-orthographic
                // shot's own pixels-per-world-unit, over kWindow pixels --
                // a generous (half to double) band around it, since the
                // camera is only approximately orthographic.
                const double pixelsPerUnit =
                    kShotHeight / (2.0 * shot.distance * std::tan(shot.fov * kPi / 180.0 / 2.0));
                const double expectedPeriodPx = kScale * pixelsPerUnit;
                const int expectedBin = std::max(2, static_cast<int>(std::lround(kWindow / expectedPeriodPx)));
                const int bandLow = std::max(2, expectedBin / 2);
                const int bandHigh = std::min(kWindow / 2 - 1, expectedBin * 2);
                const auto PeakOverMedian = [&](const std::vector<double>& magnitude) {
                    std::vector<double> sorted(magnitude.begin() + 1, magnitude.end());
                    std::sort(sorted.begin(), sorted.end());
                    const double median = sorted[sorted.size() / 2];
                    double peak = 0.0;
                    for (int k = bandLow; k <= bandHigh; ++k) {
                        peak = std::max(peak, magnitude[static_cast<std::size_t>(k)]);
                    }
                    return median > 1e-6 ? peak / median : 0.0;
                };

                const auto shootRatio = [&](int forceAntiTiling, const char* label) {
                    runner::SetTerrainAntiTilingOverride(forceAntiTiling);
                    renderer.setCamera(ShotCamera(shot), shot.fov);
                    runner::LightDraw sun = runner::SkyLightDraw(state, true);
                    sun.shadowDistance = shot.distance * 2.f + 60.f;
                    runner::ViewPixels sky, pixels;
                    const bool skyShot = target.shoot(renderer, {}, sun, sky);
                    const bool drawn = target.shoot(renderer, draws, sun, pixels);
                    runner::SetTerrainAntiTilingOverride(-1);
                    Expect(skyShot && drawn, std::string("TX-R7 ") + label + " draws and reads back");
                    double ratio = 0.0;
                    if (skyShot && drawn) {
                        if (std::string(label) == "anti-tiling on") {
                            std::filesystem::create_directories(terrainShots);
                            std::ofstream(terrainShots / shot.file, std::ios::binary) << runner::EncodePng(pixels);
                            std::printf("wrote %s\n", (terrainShots / shot.file).string().c_str());
                        }
                        const int centerY = pixels.height / 2;
                        const int x0 = pixels.width / 2 - kWindow / 2;
                        const std::vector<double> magnitude = RowMagnitudes(pixels, x0, centerY);
                        ratio = PeakOverMedian(magnitude);
                    }
                    return ratio;
                };

                const double onRatio = shootRatio(1, "anti-tiling on");
                const double offRatio = shootRatio(0, "anti-tiling off");

                lighting.terrainQuality = originalQuality;
                lighting.antialiasing = originalAA;
                renderer.setLighting(lighting);

                Expect(onRatio < 2.0,
                       "TX-R7: anti-tiling on has no strong periodic peak at the repeat frequency (ratio " +
                           std::to_string(onRatio) + ", expected bin " + std::to_string(expectedBin) + ")");
                Expect(offRatio >= 2.0,
                       "TX-R7: anti-tiling off does have one, proving the check works (ratio " +
                           std::to_string(offRatio) + ")");
                runner::DeleteTexture(texLook);
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "TX-R7 leaves no GL error");
            }

            // TX-R9 (the detail-fade bug fix): terrain.frag's far falloffs
            // (mip bias, normal strength, the third triplanar projection)
            // used to switch on a draw's whole uNodeLevel >= 2, so two
            // neighboring chunks at the same camera distance could land on
            // opposite sides of a hard step purely because an unrelated LOD
            // decision gave them different levels -- a straight dark seam
            // across otherwise flat, continuous ground (the bug report: flat
            // grass, grazing toward the sun, a hard line at mid distance).
            // Reproduced directly, without the real LodTree: one flat grass
            // field, meshed at full detail (one MeshDraw per chunk, as
            // TexturedTerrainDraws gives it), each draw's terrainLevel set
            // by hand from its own chunk's distance to a fixed eye -- 0
            // inside kSeamDistance, 2 beyond it -- so the test fully
            // controls where the old step would land, independent of
            // whatever uDetailFade0/uDetailFade1 the fix's ramp uses.
            // Grazing, toward the DynamicSky's own sun (state, the same
            // light every other TX-R shot uses), at High quality (the only
            // one whose projection count itself used to fall at the old
            // step).
            {
                terrain::VoxelVolume volume;
                constexpr float kFieldHalf = 250.f;
                Edit(volume.fill(Block(kFieldHalf, -2.f, kFieldHalf, 2.f * kFieldHalf, 6.f, 2.f * kFieldHalf), 1),
                     "TX-R9's flat grass field");
                const auto set = MakeTextureSet(256, {1}, 9801);  // layer 1: grass
                unsigned texLook =
                    runner::MakeTerrainLookTexture(TexturedLookFloats({{1, 92, 142, 58, 1, 8.f}}).data());

                auto pairs = UploadTerrainWithCoords(volume);
                std::vector<std::unique_ptr<GpuMesh>> chunkMeshes;
                std::vector<terrain::ChunkCoord> coords;
                chunkMeshes.reserve(pairs.size());
                coords.reserve(pairs.size());
                for (auto& entry : pairs) {
                    coords.push_back(entry.first);
                    chunkMeshes.push_back(std::move(entry.second));
                }

                const engine_core::Vec3 eye{kFieldHalf, 4.f, kFieldHalf};
                const float sunAzimuth = std::atan2(state.sun.z, state.sun.x);
                const engine_core::Vec3 lookAt{eye.x + 220.f * std::cos(sunAzimuth), 1.5f,
                                                eye.z + 220.f * std::sin(sunAzimuth)};

                // Beyond this distance from eye, the old shader's uNodeLevel
                // >= 2 step kicked in at High quality; inside it, a chunk
                // drew at level 0. The exact number only has to land inside
                // the field and inside the shot's visible ground -- it need
                // not match uDetailFade0/uDetailFade1 (the fix never reads
                // terrainLevel for these falloffs any more).
                constexpr float kSeamDistance = 110.f;
                const float kChunkWorld = static_cast<float>(terrain::kChunkSize);
                std::vector<runner::MeshDraw> draws;
                for (int frame = 0; frame < 4; ++frame) {
                    draws = TexturedTerrainDraws(chunkMeshes, renderer, 9801, texLook, set);
                }
                for (std::size_t i = 0; i < draws.size() && i < coords.size(); ++i) {
                    const float cx = (static_cast<float>(coords[i].x) + 0.5f) * kChunkWorld;
                    const float cz = (static_cast<float>(coords[i].z) + 0.5f) * kChunkWorld;
                    const float dx = cx - eye.x;
                    const float dz = cz - eye.z;
                    const float distance = std::sqrt(dx * dx + dz * dz);
                    draws[i].terrainLevel = distance < kSeamDistance ? 0 : 2;
                }

                const runner::SceneQuality originalQuality9 = lighting.terrainQuality;
                const runner::SceneAntialiasing originalAA9 = lighting.antialiasing;
                lighting.terrainQuality = runner::SceneQuality::High;
                lighting.antialiasing = runner::SceneAntialiasing::None;  // no FXAA blending across the seam
                renderer.setLighting(lighting);
                const engine_core::Matrix4 cameraWorld = engine_core::matrix4_look_at(eye, lookAt, {0.f, 1.f, 0.f});
                renderer.setCamera(cameraWorld, 45.f);
                runner::LightDraw sun9 = runner::SkyLightDraw(state, true);
                sun9.shadowDistance = 500.f;
                runner::ViewPixels sky9, pixels9;
                const bool skyShot9 = target.shoot(renderer, {}, sun9, sky9);
                const bool drawn9 = target.shoot(renderer, draws, sun9, pixels9);
                Expect(skyShot9 && drawn9, "TX-R9 draws and reads back");
                if (skyShot9 && drawn9) {
                    ExpectTerrainShown("TX-R9", pixels9, sky9);
                    std::filesystem::create_directories(terrainShots);
                    std::ofstream(terrainShots / "terrain-tx9-detail-fade.png", std::ios::binary)
                        << runner::EncodePng(pixels9);
                    std::printf("wrote %s\n", (terrainShots / "terrain-tx9-detail-fade.png").string().c_str());

                    const auto luma = [](const runner::ViewPixels& pixels, int x, int y) -> int {
                        const unsigned char* p =
                            pixels.rgba.data() + (static_cast<std::size_t>(y) * pixels.width + x) * 4;
                        return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
                    };
                    // Row-averaged luminance (ground pixels only) along the
                    // view direction (image rows: a grazing look puts far
                    // ground near the top, nearer ground toward the
                    // bottom). No adjacent pair of ground rows should jump
                    // by much more than the profile's typical row-to-row
                    // step -- a hard seam shows up as one huge outlier step.
                    std::vector<double> rowLuma;
                    std::vector<int> rowY;
                    for (int y = 0; y < pixels9.height; ++y) {
                        double sum = 0.0;
                        int count = 0;
                        for (int x = 0; x < pixels9.width; ++x) {
                            if (std::abs(PixelSum(pixels9, x, y) - PixelSum(sky9, x, y)) <= 12) {
                                continue;
                            }
                            sum += luma(pixels9, x, y);
                            ++count;
                        }
                        if (count * 10 > pixels9.width * 7) {  // at least 70% ground in this row
                            rowLuma.push_back(sum / count);
                            rowY.push_back(y);
                        }
                    }
                    Expect(rowLuma.size() > 20, "TX-R9 has a wide enough ground band to measure (" +
                                                     std::to_string(rowLuma.size()) + " rows)");
                    if (rowLuma.size() > 2) {
                        std::vector<double> steps;
                        for (std::size_t i = 1; i < rowLuma.size(); ++i) {
                            steps.push_back(std::abs(rowLuma[i] - rowLuma[i - 1]));
                        }
                        std::vector<double> sortedSteps = steps;
                        std::sort(sortedSteps.begin(), sortedSteps.end());
                        const double median = sortedSteps[sortedSteps.size() / 2];
                        double worst = 0.0;
                        std::size_t worstAt = 0;
                        for (std::size_t i = 0; i < steps.size(); ++i) {
                            if (steps[i] > worst) {
                                worst = steps[i];
                                worstAt = i;
                            }
                        }
                        const double threshold = std::max(6.0, median * 6.0);
                        std::printf("TX-R9: median row step %.3f, worst %.3f at rows %d -> %d (threshold %.3f)\n",
                                    median, worst, rowY[worstAt], rowY[worstAt + 1], threshold);
                        Expect(worst <= threshold,
                               "TX-R9: no row-to-row luminance jump bigger than the detail fade's smooth ramp "
                               "allows (worst " +
                                   std::to_string(worst) + ", threshold " + std::to_string(threshold) + ")");
                    }
                }
                lighting.terrainQuality = originalQuality9;
                lighting.antialiasing = originalAA9;
                renderer.setLighting(lighting);
                runner::DeleteTexture(texLook);
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "TX-R9 leaves no GL error");
            }

            // Through the snapshot path, as the Scene View sees a place: a Game
            // with a Terrain in Workspace, edited through volume() (the Lua
            // Terrain:FillBall is not on this branch), a TerrainWorld settled on
            // it, a SnapshotPump wired with set_terrain_world, and the published
            // snapshot's terrains drawn by AppendTerrainDraws, as GameView's
            // collectMeshes draws them. This thread takes SimulationThread's
            // role for the Game, TerrainWorld and pump (no other thread touches
            // them; TerrainWorld's own mesher threads read only immutable
            // chunks), as sandbox/terrain_surface_tests.cpp's TS1 does.
            const auto shootSnapshot = [&](const auto& build, std::initializer_list<TerrainShot> shots) {
                engine_core::set_thread_role(engine_core::ThreadRole::Simulation);
                {
                    engine_core::Game game;
                    engine_core::Terrain& placed = game.create<engine_core::Terrain>();
                    game.set_parent(placed.id(), game.scene_service("Workspace"));
                    build(game, placed);
                    engine_core::TerrainWorld world;
                    // As TS1's settle, and on until every LOD level is built: until
                    // an update after the pool is idle publishes no new node set.
                    std::uint64_t settled = ~std::uint64_t{0};
                    for (int pass = 0; pass < 64; ++pass) {
                        world.update(game);
                        world.wait_idle();
                        world.update(game);
                        const std::uint64_t revision = world.views().empty() ? 0 : world.views()[0].nodes_revision;
                        if (revision == settled) {
                            break;
                        }
                        settled = revision;
                    }
                    engine_core::SnapshotPump pump;
                    pump.reserve(engine_core::DataModel::kMaxInstances);
                    pump.set_terrain_world(&world);
                    pump.prepare_copy(game);
                    pump.publish();
                    const engine_core::VisualSnapshot& snapshot = pump.front();
                    Expect(snapshot.terrains.size() == 1 && snapshot.terrains[0].chunks != nullptr &&
                               !snapshot.terrains[0].chunks->empty() && snapshot.terrains[0].look != nullptr,
                           "the snapshot carries the Terrain's chunks and look");
                    Expect(snapshot.terrains.size() == 1 && snapshot.terrains[0].nodes != nullptr &&
                               !snapshot.terrains[0].nodes->empty(),
                           "and its LOD nodes (top level " +
                               std::to_string(snapshot.terrains.empty() ? -1 : snapshot.terrains[0].top_level) + ")");
                    runner::MeshCache chunkMeshes;
                    std::vector<runner::MeshDraw> draws;
                    for (const TerrainShot& shot : shots) {
                        // Selected for this shot's camera on the 1280 x 720 target, with no fade from another.
                        runner::TerrainCamera camera;
                        camera.world = ShotCamera(shot);
                        camera.fov_y_degrees = shot.fov;
                        camera.pane_width = kShotWidth;
                        camera.pane_height = kShotHeight;
                        runner::TerrainFadeState fades;
                        draws.clear();
                        runner::AppendTerrainDraws(snapshot.terrains, camera, 0.0, fades, chunkMeshes, renderer, draws);
                        // Which levels it drew, for the log.
                        int levels[16] = {};
                        std::vector<runner::NodeChoice> choices;
                        runner::TerrainFadeState counting;
                        runner::SelectTerrainNodes(snapshot.terrains[0], camera, 0.0, counting, choices);
                        std::string histogram;
                        for (const runner::NodeChoice& choice : choices) {
                            ++levels[std::min((*snapshot.terrains[0].nodes)[choice.index].key.level, 15)];
                        }
                        for (int level = 0; level < 16; ++level) {
                            if (levels[level] > 0) {
                                histogram += " L" + std::to_string(level) + "x" + std::to_string(levels[level]);
                            }
                        }
                        const int shadowOnly = static_cast<int>(
                            std::count_if(draws.begin(), draws.end(),
                                          [](const runner::MeshDraw& draw) { return draw.shadowOnly; }));
                        std::printf("%s: %d nodes drawn (and %d casting shadows only) of %d published:%s\n",
                                    shot.file.c_str(), static_cast<int>(draws.size()) - shadowOnly, shadowOnly,
                                    static_cast<int>(snapshot.terrains[0].nodes->size()), histogram.c_str());
                        shootDraws(draws, shot);
                    }
                    // What a prefetch (or a switch) costs a node: its unpack and
                    // upload, timed to the GPU having it (a pixel read waits), each node
                    // into a cache that has none.
                    {
                        runner::MeshCache timing;
                        double compactMs = 0.0;
                        double meshMs = 0.0;
                        int compactCount = 0;
                        int meshCount = 0;
                        std::size_t compactTriangles = 0;
                        std::size_t meshTriangles = 0;
                        (void)ReadPixel(0, 0);
                        for (const engine_core::TerrainNodeView& node : *snapshot.terrains[0].nodes) {
                            const auto start = std::chrono::steady_clock::now();
                            if (node.compact != nullptr) {
                                timing.getTerrainNode(1, node.key, *node.compact, node.revision);
                            } else if (node.mesh != nullptr) {
                                timing.getTerrainNode(1, node.key, *node.mesh, node.revision);
                            }
                            (void)ReadPixel(0, 0);
                            const double took =
                                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                                    .count();
                            if (node.compact != nullptr) {
                                compactMs += took;
                                ++compactCount;
                                compactTriangles += (node.compact->indices.size() + node.compact->indices32.size()) / 3;
                            } else if (node.mesh != nullptr) {
                                meshMs += took;
                                ++meshCount;
                                meshTriangles += node.mesh->indices.size() / 3;
                            }
                        }
                        std::printf("node upload: %d compact nodes (levels >= 1) unpack + upload %.3f ms each, "
                                    "%zu triangles each; %d level-0 nodes upload %.3f ms each, %zu triangles each\n",
                                    compactCount, compactCount > 0 ? compactMs / compactCount : 0.0,
                                    compactCount > 0 ? compactTriangles / static_cast<std::size_t>(compactCount) : 0,
                                    meshCount, meshCount > 0 ? meshMs / meshCount : 0.0,
                                    meshCount > 0 ? meshTriangles / static_cast<std::size_t>(meshCount) : 0);
                        timing.clear();
                    }
                    draws.clear();
                    // No Terrains: the look goes, so the next Game's Terrain (the same
                    // id, a fresh TerrainWorld's look revisions) makes its own.
                    runner::TerrainFadeState none;
                    runner::AppendTerrainDraws({}, runner::TerrainCamera{}, 0.0, none, chunkMeshes, renderer, draws);
                    chunkMeshes.clear();
                    pump.set_terrain_world(nullptr);
                }
                engine_core::set_thread_role(engine_core::ThreadRole::Unknown);
                Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the snapshot terrain shots leave no GL error");
            };

            // As the studio check "workspace.Terrain:FillBall(Vector3.new(0, 10, 0), 8,
            // workspace.Terrain:AddMaterial(nil))" would make, over a floor of
            // the same Id so the ball's shadow shows: one TerrainMaterial with
            // no Material, so the default Material's look.
            shootSnapshot(
                [](engine_core::Game&, engine_core::Terrain& placed) {
                    engine_core::TerrainMaterial* entry = nullptr;
                    Expect(!placed.add_material(0, entry).has_value() && entry != nullptr &&
                               entry->material_id() == 1,
                           "AddMaterial(nil) takes Id 1");
                    Edit(placed.volume().fill(Block(0.f, -2.f, 0.f, 96.f, 4.f, 96.f), 1), "the default floor");
                    Edit(placed.volume().fill(Ball(0.f, 10.f, 0.f, 8.f), 1), "the default ball");
                },
                {TerrainShot{"terrain-snapshot-gray-ball.png", {0.f, 6.f, 0.f}, 70.f, 28.f, 60.f, 50.f}});

            // Two TerrainMaterials with their own Materials, a green floor and a
            // clay ball, on a Terrain moved 40 along X, so the snapshot's
            // Transform places the chunks.
            shootSnapshot(
                [](engine_core::Game& game, engine_core::Terrain& placed) {
                    Expect(!placed.set_transform(engine_core::matrix4_translation(40.f, 0.f, 0.f)).has_value(),
                           "the Terrain moves");
                    const auto material = [&game](const char* name, float r, float g, float b) {
                        engine_core::Material& made = game.create<engine_core::Material>();
                        game.set_name(made.id(), name);
                        game.set_parent(made.id(), game.service("Materials"));
                        engine_core::ColorRgb color;
                        color.r = r;
                        color.g = g;
                        color.b = b;
                        color.a = 1.f;
                        Expect(!made.set_color(color).has_value(), std::string(name) + " takes its color");
                        return made.id();
                    };
                    engine_core::TerrainMaterial* grass = nullptr;
                    engine_core::TerrainMaterial* clay = nullptr;
                    Expect(!placed.add_material(material("Grass", 0.33f, 0.55f, 0.22f), grass).has_value() &&
                               !placed.add_material(material("Clay", 0.78f, 0.40f, 0.25f), clay).has_value() &&
                               grass->material_id() == 1 && clay->material_id() == 2,
                           "Grass takes Id 1 and Clay Id 2");
                    Edit(placed.volume().fill(Block(0.f, -2.f, 0.f, 96.f, 4.f, 96.f), 1), "the grass floor");
                    Edit(placed.volume().fill(Ball(0.f, 10.f, 0.f, 8.f), 2), "the clay ball");
                    Edit(placed.volume().fill(Ball(-14.f, 2.f, 10.f, 5.f), 2), "a clay mound in the grass");
                },
                {TerrainShot{"terrain-snapshot-two-materials.png", {40.f, 6.f, 0.f}, 70.f, 28.f, 60.f, 50.f}});

            // Terrain LOD: the large island, its still shots and its sequence
            // checked against full detail, and with --terrain-lod-colors the
            // same shots tinted by level.
            if (lodColors) {
                std::string legend;
                for (int level = 0; level < 8; ++level) {
                    legend += std::string(level > 0 ? ", L" : "L") + std::to_string(level) + " " +
                              runner::kTerrainLodColors[level].name;
                }
                std::printf("terrain LOD level colors: %s\n", legend.c_str());
            }
            TerrainLodShots(renderer, target, kShotWidth, kShotHeight, lighting, state, terrainShots, lodColors);

            runner::GLuint texture = look;
            glDeleteTextures(1, &texture);
        }
        if (budgetMode) {
            TerrainBudget(renderer);
        }
        renderer.shutdown();
        Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the terrain checks leave no GL error");
    }

    // A Material's preview: a ball in its Color, lit from the upper left and
    // clear around it, drawn off screen, leaving the window's framebuffer as it was.
    {
        ide::MaterialBall ball(64, 32);
        runner::GLint viewportBefore[4] = {};
        glGetIntegerv(runner::GL_VIEWPORT, viewportBefore);
        ide::MaterialLook red;
        red.color = {1.f, 0.f, 0.f, 1.f};
        runner::ViewPixels pixels;
        Expect(ball.draw(red, pixels), "a context with nothing waiting draws the ball at once");
        Expect(pixels.width == 32 && pixels.height == 32 && pixels.rgba.size() == 32 * 32 * 4,
               "the ball comes back at the size asked");
        if (pixels.rgba.size() == 32 * 32 * 4) {
            auto at = [&pixels](int x, int y) { return pixels.rgba.data() + (y * 32 + x) * 4; };
            auto text = [](const unsigned char* p) {
                return std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + "," +
                       std::to_string(p[3]);
            };
            Expect(at(0, 0)[3] == 0 && at(31, 31)[3] == 0, "the corners are clear");
            const unsigned char* middle = at(16, 16);
            Expect(middle[3] == 255 && middle[0] > middle[1] + 20 && middle[0] > middle[2] + 20,
                   "the middle is the red ball (" + text(middle) + ")");
            const unsigned char* lit = at(11, 11);
            const unsigned char* shaded = at(21, 21);
            Expect(lit[0] > shaded[0] + 20, "its upper left is lit brighter than its lower right (" + text(lit) +
                                                " over " + text(shaded) + ")");
        }
        ide::MaterialLook glowing;
        glowing.color = {0.f, 0.f, 0.f, 1.f};
        glowing.emissive = {0.f, 1.f, 0.f, 1.f};
        runner::ViewPixels glow;
        ball.draw(glowing, glow);
        Expect(glow.rgba.size() == 32 * 32 * 4 && glow.rgba[(16 * 32 + 16) * 4 + 1] > glow.rgba[(16 * 32 + 16) * 4] + 40,
               "an Emissive glows on a black ball");
        runner::GLint viewportAfter[4] = {};
        glGetIntegerv(runner::GL_VIEWPORT, viewportAfter);
        runner::GLint framebuffer = -1;
        glGetIntegerv(runner::RT_GL_FRAMEBUFFER_BINDING, &framebuffer);
        Expect(framebuffer == 0 && std::equal(viewportBefore, viewportBefore + 4, viewportAfter),
               "the window's framebuffer and viewport are as they were");
        ball.release();
        Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the ball leaves no GL error");
    }

    {
        // RD2: a Dragger's handles at (0, 0, -10), seen from the origin down -Z
        // with a 90 degree view: the X shaft runs right of the middle, the Y
        // shaft up from it, each 30 points out at the middle of its length.
        runner::Renderer renderer;
        Expect(renderer.initialize(), "the renderer builds for the handles");
        renderer.setCamera(engine_core::matrix4_identity(), 90.f);
        engine_core::DraggerView view;
        view.camera = engine_core::matrix4_identity();
        view.fov_degrees = 90.f;
        view.size = engine_core::Vec2{static_cast<float>(kSize), static_cast<float>(kSize)};
        std::vector<engine_core::HandleVertex> mesh;
        engine_core::handle_mesh(engine_core::dragger_frame(engine_core::matrix4_translation(0.f, 0.f, -10.f), false),
                                 view, engine_core::DraggerHandle::None, engine_core::DraggerHandle::None, mesh);
        renderer.setHandles(mesh.data(), static_cast<int>(mesh.size()));
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, nullptr, 0);
        const Pixel right = ReadPixel(fbWidth * 94 / kSize, fbHeight / 2);
        const Pixel up = ReadPixel(fbWidth / 2, fbHeight * 94 / kSize);
        Expect(right.r > 150 && right.g < 100 && right.b < 100, "the X shaft draws red (" + Text(right) + ")");
        Expect(up.g > 150 && up.r < 150 && up.b < 150, "the Y shaft draws green (" + Text(up) + ")");
        Expect(IsClear(ReadPixel(2, 2)), "away from the handles is the clear color");
        renderer.setHandles(nullptr, 0);
        renderer.draw(0, 0, kSize, kSize, kSize, kSize, nullptr, 0);
        Expect(IsClear(ReadPixel(fbWidth * 94 / kSize, fbHeight / 2)), "no handles set, none drawn");
        Expect(runner::rt_glGetError() == runner::GL_NO_ERROR, "the handles leave no GL error");
        renderer.shutdown();
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    std::filesystem::remove_all(root);
    if (gFailures != 0) {
        std::fprintf(stderr, "%d scene render check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("scene render checks passed\n");
    return 0;
}
