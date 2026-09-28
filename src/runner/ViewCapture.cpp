#include "ViewCapture.hpp"

#include <algorithm>
#include <cmath>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace runner {

ViewPixels FitWithin(const ViewPixels& pixels, int max_size) {
    const int longer = std::max(pixels.width, pixels.height);
    if (pixels.empty() || max_size <= 0 || longer <= max_size) {
        return pixels;
    }
    const double scale = static_cast<double>(max_size) / longer;
    ViewPixels out;
    out.width = std::max(1, static_cast<int>(std::lround(pixels.width * scale)));
    out.height = std::max(1, static_cast<int>(std::lround(pixels.height * scale)));
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    // Each new pixel covers a box of old ones, [x0, x1) by [y0, y1).
    for (int y = 0; y < out.height; ++y) {
        const int y0 = y * pixels.height / out.height;
        const int y1 = std::max(y0 + 1, (y + 1) * pixels.height / out.height);
        for (int x = 0; x < out.width; ++x) {
            const int x0 = x * pixels.width / out.width;
            const int x1 = std::max(x0 + 1, (x + 1) * pixels.width / out.width);
            unsigned sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy) {
                const unsigned char* row = pixels.rgba.data() + (static_cast<std::size_t>(sy) * pixels.width + x0) * 4;
                for (int sx = x0; sx < x1; ++sx, row += 4) {
                    for (int channel = 0; channel < 4; ++channel) {
                        sum[channel] += row[channel];
                    }
                }
            }
            const unsigned count = static_cast<unsigned>((x1 - x0) * (y1 - y0));
            unsigned char* to = out.rgba.data() + (static_cast<std::size_t>(y) * out.width + x) * 4;
            for (int channel = 0; channel < 4; ++channel) {
                to[channel] = static_cast<unsigned char>((sum[channel] + count / 2) / count);
            }
        }
    }
    return out;
}

std::string EncodePng(const ViewPixels& pixels) {
    if (pixels.empty() || pixels.rgba.size() < static_cast<std::size_t>(pixels.width) * pixels.height * 4) {
        return {};
    }
    // The framebuffer's alpha is not part of the picture.
    std::vector<unsigned char> rgb(static_cast<std::size_t>(pixels.width) * pixels.height * 3);
    for (std::size_t from = 0, to = 0; to < rgb.size(); from += 4, to += 3) {
        rgb[to] = pixels.rgba[from];
        rgb[to + 1] = pixels.rgba[from + 1];
        rgb[to + 2] = pixels.rgba[from + 2];
    }
    std::string png;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
        },
        &png, pixels.width, pixels.height, 3, rgb.data(), pixels.width * 3);
    return png;
}

}  // namespace runner
