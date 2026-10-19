#version 330 core
// The DynamicSky behind everything, where no opaque surface was drawn, added
// into the accumulation buffer so it is tone mapped with the rest. Its air
// and clouds come from dynamic_sky_layers.frag at half size; the stars, the
// discs, and the horizon are drawn here at full size. Renderer puts
// lighting.glsl, environment.glsl, and procedural_sky.glsl in after the
// #version line.
in vec2 vUv;
out vec4 outColor;

uniform sampler2D uDepth;
// From view space to the world's: a DynamicSky does not turn.
uniform mat3 uViewToSky;
// The DynamicSky's brightness against the picture proceduralSky draws, the same each channel.
uniform vec3 uSkyColor;
// skyLayers' air and premultiplied clouds at half size, when uSkyLayersDrawn
// is 1; at 0 (their buffers could not be made) this draws them itself.
uniform sampler2D uSkyAir;
uniform sampler2D uSkyCloud;
uniform float uSkyLayersDrawn;

void main() {
    if (texture(uDepth, vUv).r < 1.0) {
        discard;
    }
    vec3 direction = normalize(uViewToSky * viewPositionAt(vUv, 1.0));
    vec3 air;
    vec4 cloud;
    if (uSkyLayersDrawn > 0.5) {
        air = texture(uSkyAir, vUv).rgb;
        // A tent over 3 by 3 texels, four reads between them: each pixel's
        // march starts its own way in, so the clouds carry a fine grain that
        // read texel for texel would show at twice its size.
        vec2 texel = 0.5 / vec2(textureSize(uSkyCloud, 0));
        cloud = 0.25 * (texture(uSkyCloud, vUv - texel) + texture(uSkyCloud, vUv + vec2(texel.x, -texel.y)) +
                        texture(uSkyCloud, vUv + vec2(-texel.x, texel.y)) + texture(uSkyCloud, vUv + texel));
    } else {
        skyLayers(direction, air, cloud);
    }
    outColor = vec4(min(composeSky(direction, air, cloud, true) * uSkyColor, vec3(kMaxHalf)), 1.0);
}
