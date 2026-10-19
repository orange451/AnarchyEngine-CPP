#version 330 core
// Shadow maps have no color: depth is all they keep. Built with CUTOUT for a
// caster whose texture keeps alpha (runner::MeshDraw::cutout): its holes cast
// nothing, by the G-buffer's own alpha test (deferred.frag). Without it, no
// texture is read and the GPU's early depth test stays on.
#ifdef CUTOUT
in vec2 vUv;
uniform sampler2D uDiffuse;
#endif

void main() {
#ifdef CUTOUT
    if (texture(uDiffuse, vUv).a < 0.25) {
        discard;
    }
#endif
}
