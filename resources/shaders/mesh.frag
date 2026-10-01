#version 330 core
in vec3 vNormal;
in vec2 vUv;
in vec4 vColor;

// The Material's DiffuseTexture, or 1 by 1 white when it has none.
uniform sampler2D uDiffuse;
// The Material's Color, white when there is no Material.
uniform vec4 uColor;

out vec4 fragColor;

// One light from above and in front, and some ambient so faces turned away still read.
const vec3 kLight = normalize(vec3(0.4, 1.0, 0.6));
const float kAmbient = 0.25;

void main() {
    float lit = kAmbient;
    float len = length(vNormal);
    if (len > 0.0) {
        lit += (1.0 - kAmbient) * max(dot(vNormal / len, kLight), 0.0);
    }
    vec3 albedo = texture(uDiffuse, vUv).rgb * uColor.rgb * vColor.rgb;
    fragColor = vec4(albedo * lit, 1.0);
}
