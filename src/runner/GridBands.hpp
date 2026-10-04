#pragma once

#include "Matrix4.hpp"

#include <vector>

namespace runner {

// A camera looking at the floor grid: its view and projection, and the pane's size in pixels.
struct GridView {
    engine_core::Matrix4 view;
    engine_core::Matrix4 projection;
    int width = 0;
    int height = 0;
};

// Triangles, as normalized device x and y, three points each, covering every
// pixel grid.frag can draw: a band a few pixels wide around each rule it can
// show from this camera, and around the X and Z axes, out to where the grid
// fades away. The grid shader runs on these instead of the whole pane, so it
// shades the lines and little else. Which rule spacings show where is read off
// a sample of the pane, as the shader picks them by how much floor each pixel
// covers. out is cleared first.
void build_grid_bands(const GridView& view, std::vector<float>& out);

}  // namespace runner
