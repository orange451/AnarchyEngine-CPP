#pragma once

#include "AtmosphereUniforms.hpp"

namespace runner {

// A pass's program and the uniforms Renderer sets on it. -1 for one the
// program does not use.
struct RenderProgram {
    unsigned id = 0;
    int model = -1;
    int view = -1;
    int projection = -1;
    int viewProjection = -1;
    int inverseProjection = -1;
    int inverseView = -1;
    int inverseViewProjection = -1;
    int texel = -1;
    int keepAlpha = -1;
    int ambient = -1;
    int skyRadiance = -1;
    // Skybox (image_lighting.glsl, sky.frag, merge.frag).
    int skyEnabled = -1;
    int viewToSky = -1;
    int skyColor = -1;
    int skyLightScale = -1;
    int prefilteredMaxLod = -1;
    // merge.frag only: whether the sky pass drew into the accumulation
    // buffer this frame, which is not always uSkyEnabled (a DynamicSky
    // draws even when its lighting cube, uSkyEnabled's cubesReady, is not).
    int skyDrawn = -1;
    // Material.
    int color = -1;
    int emissive = -1;
    int metalness = -1;
    int roughness = -1;
    int reflectivity = -1;
    int normalMapEnabled = -1;
    int emissiveMapEnabled = -1;
    int transparency = -1;
    // terrain.frag: a LOD node's cross-fade.
    int fade = -1;
    int fadeIn = -1;
    // terrain.frag: SetTerrainLodColors' level, or -1 for none.
    int lodLevel = -1;
    // terrain.frag: whether a per-Terrain array pair is bound (Task 6),
    // and Lighting.TerrainQuality (0 Low, 1 Medium, 2 High).
    int hasSurface = -1;
    int terrainQuality = -1;
    // terrain.frag: the view-space distance band the far falloffs (mip
    // bias, normal strength, the third triplanar projection) ramp
    // across, replacing the old per-node-level step -- see terrain.frag's
    // uDetailFade0/uDetailFade1 comment.
    int detailFade0 = -1;
    int detailFade1 = -1;
    // terrain.frag, test-only (Task 7): SetTerrainAntiTilingOverride's
    // value (-1 follows quality as usual; 0/1 forces anti-tiling off/on,
    // isolating it from quality's other falloffs for TX-R7), and
    // SetTerrainProjectionDebug's switch (1 replaces the shaded color
    // with the active triplanar projection count, for TX-R6/TX-R8).
    int antiTilingOverride = -1;
    int projectionDebug = -1;
    int detailFadeOverride = -1;
    // One light (light.frag).
    int lightPosition = -1;
    int lightDirection = -1;
    int lightCone = -1;
    int lightColor = -1;
    int lightRadius = -1;
    int lightIntensity = -1;
    // One light's shadow (shadow.glsl).
    int shadowKind = -1;
    int shadowMatrix = -1;
    int shadowCascadeCount = -1;
    int shadowLight = -1;
    int shadowNearFar = -1;
    int shadowTexel = -1;
    int shadowTexelUv = -1;
    int shadowTiles = -1;
    int shadowFaceScale = -1;
    // Every light (forward.frag).
    int lightCount = -1;
    int lightPositionRadius = -1;
    int lightColorIntensity = -1;
    int lightDirections = -1;
    int lightCones = -1;
    // Tone map.
    int exposure = -1;
    int inverseGamma = -1;
    // Lighting.ToneMapping is Cinematic.
    int cinematic = -1;
    // Lighting.ShadingModel is Fast (lighting.glsl).
    int fastShading = -1;
    int saturation = -1;
    // Bloom (bloom_down.frag, bloom_up.frag) and its mix in the tone map.
    int prefilter = -1;
    int threshold = -1;
    int radius = -1;
    int bloomIntensity = -1;
    int bloomLevelScale = -1;
    int bloomThreshold = -1;
    // Screen-space reflections (ssr.frag, merge.frag).
    int screenSize = -1;
    // A surface shader's TIME.
    int time = -1;
    int nearPlane = -1;
    int maxDistance = -1;
    int maxRoughness = -1;
    int chainLevels = -1;
    // ssr_blur.frag: the level of its source the taps read.
    int sourceLevel = -1;
    int reflectionsEnabled = -1;
    int reflectionsIntensity = -1;
    // Ambient occlusion (gtao.frag, ao_blur.frag, occlusion.glsl).
    int occlusionRadius = -1;
    int projectionScale = -1;
    int occlusionScale = -1;
    int slices = -1;
    int blurDirection = -1;
    int blurRadius = -1;
    int occlusionEnabled = -1;
    int occlusionIntensity = -1;
    // geometry.vert's billboards (MeshDraw::billboard).
    int billboard = -1;
    int cardBand = -1;
    int probeTexel = -1;
    // DynamicSky (procedural_sky.glsl).
    int face = -1;
    int sunDirection = -1;
    int moonDirection = -1;
    int starFrame = -1;
    int starVisibility = -1;
    int starClock = -1;
    int bodyLightDirection = -1;
    int bodyLightColor = -1;
    int sunColor = -1;
    int moonColor = -1;
    int cloudCover = -1;
    int cloudDensity = -1;
    int cloudOffset = -1;
    int sunSize = -1;
    int moonSize = -1;
    int sunTextureEnabled = -1;
    int moonTextureEnabled = -1;
    // dynamic_sky.frag: whether skyLayersPass drew this frame's air and clouds.
    int skyLayersDrawn = -1;
    // atmosphere.glsl's, in the programs that include it.
    AtmosphereLocations atmosphere;
};

}  // namespace runner
