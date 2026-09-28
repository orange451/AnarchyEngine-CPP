#include "runner/ViewCapture.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void Expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++gFailures;
    }
}

runner::ViewPixels Solid(int width, int height, unsigned char r, unsigned char g, unsigned char b) {
    runner::ViewPixels pixels;
    pixels.width = width;
    pixels.height = height;
    for (int i = 0; i < width * height; ++i) {
        pixels.rgba.insert(pixels.rgba.end(), {r, g, b, 255});
    }
    return pixels;
}

// Big-endian, as PNG writes its header fields.
unsigned ReadU32(const std::string& bytes, std::size_t at) {
    return static_cast<unsigned>(static_cast<unsigned char>(bytes[at])) << 24u |
           static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 1])) << 16u |
           static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 2])) << 8u |
           static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 3]));
}

}  // namespace

int RunViewCaptureTests() {
    // Left half red, right half blue.
    runner::ViewPixels halves = Solid(4, 2, 255, 0, 0);
    for (int y = 0; y < 2; ++y) {
        for (int x = 2; x < 4; ++x) {
            unsigned char* at = halves.rgba.data() + (y * 4 + x) * 4;
            at[0] = 0;
            at[2] = 255;
        }
    }
    const runner::ViewPixels half = runner::FitWithin(halves, 2);
    Expect(half.width == 2 && half.height == 1, "FitWithin keeps the shape and fits the longer side");
    Expect(half.rgba.size() == 8 && half.rgba[0] == 255 && half.rgba[2] == 0 && half.rgba[4] == 0 && half.rgba[6] == 255,
           "each new pixel covers its own part of the old ones");

    runner::ViewPixels pair = Solid(2, 1, 0, 0, 0);
    pair.rgba[4] = pair.rgba[5] = pair.rgba[6] = 255;
    const runner::ViewPixels gray = runner::FitWithin(pair, 1);
    Expect(gray.width == 1 && gray.height == 1 && gray.rgba[0] == 128, "a new pixel averages what it covers");

    const runner::ViewPixels small = Solid(3, 2, 10, 20, 30);
    const runner::ViewPixels kept = runner::FitWithin(small, 1024);
    Expect(kept.width == 3 && kept.height == 2 && kept.rgba == small.rgba, "pixels that already fit are unchanged");

    const std::string png = runner::EncodePng(Solid(5, 3, 1, 2, 3));
    Expect(png.size() > 24 && png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0, "EncodePng writes a PNG signature");
    Expect(png.size() > 24 && png.compare(12, 4, "IHDR") == 0 && ReadU32(png, 16) == 5 && ReadU32(png, 20) == 3,
           "the PNG header holds the width and height");
    Expect(runner::EncodePng(runner::ViewPixels{}).empty(), "no pixels encode to nothing");
    return gFailures;
}
