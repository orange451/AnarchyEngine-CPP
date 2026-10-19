#version 330 core
// A particle's color, lit by the scene's lights as a cloud is (no surface,
// so no facing term), faded where it meets the scene, and written
// premultiplied into the see-through buffer merge.frag composites, seen
// through the Atmosphere's air. ParticleRenderer puts atmosphere.glsl in
// after the #version line.
const int kMaxLights = 32;
const float kInvPi = 0.31830989;

in vec2 vUv;
in vec2 vCorner;
in vec4 vColor;
in vec3 vViewPosition;

uniform sampler2D uTexture;
uniform bool uHasTexture;
uniform sampler2D uSceneDepth;      // a copy of the scene's depth
uniform vec2 uTargetSize;
uniform mat4 uInverseProjection;
uniform float uSoftEdge;            // units; 0 is off
uniform float uBrightness;
uniform float uLightInfluence;
uniform int uBlend;                 // 0 Normal, 1 Additive, 2 Multiply
uniform vec3 uAmbient;
uniform int uLightCount;
uniform vec4 uLightPositionRadius[kMaxLights];   // view space
uniform vec4 uLightColorIntensity[kMaxLights];
uniform vec4 uLightDirection[kMaxLights];        // view space
uniform vec4 uLightCone[kMaxLights];

out vec4 outColor;

float sceneDistance(vec2 uv) {
    float depth = texture(uSceneDepth, uv).r;
    vec4 view = uInverseProjection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return -view.z / view.w;
}

// What one light gives a point in a cloud: lighting.glsl's falloff and cone, with no surface.
vec3 lightAt(int i, vec3 P) {
    vec2 cone = uLightCone[i].xy;
    vec3 color = uLightColorIntensity[i].rgb * uLightColorIntensity[i].a;
    if (cone.x < -3.0) {
        return color;   // a DirectionalLight reaches everything
    }
    vec3 toLight = uLightPositionRadius[i].xyz - P;
    float distance = length(toLight);
    float radius = uLightPositionRadius[i].w;
    if (distance <= 0.0 || distance >= radius) {
        return vec3(0.0);
    }
    float ratio = distance / radius;
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    float attenuation = window * window / max(distance * distance, 0.0625);
    float spot = 1.0;
    if (cone.x > -1.5) {
        spot = smoothstep(cone.x, cone.y, dot(-toLight / distance, uLightDirection[i].xyz));
    }
    return color * attenuation * spot;
}

void main() {
    vec4 tex = uHasTexture ? texture(uTexture, vUv) : vec4(1.0, 1.0, 1.0, smoothstep(1.0, 0.0, length(vCorner)));
    vec4 c = tex * vColor;
    if (uSoftEdge > 0.0) {
        float gap = sceneDistance(gl_FragCoord.xy / uTargetSize) - (-vViewPosition.z);
        c.a *= clamp(gap / uSoftEdge, 0.0, 1.0);
    }
    if (c.a <= 0.0) {
        discard;
    }
    if (uBlend == 2) {
        // Multiply: darken what is behind by the color's lightness, as a black over.
        float lightness = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
        // Far off, the air between hides the darkening.
        float through = atmosphereAtPoint(vViewPosition).a;
        outColor = vec4(0.0, 0.0, 0.0, c.a * (1.0 - clamp(lightness, 0.0, 1.0)) * through);
        return;
    }
    vec3 light = uAmbient;
    for (int i = 0; i < uLightCount && i < kMaxLights; ++i) {
        light += lightAt(i, vViewPosition) * kInvPi;
    }
    vec3 rgb = c.rgb * mix(vec3(1.0), light, uLightInfluence) * uBrightness;
    // Normal takes on the air like a surface; Additive only dims, adding no
    // air of its own, so a far fire fades out rather than turning gray.
    vec4 air = atmosphereAtPoint(vViewPosition);
    rgb = uBlend == 1 ? rgb * air.a : rgb * air.a + air.rgb;
    // Premultiplied: Normal covers what is behind; Additive adds and covers nothing.
    outColor = vec4(rgb * c.a, uBlend == 1 ? 0.0 : c.a);
}
