#version 330 core
in vec3 vNormal;
in vec4 vColor;

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
    fragColor = vec4(vColor.rgb * lit, 1.0);
}
