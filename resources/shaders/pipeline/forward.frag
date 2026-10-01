#version 330 core
// A see-through surface, lit whole in one pass and blended over what is
// behind it (the legacy forward.frag). Renderer puts surface.glsl and
// lighting.glsl in after the #version line.
in vec3 vViewPosition;
in vec3 vViewNormal;
in vec2 vUv;
in vec4 vColor;

out vec4 outColor;

uniform vec3 uAmbient;
uniform vec3 uSkyRadiance;
// The Material's Transparency, 0 to 1.
uniform float uTransparency;

// The lights, in view space. Renderer::kMaxForwardLights.
const int kMaxLights = 32;
uniform int uLightCount;
// xyz position, w Radius.
uniform vec4 uLightPositionRadius[kMaxLights];
// rgb Color, a Intensity.
uniform vec4 uLightColorIntensity[kMaxLights];
// xyz where a SpotLight points, w unused.
uniform vec4 uLightDirection[kMaxLights];
// x, y the outer and inner cosines; x below -1.5 for a PointLight.
uniform vec4 uLightCone[kMaxLights];

void main() {
    Surface s = readSurface(vViewPosition, vViewNormal, vUv, vColor, gl_FrontFacing);
    if (s.alpha < 0.1) {
        discard;
    }
    vec3 viewDirection = normalize(vViewPosition);
    vec3 color = ambientLight(viewDirection, s.normal, s.albedo, s.metalness, s.roughness, s.reflectivity, uAmbient,
                              uSkyRadiance);
    for (int i = 0; i < uLightCount && i < kMaxLights; ++i) {
        color += shadeLight(s.normal, vViewPosition, s.albedo, s.metalness, s.roughness, uLightPositionRadius[i].xyz,
                            uLightDirection[i].xyz, uLightCone[i].xy, uLightColorIntensity[i].rgb,
                            uLightPositionRadius[i].w, uLightColorIntensity[i].a);
    }
    color += s.emissive;
    outColor = vec4(color, s.alpha * (1.0 - uTransparency));
}
