#pragma once

// The Atmosphere's uniforms, as atmosphere.glsl declares them, shared by
// every program that includes it: Renderer's merge and see-through programs
// and ParticleRenderer's. Renderer works out one AtmosphereFrame a draw.
// UI thread, GL context current.
namespace runner {

// atmosphere.glsl's uniforms, worked out once a draw. The defaults draw no air.
struct AtmosphereFrame {
    bool enabled = false;
    // Extinction per unit at BaseHeight, and how fast it thins going up, per unit.
    float sigma0 = 0.f;
    float falloff = 0.f;
    // The camera's height above BaseHeight.
    float height = 0.f;
    // World up, in view space.
    float up[3] = {0.f, 1.f, 0.f};
    // Color, linear.
    float color[3] = {0.f, 0.f, 0.f};
    float aerialPerspective = 0.f;
    float skyHaze = 0.f;
    float skyDistance = 0.f;
    // Toward the light that glows through the air, view space, and its
    // linear color times its intensity times SunScatter; black for none.
    float lightDirection[3] = {0.f, 1.f, 0.f};
    float lightColor[3] = {0.f, 0.f, 0.f};
    // The sky's prefiltered cube, from view space into its frame, what its
    // texels are multiplied by, and its last mip. skyReady false leaves the
    // air its own Color.
    bool skyReady = false;
    float viewToSky[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    float skyColor[3] = {1.f, 1.f, 1.f};
    float skyMaxLod = 0.f;
    unsigned skyCube = 0;
};

// Where a program keeps atmosphere.glsl's uniforms; -1 for any it lacks.
struct AtmosphereLocations {
    int enabled = -1;
    int sigma0 = -1;
    int falloff = -1;
    int height = -1;
    int up = -1;
    int color = -1;
    int aerialPerspective = -1;
    int skyHaze = -1;
    int skyDistance = -1;
    int lightDirection = -1;
    int lightColor = -1;
    int skyReady = -1;
    int viewToSky = -1;
    int skyColor = -1;
    int skyMaxLod = -1;
    int sky = -1;
};

// program's locations, looked up once after it links.
AtmosphereLocations FindAtmosphere(unsigned program);

// Uploads frame into the program in use at where, and binds its cube to
// skyUnit, a unit the program samples nothing else from (or the same cube).
// With the air off it uploads only uAtmosphereEnabled.
void UploadAtmosphere(const AtmosphereLocations& where, const AtmosphereFrame& frame, int skyUnit);

}  // namespace runner
