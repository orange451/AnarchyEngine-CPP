#version 330 core
// The Skybox behind everything: its image where no opaque surface was drawn,
// added into the accumulation buffer so it is tone mapped with the rest.
// Renderer puts lighting.glsl and environment.glsl in after the #version line.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uDepth;
uniform sampler2D uSky;
// From view space to the sky's, with the camera's turn and the Rotation.
uniform mat3 uViewToSky;
// Exposure times Tint, linear.
uniform vec3 uSkyColor;

void main() {
    if (texture(uDepth, vUv).r < 1.0) {
        discard;
    }
    vec3 direction = uViewToSky * normalize(viewPositionAt(vUv, 1.0));
    vec2 uv = equirectUv(direction);
    // u jumps from 1 to 0 where the image's edges meet, and the jump would
    // pick its smallest mip there, a line down the sky. A second u that jumps
    // on the far side has a true slope here; take whichever is smaller
    // (Tarini, "Cylindrical and Toroidal Parameterizations Without Vertex Seams", 2012).
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    float u2 = fract(uv.x + 0.5) - 0.5;
    float dx2 = dFdx(u2);
    float dy2 = dFdy(u2);
    if (abs(dx2) + abs(dy2) < abs(dx.x) + abs(dy.x)) {
        dx.x = dx2;
        dy.x = dy2;
    }
    vec3 color = textureGrad(uSky, uv, dx, dy).rgb * uSkyColor;
    outColor = vec4(min(color, vec3(kMaxHalf)), 1.0);
}
