#pragma once

#include <vector>

namespace runner {

// One frame's skinning matrices, as the bone texture holds them: each pose's
// bones back to back, three RGBA texels a bone (the first three rows of its
// skinning matrix). No GL; BoneTexture uploads it.
class BonePalette {
public:
    void clear();
    // Packs bones matrices of 12 floats each from palette, and returns the
    // texel they start at: the per-instance bone base. A palette that is the
    // one added last (a GameObject's other Models) packs nothing again.
    int add(const float* palette, int bones);
    // Four floats a texel.
    const std::vector<float>& texels() const { return texels_; }
    // Rows of width texels that hold them all.
    int rows(int width) const;

private:
    std::vector<float> texels_;
    const float* last_ = nullptr;
    int lastBase_ = 0;
};

}  // namespace runner
