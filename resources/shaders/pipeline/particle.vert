#version 330 core
// One particle's quad, built here from its instance: placed, sized,
// stretched, turned, and faced as its emitter says (ParticleRenderer).
layout (location = 0) in vec2 aCorner;      // -0.5 .. 0.5
layout (location = 1) in vec3 iPosition;    // world
layout (location = 2) in float iSize;       // units
layout (location = 3) in vec4 iColor;       // straight RGBA
layout (location = 4) in vec3 iVelocity;    // world
layout (location = 5) in float iRotation;   // radians
layout (location = 6) in float iStretch;
layout (location = 7) in float iFrame;

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uCameraPosition;
uniform vec3 uCameraRight;
uniform vec3 uCameraUp;
uniform int uFacing;            // 0 Camera, 1 CameraUpright, 2 Velocity, 3 World
uniform vec3 uFacingAxis;       // world, unit
uniform float uVelocityStretch;
uniform vec2 uGrid;             // flipbook columns, rows
uniform bool uFlipY;            // the Texture's FlipY

out vec2 vUv;        // within the flipbook cell
out vec2 vCorner;    // -1 .. 1 across the quad, for the built-in dot
out vec4 vColor;
out vec3 vViewPosition;

void main() {
    // Stretch keeps the area: wider and shorter, or taller and thinner.
    float s = clamp(iStretch, -4.0, 4.0);
    vec2 corner = aCorner * vec2(exp(-0.5 * s), exp(0.5 * s)) * iSize;
    vec3 right;
    vec3 up;
    if (uFacing == 2) {
        // Long along the motion, across the view.
        float speed = length(iVelocity);
        vec3 along = speed > 1e-4 ? iVelocity / speed : uCameraUp;
        vec3 toCamera = normalize(uCameraPosition - iPosition);
        vec3 across = cross(along, toCamera);
        right = length(across) > 1e-4 ? normalize(across) : uCameraRight;
        up = along * (1.0 + speed * uVelocityStretch);
    } else if (uFacing == 3) {
        vec3 n = uFacingAxis;
        right = abs(n.y) < 0.9 ? normalize(cross(n, vec3(0.0, 1.0, 0.0))) : normalize(cross(n, vec3(1.0, 0.0, 0.0)));
        up = cross(n, right);
    } else if (uFacing == 1) {
        vec3 toCamera = uCameraPosition - iPosition;
        toCamera.y = 0.0;
        vec3 forward = length(toCamera) > 1e-4 ? normalize(toCamera) : vec3(0.0, 0.0, 1.0);
        right = normalize(cross(vec3(0.0, 1.0, 0.0), forward));
        up = vec3(0.0, 1.0, 0.0);
    } else {
        right = uCameraRight;
        up = uCameraUp;
    }
    if (uFacing != 2) {
        float c = cos(iRotation);
        float sn = sin(iRotation);
        corner = vec2(c * corner.x - sn * corner.y, sn * corner.x + c * corner.y);
    }
    vec3 world = iPosition + right * corner.x + up * corner.y;
    vec4 view = uView * vec4(world, 1.0);
    gl_Position = uProjection * view;
    vViewPosition = view.xyz;
    vColor = iColor;
    vCorner = aCorner * 2.0;
    float column = mod(iFrame, uGrid.x);
    float row = floor(iFrame / uGrid.x);
    // Frames run from the sheet's top row down, each the right way up. A
    // Texture's image has its top row at v 1, unless FlipY puts it at v 0.
    vec2 sheet = (vec2(column, row) + vec2(aCorner.x + 0.5, 0.5 - aCorner.y)) / uGrid;
    vUv = uFlipY ? sheet : vec2(sheet.x, 1.0 - sheet.y);
}
