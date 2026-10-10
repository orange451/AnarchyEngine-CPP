#pragma once

#include "BonePalette.hpp"

namespace runner {

// The texture unit the vertex shaders read uBones from. The Material and
// G-buffer units are 0 to 15, all fragment-side.
inline constexpr int kBoneTextureUnit = 16;
// Texels across a row of the bone texture: geometry.vert and shadow.vert
// address texel t at (t % 1024, t / 1024).
inline constexpr int kBoneTextureWidth = 1024;

// One frame's BonePalette as an RGBA32F texture, GL_NEAREST, kBoneTextureWidth
// texels wide and as many rows as it needs, grown and never shrunk. Every call
// needs the GL context it was made in; destroy before that context goes.
class BoneTexture {
public:
    // Replaces what the texture holds with palette's texels. With none, it
    // still has a row, so a sampler bound to it is always complete.
    void upload(const BonePalette& palette);
    unsigned texture() const { return texture_; }
    void destroy();

private:
    unsigned texture_ = 0;
    int rows_ = 0;
};

}  // namespace runner
