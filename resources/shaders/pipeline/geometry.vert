#version 330 core
// An AMESH vertex (amesh.hpp's GpuMesh locations) at its instance's world
// matrix, passed on in view space. The G-buffer pass and the transparency
// pass both draw with it (the legacy deferred.vert and forward.vert). Each
// instance's matrices and tint come from runner::InstanceBuffer, slots 7 to 15.
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;
layout (location = 4) in vec4 aColor;
// Up to four bones and their weights; an unused one has weight 0.
layout (location = 5) in uvec4 aBones;
layout (location = 6) in vec4 aWeights;
layout (location = 7) in mat4 aModel;
// The inverse transpose of aModel's 3 by 3, made once per instance on the CPU,
// so normals stay square to the surface under a non-uniform scale.
layout (location = 11) in mat3 aNormalMatrix;
// The GameObject's Color, already linear, so it multiplies like a vertex color.
layout (location = 14) in vec3 aTint;
// The first texel of the instance's skinning matrices in uBones, or -1 when
// it is not skinned.
layout (location = 15) in float aBoneBase;

uniform mat4 uView;
uniform mat4 uProjection;
// 1 while drawing billboards (runner::MeshDraw::billboard): each instance's
// card is turned about its up axis to face the camera.
uniform int uBillboard;
// The card field's band (runner::CardField), squared, from the camera to a
// card's origin: a card outside [x, y) draws nothing. Off while y is 0, as
// for every draw the CPU already chose by band.
uniform vec2 uCardBand;
// An LOD's cross-fades, in units from the camera to the instance's origin: it
// fades in from x to y and out from z to w. An end whose second value is 0
// does not fade.
uniform vec4 uLodFade;
// Three RGBA32F texels a bone, the first three rows of its skinning matrix,
// 1024 texels to a row (runner::BoneTexture).
uniform sampler2D uBones;

out vec3 vViewPosition;
out vec3 vViewNormal;
out vec2 vUv;
out vec4 vColor;
// How far the instance has faded in (x) and not yet faded out (y), 0 to 1:
// the fragment shader dithers the rest of its pixels away.
flat out vec2 vLodFade;

vec4 boneRow(int texel) {
    return texelFetch(uBones, ivec2(texel % 1024, texel / 1024), 0);
}

mat4 boneMatrix(uint bone) {
    int texel = int(aBoneBase) + 3 * int(bone);
    return transpose(mat4(boneRow(texel), boneRow(texel + 1), boneRow(texel + 2), vec4(0.0, 0.0, 0.0, 1.0)));
}

// The vertex's blend of its bones' matrices; the identity when it is not
// skinned, or has no weight (a static part of a skinned mesh).
mat4 skin() {
    if (aBoneBase < 0.0) {
        return mat4(1.0);
    }
    mat4 blended = mat4(0.0);
    float total = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (aWeights[k] > 0.0) {
            blended += aWeights[k] * boneMatrix(aBones[k]);
            total += aWeights[k];
        }
    }
    return total > 0.0 ? blended / total : mat4(1.0);
}

// vLodFade for an instance with its origin at origin, the camera at eye.
void lodFade(vec3 origin, vec3 eye) {
    float distance = length(origin - eye);
    vLodFade = vec2(uLodFade.y > 0.0 ? smoothstep(uLodFade.x, uLodFade.y, distance) : 1.0,
                    uLodFade.w > 0.0 ? 1.0 - smoothstep(uLodFade.z, uLodFade.w, distance) : 1.0);
}

// The camera's position: the view only turns and moves.
vec3 eyePosition() {
    return -(transpose(mat3(uView)) * uView[3].xyz);
}

// A billboard's corner: the card stands on aModel's origin along its up axis
// (the second column, as long as the card is tall), turned about it until it
// faces the camera, as wide as the first column. Its normal faces the camera.
void billboard() {
    vec3 origin = aModel[3].xyz;
    vec3 upAxis = aModel[1].xyz;
    float height = length(upAxis);
    vec3 up = height > 0.0 ? upAxis / height : vec3(0.0, 1.0, 0.0);
    vec3 eye = eyePosition();
    vec3 toward = eye - origin;
    lodFade(origin, eye);
    if (uCardBand.y > 0.0) {
        float squared = dot(toward, toward);
        if (squared < uCardBand.x || !(squared < uCardBand.y)) {
            // Outside the clip volume at every corner: clipped away whole.
            vViewNormal = vec3(0.0, 0.0, 1.0);
            vViewPosition = vec3(0.0);
            vUv = vec2(0.0);
            vColor = vec4(0.0);
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            return;
        }
    }
    toward -= up * dot(toward, up);
    float across = length(toward);
    // Straight above or below, it keeps its own facing.
    vec3 forward = across > 1e-6 ? toward / across : normalize(aModel[2].xyz);
    vec3 right = cross(up, forward);
    vec3 world = origin + right * (aPosition.x * length(aModel[0].xyz)) + upAxis * aPosition.y;
    vec4 viewPosition = uView * vec4(world, 1.0);
    vViewNormal = mat3(uView) * forward;
    vViewPosition = viewPosition.xyz;
    vUv = aUv;
    vColor = aColor * vec4(aTint, 1.0);
    gl_Position = uProjection * viewPosition;
}

void main() {
    if (uBillboard != 0) {
        billboard();
        return;
    }
    lodFade(aModel[3].xyz, eyePosition());
    mat4 skinning = skin();
    vec4 viewPosition = uView * (aModel * (skinning * vec4(aPosition, 1.0)));
    // The view has no scale, so its rotation carries normals on.
    vViewNormal = mat3(uView) * (aNormalMatrix * (mat3(skinning) * aNormal));
    vViewPosition = viewPosition.xyz;
    vUv = aUv;
    vColor = aColor * vec4(aTint, 1.0);
    gl_Position = uProjection * viewPosition;
}
