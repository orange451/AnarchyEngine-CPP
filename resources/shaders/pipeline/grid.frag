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
// Normalized device coordinates back to world space: the inverse view times
// the inverse projection.
uniform mat4 uInverseViewProjection;

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

// How much of the pixel a rule covers: 1 on a line, 0 a pixel or more from
// one. cells is the floor position in cells of the rule, cellPixels a cell's
// size in pixels along X and Z.
float rule(vec2 cells, vec2 cellPixels) {
    vec2 gap = abs(fract(cells + 0.5) - 0.5) * cellPixels;
    return 1.0 - clamp(min(gap.x, gap.y), 0.0, 1.0);
}

// View-space depth of the point at ndc and device depth z: only the inverse
// projection's last two rows matter.
float viewDepth(vec2 ndc, float z) {
    vec4 point = vec4(ndc, z, 1.0);
    mat4 m = uInverseProjection;
    return dot(vec4(m[0][2], m[1][2], m[2][2], m[3][2]), point) / dot(vec4(m[0][3], m[1][3], m[2][3], m[3][3]), point);
}

void main() {
    vec2 ndc = vUv * 2.0 - 1.0;
    // The pixel's ray, from the near plane (device depth -1) to the far one (1),
    // in homogeneous world space: center + z * along at device depth z. One
    // matrix product; along is the inverse's third column.
    vec4 center = uInverseViewProjection * vec4(ndc, 0.0, 1.0);
    vec4 along = uInverseViewProjection[2];
    // Where it meets the floor, Y = 0: the device depth that zeroes Y. Between
    // the near and far planes is in front of the camera too: anything behind
    // it lands past 1.
    float floorZ = abs(along.y) > 1e-9 * abs(center.y) ? -center.y / along.y : 2.0;
    bool hit = floorZ > -1.0 && floorZ <= 1.0;
    // Kept finite off the floor, so the derivatives below stay sane at the horizon.
    vec4 onFloor = center + clamp(floorZ, -1.0, 1.0) * along;
    vec2 coord = onFloor.xz / onFloor.w;

    // World units per pixel along X and Z. Taken before anything is thrown
    // out, since derivatives need every pixel of the quad.
    vec2 dx = dFdx(coord);
    vec2 dy = dFdy(coord);
    vec2 pixel = sqrt(dx * dx + dy * dy);

    // How much of the grid survives the fades toward the horizon (farther with
    // the camera higher, and where the ray only grazes the floor) and surfaces
    // in front of the floor, found first: where none does, nothing else is.
    vec3 eye = uInverseView[3].xyz;
    float reach = clamp(abs(eye.y) * 60.0, 60.0, 600.0);
    float away = length(coord - eye.xz);
    float shown = 1.0 - smoothstep(0.25 * reach, reach, away);
    vec3 nearWorld = (center.xyz - along.xyz) / (center.w - along.w);
    vec3 farWorld = (center.xyz + along.xyz) / (center.w + along.w);
    shown *= smoothstep(0.0, 0.08, abs(normalize(farWorld - nearWorld).y));
    // A surface lying on the floor still shows the grid, rather than fighting it.
    float depth = texture(uDepth, vUv).r;
    if (depth < 1.0 && -viewDepth(ndc, depth * 2.0 - 1.0) < -viewDepth(ndc, floorZ) * 0.999) {
        shown = 0.0;
    }
    // Nothing to draw blends nothing. Not a discard: drawn on GridBands' bands,
    // every pixel then marks the depth buffer early, and is shaded once.
    if (!hit || !(shown > 0.0)) {
        fragColor = vec4(kLineColor, 0.0);
        return;
    }

    vec2 perPixel = 1.0 / max(pixel, vec2(1e-6));
    float lod = max(0.0, log2(length(pixel) * kMinCellPixels) * (1.0 / log2(10.0)) + 1.0);
    float level = floor(lod);
    float fade = lod - level;
    // The finest rule's cells, 10^level units, and the two coarser ones', in
    // cells along the floor and pixels across.
    float cell0 = pow(10.0, level);
    vec2 cells0 = coord * (1.0 / cell0);
    vec2 cellPixels0 = cell0 * perPixel;
    // Each rule eases toward the weight of the one it becomes at the next
    // level: the finest fades out, the middle dims to minor, the coarsest is major.
    float alpha = max(rule(cells0, cellPixels0) * kMinorAlpha * (1.0 - fade),
                      max(rule(cells0 * 0.1, cellPixels0 * 10.0) * mix(kMajorAlpha, kMinorAlpha, fade),
                          rule(cells0 * 0.01, cellPixels0 * 100.0) * kMajorAlpha));
    vec3 color = kLineColor;

    // The X axis is the line z = 0, the Z axis x = 0.
    vec2 onAxis = 1.0 - clamp(abs(coord.yx) * perPixel.yx - 0.5 * kAxisWidth + 0.5, 0.0, 1.0);
    float onX = onAxis.x;
    float onZ = onAxis.y;
    color = mix(color, kAxisXColor, onX);
    alpha = max(alpha, onX);
    color = mix(color, kAxisZColor, onZ * (1.0 - onX * 0.5));
    alpha = max(alpha, onZ);
    fragColor = vec4(color, alpha * shown);
}
