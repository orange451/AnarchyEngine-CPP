#include "BonePalette.hpp"

namespace runner {

void BonePalette::clear() {
    texels_.clear();
    last_ = nullptr;
    lastBase_ = 0;
}

int BonePalette::add(const float* palette, int bones) {
    if (palette == last_ && palette != nullptr) {
        return lastBase_;
    }
    const int base = static_cast<int>(texels_.size() / 4);
    texels_.insert(texels_.end(), palette, palette + bones * 12);
    last_ = palette;
    lastBase_ = base;
    return base;
}

int BonePalette::rows(int width) const {
    const int texels = static_cast<int>(texels_.size() / 4);
    return width > 0 ? (texels + width - 1) / width : 0;
}

}  // namespace runner
