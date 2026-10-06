#pragma once

#include "SkyMath.hpp"

#include <cstdint>
#include <functional>

namespace runner {

// A sky's light, made on the GPU for split-sum image-based lighting (Karis,
// "Real Shading in Unreal Engine 4", 2013): an irradiance cube a surface's
// diffuse light is read from, a prefiltered cube whose mips are its
// reflection at each roughness, and the lookup table of how much a surface
// reflects. A Skybox's cubes are made again only when an image changes, never
// for Exposure, Rotation, or Tint, which the shaders apply as they read them.
//
// A Skybox's image is a 2D RGBA16F texture, mipmapped, as TextureCache's
// getEnvironment uploads it, and a revision that changes only when it does. A
// DynamicSky draws the environment cube itself, through startProcedural and
// continueProcedural, a slice a frame, into spare cubes that take the place of
// the ones in use once they are finished.
// Every call but the destructor needs the GL context initialize ran in.
class EnvironmentMap {
public:
    // An image's cube sizes. A procedural sky picks its own (updateProcedural).
    static constexpr int kEnvironmentSize = 512;
    static constexpr int kIrradianceSize = 32;
    // Mip 0 is roughness 0 and the last mip roughness 1.
    static constexpr int kPrefilteredSize = 256;
    static constexpr int kPrefilteredLevels = 6;
    static constexpr int kBrdfSize = 256;
    // Samples per texel. A DynamicSky is filtered again every few frames and
    // gets by with fewer: each sample reads a blurrier mip the fewer there
    // are, so a bright spot is still averaged in.
    static constexpr int kIrradianceSamples = 512;
    static constexpr int kPrefilterSamples = 256;
    static constexpr int kProceduralIrradianceSamples = 128;
    static constexpr int kProceduralPrefilterSamples = 64;
    // From mip 3, rough enough that a sample covers many texels.
    static constexpr int kProceduralRoughSamples = 32;

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
    // Starts making the cubes again from a sky the caller draws, at these
    // sizes; the cubes are made again when they differ. False when they
    // cannot be made. A redraw already under way starts over.
    bool startProcedural(int environmentSize, int prefilteredSize);
    // Whether a redraw startProcedural began is not finished yet.
    bool drawingProcedural() const { return slice_ >= 0; }
    // Draws the redraw's next slice (LightingSliceAt) into the spare cubes,
    // or every slice left when the cubes in use hold no procedural sky to
    // show meanwhile. drawFace(face) draws face 0 to 5 (OpenGL's order) of
    // the environment cube into the bound framebuffer with the viewport set,
    // binding its own program the first time in a call; false when it cannot
    // draw yet. True when the redraw finished and the cubes in use are the
    // new ones. False otherwise, leaving the cubes in use as they are; a slice
    // that cannot draw yet is tried again next call. Leaves GL state as update.
    bool continueProcedural(unsigned emptyVao, const std::function<bool(int face)>& drawFace);
    // Whether the cubes in use hold a procedural sky.
    bool holdsProcedural() const { return procedural_; }

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
        int samples = -1;
    };

    bool buildProgram(Program& program, const char* name, const char* fragment);
    // Makes the cubes at these sizes, and the lookup table and framebuffer the
    // first time. False when the driver will not render into them.
    bool ensureTextures(int environmentSize, int prefilteredSize);
    // The lookup table, once. False when it cannot draw yet.
    bool ensureBrdf(unsigned emptyVao);
    // Draws program over each face of cube's level, size texels wide. False,
    // with nothing drawn, when it cannot draw yet.
    bool drawFaces(const Program& program, unsigned cube, int level, int size, unsigned emptyVao);
    // The image into environment_, with its mips. False as drawFaces.
    bool drawEnvironment(unsigned image, unsigned emptyVao);
    // irradiance_ and prefiltered_ from environment_. False as drawFaces.
    bool filter(unsigned emptyVao);
    // irradiance from environment_'s mips, with samples per texel.
    bool filterIrradiance(unsigned irradiance, int samples, unsigned emptyVao);
    // The mips of prefiltered whose bits are set in levels, from environment_'s
    // mips, with a procedural sky's sample counts or an image's.
    bool filterLevels(unsigned prefiltered, unsigned levels, bool procedural, unsigned emptyVao);
    // One slice of a procedural redraw, into the spare cubes.
    bool drawSlice(const LightingSlice& slice, unsigned emptyVao, const std::function<bool(int face)>& drawFace);

    Program equirect_;
    Program irradianceProgram_;
    Program prefilter_;
    Program brdfProgram_;
    unsigned framebuffer_ = 0;
    unsigned environment_ = 0;
    unsigned irradiance_ = 0;
    unsigned prefiltered_ = 0;
    unsigned brdf_ = 0;
    // A procedural redraw's, swapped with irradiance_ and prefiltered_ when it finishes.
    unsigned spareIrradiance_ = 0;
    unsigned sparePrefiltered_ = 0;
    // The procedural redraw's next slice, -1 with none under way.
    int slice_ = -1;
    bool brdfDrawn_ = false;
    // What the cubes were made from; 0 before they were.
    std::uint64_t imageRevision_ = 0;
    bool refused_ = false;
    // The cubes' sizes, 0 before they are made.
    int environmentSize_ = 0;
    int prefilteredSize_ = 0;
    // Whether the cubes in use hold a procedural sky rather than an image.
    bool procedural_ = false;
};

}  // namespace runner
