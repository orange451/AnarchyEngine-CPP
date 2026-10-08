#pragma once

// Derives a 0..1 blend height field for a terrain layer's Material when it
// has no HeightTexture: integrated from its NormalTexture, or (failing
// that) the brightness of its DiffuseTexture. Pure functions over raw RGBA8
// texels: no GL, no Instance state, callable from any thread, consumed by
// the layer builder (Task 4). Task 3 of the terrain textures sub-project.

#include <cstdint>
#include <vector>

namespace engine_core::terrain {

// rgba is w*h RGBA8 texels of a tangent-space normal map (R=X, G=Y, B=Z;
// alpha ignored), treated as tileable: every row/column wraps into the
// opposite edge, matching the terrain array textures this feeds. Decodes
// X,Y,Z to [-1,1] (x = r/255*2-1, same for y,z), clamps Z >= 0.2 so a
// grazing normal cannot blow up the slope, and integrates the resulting
// slope field (-X/Z, -Y/Z) into a height field with a periodic Gauss-Seidel
// Poisson solve run on a coarse-to-fine pyramid (so it stays fast at large
// sizes). The low-frequency part of that integrated height is then removed
// with a box-blur high-pass and the remainder is normalized to 0..1.
//
// Returns all 0.5 for a 1x1 input or any input whose result would otherwise
// be a flat field (including a constant/flat normal map); never returns a
// NaN.
std::vector<float> height_from_normals(const std::uint8_t* rgba, int w, int h);

// rgba is w*h RGBA8 texels of an sRGB-encoded color texture (alpha
// ignored), treated as tileable the same way. Decodes sRGB to linear per
// channel and takes the Rec.709 luminance (0.2126 R + 0.7152 G + 0.0722 B),
// then applies the same box-blur high-pass and 0..1 normalization as
// height_from_normals.
//
// Returns all 0.5 for a 1x1 input or a flat (constant-color) input; never
// returns a NaN.
std::vector<float> height_from_luminance(const std::uint8_t* rgba, int w, int h);

}  // namespace engine_core::terrain
