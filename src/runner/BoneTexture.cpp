#include "BoneTexture.hpp"

#include "gl.hpp"

#include <algorithm>

namespace runner {

void BoneTexture::upload(const BonePalette& palette) {
    const int rows = std::max(1, palette.rows(kBoneTextureWidth));
    if (texture_ == 0) {
        glGenTextures(1, &texture_);
    }
    // Bound on its own unit, where only the vertex shaders read it.
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(kBoneTextureUnit));
    glBindTexture(GL_TEXTURE_2D, texture_);
    if (rows > rows_) {
        // Doubled, so a crowd that grows a little each frame reallocates rarely.
        const int grown = std::max(rows, rows_ * 2);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA32F), kBoneTextureWidth, grown, 0, GL_RGBA, GL_FLOAT,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(RT_GL_NEAREST));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(RT_GL_NEAREST));
        rows_ = grown;
    }
    const std::vector<float>& texels = palette.texels();
    const int count = static_cast<int>(texels.size() / 4);
    const int whole = count / kBoneTextureWidth;
    if (whole > 0) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kBoneTextureWidth, whole, GL_RGBA, GL_FLOAT, texels.data());
    }
    const int rest = count - whole * kBoneTextureWidth;
    if (rest > 0) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, whole, rest, 1, GL_RGBA, GL_FLOAT,
                        texels.data() + static_cast<std::size_t>(whole) * kBoneTextureWidth * 4);
    }
    glActiveTexture(GL_TEXTURE0);
}

void BoneTexture::destroy() {
    // DeleteTexture tells the renderer's state restore the name is gone.
    DeleteTexture(texture_);
    texture_ = 0;
    rows_ = 0;
}

}  // namespace runner
