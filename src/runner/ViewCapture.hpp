#pragma once

#include <string>
#include <vector>

namespace runner {

// Pixels read back from a view: RGBA, 8 bits a channel, rows from the top.
struct ViewPixels {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;

    bool empty() const { return width <= 0 || height <= 0; }
};

// pixels scaled down, keeping its shape, so neither side is over max_size.
// Each new pixel averages the ones it covers. Smaller pixels come back as they are.
ViewPixels FitWithin(const ViewPixels& pixels, int max_size);

// pixels as a PNG file's bytes, without alpha. Empty when there are no pixels.
std::string EncodePng(const ViewPixels& pixels);

}  // namespace runner
