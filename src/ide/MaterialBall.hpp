#pragma once

#include "MaterialPreviews.hpp"
#include "amesh.hpp"
#include "runner/MeshCache.hpp"
#include "runner/Renderer.hpp"
#include "runner/TextureCache.hpp"
#include "runner/ViewCapture.hpp"

#include <filesystem>
#include <string>

struct GLFWwindow;

namespace ide {

// What drawView draws the look on: a ball, a cube, a plane facing the
// camera, or a Mesh of the place (setMeshPath).
enum class PreviewMesh { Sphere, Cube, Plane, Mesh };

// A Material's look on a ball, as the Scene View's Renderer draws it, off
// screen: a fixed camera and studio lights, a warm key light from the upper
// left and a cool fill from the right, so a preview looks the same in every
// place. Every call but the destructor and release needs a GL context
// current, the same one each time until release.
class MaterialBall {
public:
    // Draws render_size pixels square, then shrinks to size, which smooths
    // the ball's highlights and edge.
    MaterialBall(int render_size, int size);
    // Needs no GL: a ball still holding GL objects is one whose context is gone.
    ~MaterialBall();

    MaterialBall(const MaterialBall&) = delete;
    MaterialBall& operator=(const MaterialBall&) = delete;

    // The resources folder Texture Paths are under. Empty draws no textures.
    void setRoot(const std::filesystem::path& root);
    // Puts look on the ball in out, size by size, top row first, clear around
    // the ball; out is empty when GL cannot draw it. False, with out empty,
    // when GL is not ready to draw it yet, as in the first frame after the
    // renderer is made: draw again in a later frame. Leaves the framebuffer
    // binding, viewport, and scissor as they were.
    bool draw(const MaterialLook& look, runner::ViewPixels& out);
    // The Shader Editor's preview: look on the chosen mesh, turned by yaw
    // (about up) then pitch (about across), radians, as the whole view, size
    // by size. False, with out empty, as draw's.
    bool drawView(const MaterialLook& look, runner::ViewPixels& out);
    void setMesh(PreviewMesh mesh) { mesh_ = mesh; }
    PreviewMesh mesh() const { return mesh_; }
    // A Mesh's file, relative to the resources folder, drawn scaled to the ball's size.
    void setMeshPath(std::string path) {
        mesh_ = PreviewMesh::Mesh;
        mesh_path_ = std::move(path);
    }
    void setRotation(float yaw, float pitch) {
        yaw_ = yaw;
        pitch_ = pitch;
    }
    // Deletes the GL objects in the context that made them, whichever is
    // current, which must still be open. The next draw makes them again, in
    // whatever context is current then.
    void release();

private:
    bool ensureGraphics();
    // look on mesh at model into render, the whole picture.
    bool render(const MaterialLook& look, const anarchy::amesh::GpuMesh& mesh, const engine_core::Matrix4& model,
                runner::ViewPixels& render);

    const int render_size_;
    const int size_;
    runner::Renderer renderer_;
    runner::TextureCache textures_;
    anarchy::amesh::GpuMesh sphere_;
    anarchy::amesh::GpuMesh cube_;
    anarchy::amesh::GpuMesh plane_;
    runner::MeshCache meshes_;
    PreviewMesh mesh_ = PreviewMesh::Sphere;
    std::string mesh_path_;
    float yaw_ = 0.f;
    float pitch_ = 0.f;
    // What the ball is drawn into: a color texture, render_size_ square.
    unsigned framebuffer_ = 0;
    unsigned color_ = 0;
    bool attempted_ = false;
    bool ready_ = false;
    // The context the GL objects were made in.
    GLFWwindow* context_ = nullptr;
};

}  // namespace ide
