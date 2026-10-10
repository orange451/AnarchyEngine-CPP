#pragma once

// LayerBuilder: packs one terrain Material's texture layer from its
// resolved source file paths: build_layer makes two RGBA8 mip chains (A =
// color RGB + height A, B = normal XY + roughness B + metalness A), and
// compress_layer turns them into the three block-compressed planes the
// renderer uploads. preview_layer and placeholder_layer stand in until a
// layer is built. Pure functions over files on disk: no GL, no Instance
// state, callable from any thread.

#include "texture/BlockCompress.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace engine_core::terrain {

// Resolved file paths for one Material's maps; an empty path means that map
// is absent and its channels fall back to the layer-0 defaults (white
// color, 0.5 height, flat normal, roughness 1, metalness 1).
struct LayerSources {
    std::filesystem::path diffuse, normal, roughness, metalness, height;
};

// One layer's packed RGBA8 pixels at `size`, as build_layer makes them.
// a_mips/b_mips hold level 0 first (size x size), each next level half the
// size (rounded up) of the last, down to and including 1x1. warning is set
// to the first source file that could not be read (that map falls back to
// its default); empty when every present path decoded.
struct LayerPixels {
    int size = 0;
    std::vector<std::vector<std::uint8_t>> a_mips, b_mips;
    std::string warning;
};

// Builds one Material's packed layer at `size` (one side of the terrain's
// Enum.TextureSize). Safe to call from any thread; touches only the given
// files and heap memory.
LayerPixels build_layer(const LayerSources& sources, int size);

// One layer as the renderer uploads it: three planes, each indexed by level
// of the full chain at `size` (as LayerPixels'). Plane 0 (A) is color RGB and
// height A in BC3; plane 1 (B) normal X and Y in BC5; plane 2 (C) roughness
// and metalness in BC5 (R and G); each RGBA8 instead without s3tc. Levels
// before first_level are empty: a preview, or a cache file still being
// read. first_level 0 is the whole layer.
struct LayerBytes {
    int size = 0;
    int first_level = 0;
    std::array<std::vector<std::vector<std::uint8_t>>, 3> planes;
    std::string warning;
};

// Each plane's format: BC3, BC5, BC5, or RGBA8 for all without s3tc.
std::array<texture::PixelFormat, 3> layer_formats();

// Compresses pixels' every level: A as is, B's R and G into B, B's B and A
// into C.
LayerBytes compress_layer(const LayerPixels& pixels);

// A quick look at a layer before its full build: the diffuse alone (no other
// map is read), built at 64 (or size, when smaller) and placed at the levels
// a size chain has from 64 down, so first_level = log2(size / 64).
LayerBytes preview_layer(const LayerSources& sources, int size);

// A layer with nothing to show yet: its levels from 64 down (first_level
// past the rest) mid grey (128), height 0.5, a flat normal, and roughness
// and metalness 1 (what a missing map gives).
LayerBytes placeholder_layer(int size);

// What build_layer gives a layer with no maps at all, without decoding or
// compressing anything: white, height 0.5, a flat normal, roughness and
// metalness 1. Layer 0, the untextured default.
LayerBytes untextured_layer(int size);

// Bytes of a whole LayerBytes at `size`: all three planes, every level.
std::size_t layer_bytes(int size);

}  // namespace engine_core::terrain
