#version 330 core
// Screen-space reflections, at half size (McGuire and Mara 2014): the
// reflected ray, stepped across the screen in strided pixels with depth
// interpolated perspective-correctly, then bisected at the step that hit.
// Writes the lit image at the hit, and how much it counts (alpha): 0 where
// nothing was hit, so the surface keeps its sky reflection. Renderer puts
// lighting.glsl, environment.glsl, image_lighting.glsl, and ssr.glsl in
// after the #version line.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uDepth;
uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform sampler2D uMaterial;
// The lit chain (ssr_scene.frag and its mips).
uniform sampler2D uSource;
uniform mat4 uProjection;
// The full-size buffers' size in pixels.
uniform vec2 uScreenSize;
uniform float uNear;
uniform float uMaxDistance;
uniform float uMaxRoughness;
uniform float uChainLevels;

// Interleaved gradient noise (Jimenez 2014): a different offset at each pixel, with no texture.
float noise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// How far in front of the camera the depth buffer's surface is at uv.
float sceneDepthAt(vec2 uv) {
    return -viewPositionAt(uv, texture(uDepth, uv).r).z;
}

void main() {
    outColor = vec4(0.0);
    float depth = texture(uDepth, vUv).r;
    if (depth >= 1.0) {
        return;
    }
    vec3 material = texture(uMaterial, vUv).rgb;
    float roughness = material.y;
    float fade = roughnessFade(roughness, uMaxRoughness);
    if (fade <= 0.0) {
        return;
    }
    vec3 origin = viewPositionAt(vUv, depth);
    vec3 viewDirection = normalize(origin);
    vec3 N = texture(uNormal, vUv).rgb;
    vec3 albedo = texture(uAlbedo, vUv).rgb;
    SkyReflection sky = skyReflection(viewDirection, N, albedo, material.x, roughness, material.z, vec3(1.0), vec3(1.0));
    if (all(lessThan(sky.weight, vec3(0.02)))) {
        return;
    }
    vec3 dir = reflect(viewDirection, N);
    fade *= facingFade(dir.z);
    if (fade <= 0.0) {
        return;
    }

    // Clip the ray to the near plane and MaxDistance, and project both ends.
    float rayLength = (origin.z + dir.z * uMaxDistance > -uNear) ? (-uNear - origin.z) / dir.z : uMaxDistance;
    vec3 end = origin + dir * rayLength;
    vec4 h0 = uProjection * vec4(origin, 1.0);
    vec4 h1 = uProjection * vec4(end, 1.0);
    float k0 = 1.0 / h0.w;
    float k1 = 1.0 / h1.w;
    float qz0 = origin.z * k0;
    float qz1 = end.z * k1;
    vec2 p0 = (h0.xy * k0 * 0.5 + 0.5) * uScreenSize;
    vec2 p1 = (h1.xy * k1 * 0.5 + 0.5) * uScreenSize;
    if (dot(p1 - p0, p1 - p0) < 1e-4) {
        p1 += vec2(0.01);
    }
    // Step along the longer screen axis, a pixel stride at a time.
    vec2 delta = p1 - p0;
    bool permute = abs(delta.x) < abs(delta.y);
    if (permute) {
        delta = delta.yx;
        p0 = p0.yx;
        p1 = p1.yx;
    }
    float stepDir = sign(delta.x);
    float invdx = stepDir / delta.x;
    float stride = max(1.0, abs(delta.x) / float(kReflectionMaxSteps));
    vec2 dp = vec2(stepDir, delta.y * invdx) * stride;
    float dqz = (qz1 - qz0) * invdx * stride;
    float dk = (k1 - k0) * invdx * stride;
    float jitter = noise(gl_FragCoord.xy);
    vec2 p = p0 + dp * jitter;
    float qz = qz0 + dqz * jitter;
    float k = k0 + dk * jitter;
    float endX = p1.x * stepDir;

    bool hit = false;
    vec2 previousP = p;
    float previousQz = qz;
    float previousK = k;
    for (int i = 0; i < kReflectionMaxSteps; ++i) {
        previousP = p;
        previousQz = qz;
        previousK = k;
        p += dp;
        qz += dqz;
        k += dk;
        if (p.x * stepDir > endX) {
            break;
        }
        vec2 uv = (permute ? p.yx : p) / uScreenSize;
        if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
            break;
        }
        float rayDepth = -qz / k;
        float sceneDepth = sceneDepthAt(uv);
        float thickness = max(0.05, sceneDepth * 0.03);
        if (rayDepth >= sceneDepth && rayDepth <= sceneDepth + thickness) {
            hit = true;
            break;
        }
    }
    if (!hit) {
        return;
    }
    // Bisect between the last step in front and the first behind.
    vec2 a = previousP;
    float aQz = previousQz;
    float aK = previousK;
    for (int i = 0; i < kReflectionRefineSteps; ++i) {
        vec2 midP = (a + p) * 0.5;
        float midQz = (aQz + qz) * 0.5;
        float midK = (aK + k) * 0.5;
        vec2 uv = (permute ? midP.yx : midP) / uScreenSize;
        if (-midQz / midK >= sceneDepthAt(uv)) {
            p = midP;
            qz = midQz;
            k = midK;
        } else {
            a = midP;
            aQz = midQz;
            aK = midK;
        }
    }
    vec2 hitUv = (permute ? p.yx : p) / uScreenSize;
    vec3 hitPosition = viewPositionAt(hitUv, texture(uDepth, hitUv).r);
    float hitDistance = length(hitPosition - origin);
    // Pixels of the lit chain's level 0 (half size) per stud at the hit.
    float pixelsPerUnit = uProjection[1][1] * 0.25 * uScreenSize.y / max(-hitPosition.z, uNear);
    float level = coneLevel(roughness, hitDistance, pixelsPerUnit, uChainLevels);
    fade *= edgeFade(hitUv) * distanceFade(hitDistance, uMaxDistance);
    outColor = vec4(textureLod(uSource, hitUv, level).rgb, fade);
}
