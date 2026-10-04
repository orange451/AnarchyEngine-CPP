#version 330 core
// The editor's floor grid, as Blender draws it: the world's Y = 0 plane, ruled
// every 1, 10, and 100 units, the finest rule fading in and out as the camera
// nears and leaves so no cell is ever much smaller than a few pixels. The X
// axis runs along it in red and the Z axis in blue. Drawn over the tone mapped
// pane, straight alpha; a surface nearer the camera than the plane hides it.
// Usually run only on bands around the lines (GridBands, grid_band.vert), so
// pixels far from any line are never shaded.
in vec2 vUv;
out vec4 fragColor;

// The scene's depth, 1 where no opaque surface was drawn.
uniform sampler2D uDepth;
uniform mat4 uInverseProjection;
// The camera's world: view space back to world space.
uniform mat4 uInverseView;

const vec3 kLineColor = vec3(0.5);
const float kMinorAlpha = 0.3;
const float kMajorAlpha = 0.55;
// Blender's theme colors for X and its floor's second axis, here Z.
const vec3 kAxisXColor = vec3(1.0, 0.2, 0.322);
const vec3 kAxisZColor = vec3(0.157, 0.565, 1.0);
// The finest rule fades out as its cells shrink to this many pixels.
const float kMinCellPixels = 5.0;
// Pixels across an axis line.
const float kAxisWidth = 1.5;

// How much of the pixel a rule every cell units covers: 1 on a line, 0 a
// pixel or more from one.
float rule(vec2 coord, vec2 pixel, float cell) {
    vec2 gap = abs(fract(coord / cell + 0.5) - 0.5) * cell / max(pixel, vec2(1e-6));
    return 1.0 - clamp(min(gap.x, gap.y), 0.0, 1.0);
}

vec3 viewPoint(vec2 ndc, float z) {
    vec4 point = uInverseProjection * vec4(ndc, z, 1.0);
    return point.xyz / point.w;
}

void main() {
    vec2 ndc = vUv * 2.0 - 1.0;
    // The pixel's ray from the near plane to the far one, where it meets the floor.
    vec3 nearView = viewPoint(ndc, -1.0);
    vec3 farView = viewPoint(ndc, 1.0);
    vec3 nearWorld = (uInverseView * vec4(nearView, 1.0)).xyz;
    vec3 farWorld = (uInverseView * vec4(farView, 1.0)).xyz;
    float rise = farWorld.y - nearWorld.y;
    float t = abs(rise) > 1e-6 ? -nearWorld.y / rise : -1.0;
    bool hit = t > 0.0 && t <= 1.0;
    // Kept finite off the floor, so the derivatives below stay sane at the horizon.
    vec3 point = mix(nearWorld, farWorld, clamp(t, 0.0, 1.0));
    vec2 coord = point.xz;

    // World units per pixel along X and Z. Taken before anything is thrown
    // out, since derivatives need every pixel of the quad.
    vec2 pixel = vec2(length(vec2(dFdx(coord.x), dFdy(coord.x))), length(vec2(dFdx(coord.y), dFdy(coord.y))));
    float lod = max(0.0, log(length(pixel) * kMinCellPixels) / log(10.0) + 1.0);
    float fade = fract(lod);
    float cell0 = pow(10.0, floor(lod));
    float cell1 = cell0 * 10.0;
    float cell2 = cell1 * 10.0;
    // Each rule eases toward the weight of the one it becomes at the next
    // level: the finest fades out, the middle dims to minor, the coarsest is major.
    float alpha = max(rule(coord, pixel, cell0) * kMinorAlpha * (1.0 - fade),
                      max(rule(coord, pixel, cell1) * mix(kMajorAlpha, kMinorAlpha, fade),
                          rule(coord, pixel, cell2) * kMajorAlpha));
    vec3 color = kLineColor;

    // The X axis is the line z = 0, the Z axis x = 0.
    float onX = 1.0 - clamp(abs(coord.y) / max(pixel.y, 1e-6) - 0.5 * kAxisWidth + 0.5, 0.0, 1.0);
    float onZ = 1.0 - clamp(abs(coord.x) / max(pixel.x, 1e-6) - 0.5 * kAxisWidth + 0.5, 0.0, 1.0);
    color = mix(color, kAxisXColor, onX);
    alpha = max(alpha, onX);
    color = mix(color, kAxisZColor, onZ * (1.0 - onX * 0.5));
    alpha = max(alpha, onZ);

    // Fades toward the horizon: farther with the camera higher, and where the
    // ray only grazes the floor.
    vec3 eye = uInverseView[3].xyz;
    float reach = clamp(abs(eye.y) * 60.0, 60.0, 600.0);
    float away = length(coord - eye.xz);
    alpha *= 1.0 - smoothstep(0.25 * reach, reach, away);
    vec3 direction = normalize(farWorld - nearWorld);
    alpha *= smoothstep(0.0, 0.08, abs(direction.y));

    // Hidden behind any surface in front of the floor. A surface lying on it
    // still shows the grid, rather than fighting it.
    float depth = texture(uDepth, vUv).r;
    if (depth < 1.0) {
        float sceneDistance = -viewPoint(ndc, depth * 2.0 - 1.0).z;
        float gridDistance = -mix(nearView, farView, t).z;
        if (sceneDistance < gridDistance * 0.999) {
            alpha = 0.0;
        }
    }
    // Nothing to draw blends nothing. Not a discard: drawn on GridBands' bands,
    // every pixel then marks the depth buffer early, and is shaded once.
    if (!hit) {
        alpha = 0.0;
    }
    fragColor = vec4(color, max(alpha, 0.0));
}
