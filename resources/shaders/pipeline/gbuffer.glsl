// The slim G-buffer's normal packing, shared by every pass that writes or
// reads it. No #version: Renderer puts it in after the main file's.
//
// The targets, 16 bytes a pixel:
//   0 albedo    SRGB8_ALPHA8   linear rgb in, linear rgb out; the hardware
//                              stores it in sRGB, so darks keep their steps
//   1 normal    RG16           a view-space unit normal, octahedral
//   2 material  RGBA8          metalness, roughness, reflectivity
//   3 emissive  R11F_G11F_B10F linear, unbounded

vec2 octWrap(vec2 v) {
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// A unit normal folded onto the octahedron and into 0..1.
vec4 encodeNormal(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.z >= 0.0 ? n.xy : octWrap(n.xy);
    return vec4(e * 0.5 + 0.5, 0.0, 1.0);
}

// What encodeNormal wrote, back to a unit normal.
vec3 decodeNormal(vec4 texel) {
    vec2 e = texel.xy * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
