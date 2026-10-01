#pragma once

#include "MaterialPreviews.hpp"
#include "amesh.hpp"
#include "runner/Renderer.hpp"
#include "runner/TextureCache.hpp"
#include "runner/ViewCapture.hpp"

#include <filesystem>

namespace ide {

// A Material's look on a ball, as the Scene View's Renderer draws it, off
// screen: a fixed camera and studio lights, a warm key light from the upper
// left and a cool fill from the right, so a preview looks the same in every
// place. Every call but the destructor needs a GL context current, the same
// one each time until release.
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
    // Deletes the GL objects. The next draw makes them again, in whatever context is current then.
    void release();

private:
    bool ensureGraphics();

    const int render_size_;
    const int size_;
    runner::Renderer renderer_;
    runner::TextureCache textures_;
    anarchy::amesh::GpuMesh sphere_;
    // What the ball is drawn into: a color texture, render_size_ square.
    unsigned framebuffer_ = 0;
    unsigned color_ = 0;
    bool attempted_ = false;
    bool ready_ = false;
};

}  // namespace ide
