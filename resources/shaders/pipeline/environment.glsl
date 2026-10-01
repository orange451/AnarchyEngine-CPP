// The Skybox: where a direction falls on its equirectangular image, the
// directions of a cube's faces, and the sampling the environment cubes are
// built with. No #version: Renderer puts it in after the main file's. Sky
// directions are world directions turned by the Skybox's Rotation, Y up.

const float kPi = 3.14159265359;
// Half float's largest value, so a bright sun does not overflow a 16-bit buffer.
const float kMaxHalf = 65000.0;

// u runs once around, with the image's middle straight down -Z and +X to its
// right; v runs from the bottom (straight down) to the top (straight up).
vec2 equirectUv(vec3 direction) {
    vec3 d = normalize(direction);
    return vec2(0.5 + atan(d.x, -d.z) / (2.0 * kPi), 0.5 + asin(clamp(d.y, -1.0, 1.0)) / kPi);
}

// The direction through uv (0 to 1) on cube face 0 to 5, in OpenGL's order
// +X, -X, +Y, -Y, +Z, -Z, as a samplerCube looks it up. Not unit length.
vec3 cubeDirection(int face, vec2 uv) {
    vec2 c = uv * 2.0 - 1.0;
    if (face == 0) {
        return vec3(1.0, -c.y, -c.x);
    }
    if (face == 1) {
        return vec3(-1.0, -c.y, c.x);
    }
    if (face == 2) {
        return vec3(c.x, 1.0, c.y);
    }
    if (face == 3) {
        return vec3(c.x, -1.0, -c.y);
    }
    if (face == 4) {
        return vec3(c.x, -c.y, 1.0);
    }
    return vec3(-c.x, -c.y, -1.0);
}

// The i-th of count points spread evenly over the unit square.
vec2 hammersley(uint i, uint count) {
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return vec2(float(i) / float(count), float(bits) * 2.3283064365386963e-10);
}

// Two unit vectors at right angles to N and to each other.
void basis(vec3 N, out vec3 T, out vec3 B) {
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

// A half vector around N, drawn as often as GGX with alpha a (roughness
// squared) makes it.
vec3 importanceSampleGGX(vec2 xi, vec3 N, float a) {
    float phi = 2.0 * kPi * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3 T;
    vec3 B;
    basis(N, T, B);
    return normalize(T * (cos(phi) * sinTheta) + B * (sin(phi) * sinTheta) + N * cosTheta);
}

float ggx(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

// The mip of a cube size texels wide whose texels cover what one of count
// samples drawn with probability pdf covers (Colbert and Křivánek, GPU Gems 3,
// ch. 20), so a bright spot is averaged in rather than hit or missed.
float sampleLod(float pdf, float count, float size) {
    float sampleAngle = 1.0 / (count * max(pdf, 1e-6));
    float texelAngle = 4.0 * kPi / (6.0 * size * size);
    return max(0.5 * log2(sampleAngle / texelAngle) + 1.0, 0.0);
}
