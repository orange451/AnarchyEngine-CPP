// Darkens the corners of the frame, as a lens does. Runs after the tone map
// on display colors: a ShaderEffect under Lighting with Hdr off.

uniform float Strength : range(0, 1) = 0.5;
uniform float Radius : range(0, 1) = 0.75;

void post() {
    float d = length(SCREEN_UV - 0.5) / 0.7071;
    COLOR.rgb *= 1.0 - Strength * smoothstep(Radius, 1.0, d);
}
