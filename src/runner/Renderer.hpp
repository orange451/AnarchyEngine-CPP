#pragma once

namespace runner {

// Draws one triangle whose corners are red, green, and blue.
// The rasterizer interpolates those colors across the face, which is the rainbow.
// angleDegrees spins that triangle about the vertical axis through its center.
class Renderer {
public:
    // The GL context has to be current, and LoadGl has to have run.
    bool initialize();

    // x, y, width, and height are the pane in window points, origin at the top
    // left. sceneWidth and sceneHeight are the window in the same units.
    // The current GL viewport is the framebuffer. Drawing restores the viewport,
    // scissor, and blend so a UI pass can continue.
    void draw(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
              float angleDegrees);
    void shutdown();

private:
    unsigned program_ = 0;
    unsigned vao_ = 0;
    unsigned vbo_ = 0;
    int angleLocation_ = -1;
    bool ready_ = false;
};

}  // namespace runner
