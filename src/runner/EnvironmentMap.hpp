#pragma once

#include <cstdint>

namespace runner {

// A Skybox's light, made on the GPU from its equirectangular images for
// split-sum image-based lighting (Karis, "Real Shading in Unreal Engine 4",
// 2013): an irradiance cube a surface's diffuse light is read from, a
// prefiltered cube whose mips are its reflection at each roughness, and the
// lookup table of how much a surface reflects. The cubes are made again only
// when an image changes, never for Exposure, Rotation, or Tint, which the
// shaders apply as they read them.
//
// Each image is a 2D RGBA16F texture, mipmapped, as TextureCache's
// getEnvironment uploads it, and a revision that changes only when it does.
// Every call but the destructor needs the GL context initialize ran in.
class EnvironmentMap {
public:
    // The environment cube the image is drawn into first, which the other
    // two are filtered from, and their sizes, each a face's width in texels.
    static constexpr int kEnvironmentSize = 512;
    static constexpr int kIrradianceSize = 32;
    // Mip 0 is roughness 0 and the last mip roughness 1.
    static constexpr int kPrefilteredSize = 256;
    static constexpr int kPrefilteredLevels = 6;
    static constexpr int kBrdfSize = 256;

    EnvironmentMap() = default;
    EnvironmentMap(const EnvironmentMap&) = delete;
    EnvironmentMap& operator=(const EnvironmentMap&) = delete;

    // False when a shader does not build.
    bool initialize();
    // Deletes the programs and every texture.
    void shutdown();

    // Makes the cubes for image, when it is not what they were made from: the
    // irradiance and the reflections, both from image. emptyVao is a vertex
    // array with nothing bound, for full-screen triangles. True when the cubes are ready. False
    // when a pass cannot draw yet, as with render buffers made this frame on
    // macOS: nothing is drawn with half-made cubes, and the next call tries
    // again. Leaves the framebuffer, viewport, program, and texture unit 0
    // changed, blending, depth testing, and culling off, and cube maps seamless.
    bool update(unsigned image, std::uint64_t imageRevision, unsigned emptyVao);

    // False when update can never make the cubes: initialize failed, or the
    // driver would not render into them.
    bool available() const { return equirect_.id != 0 && !refused_; }

    // Cube maps, and the lookup table: 2D, NdotV across and roughness up,
    // scale in red and bias in green.
    unsigned irradiance() const { return irradiance_; }
    unsigned prefiltered() const { return prefiltered_; }
    unsigned brdf() const { return brdf_; }
    static constexpr float prefilteredMaxLod() { return static_cast<float>(kPrefilteredLevels - 1); }

private:
    struct Program {
        unsigned id = 0;
        int face = -1;
        int roughness = -1;
        int environmentSize = -1;
    };

    bool buildProgram(Program& program, const char* name, const char* fragment);
    // Makes the textures and the framebuffer the first time. False when the
    // driver will not render into them.
    bool ensureTextures();
    // Draws program over each face of cube's level, size texels wide. False,
    // with nothing drawn, when it cannot draw yet.
    bool drawFaces(const Program& program, unsigned cube, int level, int size, unsigned emptyVao);
    // The image into environment_, with its mips. False as drawFaces.
    bool drawEnvironment(unsigned image, unsigned emptyVao);

    Program equirect_;
    Program irradianceProgram_;
    Program prefilter_;
    Program brdfProgram_;
    unsigned framebuffer_ = 0;
    unsigned environment_ = 0;
    unsigned irradiance_ = 0;
    unsigned prefiltered_ = 0;
    unsigned brdf_ = 0;
    bool brdfDrawn_ = false;
    // What the cubes were made from; 0 before they were.
    std::uint64_t imageRevision_ = 0;
    bool refused_ = false;
};

}  // namespace runner
