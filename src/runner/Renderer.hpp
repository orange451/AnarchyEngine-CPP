#pragma once

#include "Matrix4.hpp"
#include "ViewCapture.hpp"

namespace anarchy::amesh {
class GpuMesh;
}

namespace runner {

// One uploaded AMESH at a GameObject's Transform (column-major, world space),
// colored by its Material: the diffuse texture times color times the mesh's
// vertex colors, then lit.
struct MeshDraw {
    const anarchy::amesh::GpuMesh* mesh = nullptr;
    engine_core::Matrix4 model = engine_core::matrix4_identity();
    // A GL texture, sampled at the mesh's UVs. 0 draws white.
    unsigned texture = 0;
    // RGBA, 0 to 1. Alpha is not drawn yet.
    float color[4] = {1.f, 1.f, 1.f, 1.f};
};

// Draws meshes seen from the camera, lit by one light from above.
class Renderer {
public:
    // The camera until setCamera: where it is and what it looks at, in world units, Y up.
    static constexpr float kCameraEye[3] = {0.f, 3.f, 7.f};
    static constexpr float kCameraTarget[3] = {0.f, 0.f, 0.f};
    static constexpr float kCameraFovYDegrees = 60.f;

    // The GL context has to be current, and LoadGl has to have run. False when
    // mesh.vert or mesh.frag does not build.
    bool initialize();

    // Where meshes are seen from: a Camera's Transform, looking down its -Z
    // with its +Y up, and the vertical angle it sees in degrees. Any scale in
    // world is taken out. A world with no inverse, or an angle not between 0
    // and 180, is ignored and the camera stays as it was. Needs no GL context.
    void setCamera(const engine_core::Matrix4& world, float fovYDegrees);

    // x, y, width, and height are the pane in window points, origin at the top
    // left. sceneWidth and sceneHeight are the window in the same units.
    // The current GL viewport is the framebuffer. Drawing restores the viewport,
    // scissor, blend, and depth test so a UI pass can continue.
    // meshes may be null when meshCount is 0. The pane is still cleared.
    void draw(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
              const MeshDraw* meshes, int meshCount);
    // Reads back what draw just drew for the same pane, top row first. Only
    // the part inside the framebuffer and the current scissor. False when
    // there is nothing to read.
    bool read(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
              ViewPixels& out) const;
    void shutdown();
    // What draw clears the pane to, 0 to 1 per channel. The Scene View passes
    // its theme color, so the clear matches the pane around it.
    void setClearColor(float r, float g, float b);

private:
    void drawMeshes(const MeshDraw* meshes, int count, float aspect);

    unsigned meshProgram_ = 0;
    int modelLocation_ = -1;
    int viewProjectionLocation_ = -1;
    int diffuseLocation_ = -1;
    int colorLocation_ = -1;
    // 1 by 1 white, bound for a draw with no texture.
    unsigned whiteTexture_ = 0;
    bool ready_ = false;
    float clear_[3] = {30.f / 255.f, 30.f / 255.f, 30.f / 255.f};
    // The inverse of the camera's world, column-major.
    engine_core::Matrix4 view_ = DefaultView();
    float fovYDegrees_ = kCameraFovYDegrees;

    static engine_core::Matrix4 DefaultView();
};

}  // namespace runner
