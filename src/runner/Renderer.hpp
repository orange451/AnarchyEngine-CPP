#pragma once

namespace runner {

// One triangle in the pane. x, y, and z are its position in view space.
// angleDegrees spins it about the vertical axis through that position.
struct TriangleDraw {
    float angleDegrees = 0.f;
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

// Draws triangles whose corners are red, green, and blue.
// The rasterizer interpolates those colors across each face.
class Renderer {
public:
    // The GL context has to be current, and LoadGl has to have run.
    bool initialize();

    // x, y, width, and height are the pane in window points, origin at the top
    // left. sceneWidth and sceneHeight are the window in the same units.
    // The current GL viewport is the framebuffer. Drawing restores the viewport,
    // scissor, blend, and depth test so a UI pass can continue.
    // triangles may be null when count is 0. The pane is still cleared.
    void draw(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
              const TriangleDraw* triangles, int count);
    void shutdown();

private:
    unsigned program_ = 0;
    unsigned vao_ = 0;
    unsigned vbo_ = 0;
    int angleLocation_ = -1;
    int positionLocation_ = -1;
    bool ready_ = false;
};

}  // namespace runner
