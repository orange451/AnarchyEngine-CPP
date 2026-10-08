#pragma once

// LayerBuilder: packs one terrain Material's texture layer (two RGBA8
// arrays, A = color RGB + height A, B = normal XY + roughness B + metalness
// A, each with a full mip chain) from its resolved source file paths. Pure
// function over files on disk: no GL, no Instance state, callable from any
// thread. Task 4 of the terrain textures sub-project; Task 5 caches and
// publishes the result.

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

// The packed bytes for one layer at `size`. a_mips/b_mips hold RGBA8 level
// 0 first (size x size), each next level half the size (rounded up) of the
// last, down to and including 1x1. warning is set to the first source file
// that could not be read (that map falls back to its default); empty when
// every present path decoded.
struct LayerBytes {
    int size = 0;
    std::vector<std::vector<std::uint8_t>> a_mips, b_mips;
    std::string warning;
};

// Builds one Material's packed layer at `size` (one side of the terrain's
// Enum.TextureSize). Safe to call from any thread; touches only the given
// files and heap memory.
LayerBytes build_layer(const LayerSources& sources, int size);

// Exact byte count of a_mips plus b_mips together for a layer built at
// `size` (both RGBA8 arrays, full mip chains).
std::size_t layer_bytes(int size);

}  // namespace engine_core::terrain
