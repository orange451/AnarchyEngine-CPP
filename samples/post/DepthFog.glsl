// Fog by distance from the camera, read from the depth buffer. Meant to run
// before bloom and the tone map (a ShaderEffect whose Shader has Hdr on), so
// the fog is light the tone map sees; the sky, at depth 1, is left alone.

uniform vec3 FogColor : color = vec3(0.86, 0.72, 0.56);
// How quickly the fog thickens past Start, per unit.
uniform float Density : range(0, 0.1) = 0.01;
// Units from the camera where the fog begins.
uniform float Start : range(0, 500) = 20.0;

void post() {
    float distance = depthAt(SCREEN_UV);
    if (distance > 1.0e8) {
        return;
    }
    float fog = 1.0 - exp(-max(distance - Start, 0.0) * Density);
    // Premultiplied: the fog's light is scaled by the pixel's coverage.
    COLOR.rgb = mix(COLOR.rgb, FogColor * COLOR.a, fog);
}
