#version 330 core
// The outlines over the tone mapped pane, straight alpha: a selected
// PhysicsObject's collision shape. Where a nearer surface hides a line it is
// drawn faint rather than not at all, so a shape inside a mesh still shows.
in float vDistance;
in vec4 vClip;
out vec4 fragColor;

// The scene's depth, 1 where no opaque surface was drawn.
uniform sampler2D uDepth;
uniform mat4 uInverseProjection;

const vec3 kColor = vec3(0.35, 1.0, 0.45);
const float kHiddenAlpha = 0.3;
// A line on a surface, as a collision box on its own mesh is, stays in
// front of it: this far, or this part of its distance, whichever is more.
const float kSlack = 0.01;
const float kRelativeSlack = 0.005;

void main() {
    vec2 ndc = vClip.xy / vClip.w;
    float alpha = 1.0;
    float depth = texture(uDepth, ndc * 0.5 + 0.5).r;
    if (depth < 1.0) {
        vec4 scene = uInverseProjection * vec4(ndc, depth * 2.0 - 1.0, 1.0);
        float sceneDistance = -scene.z / scene.w;
        if (sceneDistance < vDistance - max(kSlack, vDistance * kRelativeSlack)) {
            alpha = kHiddenAlpha;
        }
    }
    fragColor = vec4(kColor, alpha);
}
