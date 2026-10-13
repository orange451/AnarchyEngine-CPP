#pragma once

#include "SnapshotPump.hpp"
#include "runner/MeshCache.hpp"
#include "runner/Renderer.hpp"
#include "runner/TextureCache.hpp"
#include "runner/ViewCapture.hpp"
#include "types.hpp"

#include <filesystem>
#include <string>

namespace ide {

// The card a Billboard LodLayer stands up for a Prefab whose box is low to
// high, in units: centered on the Prefab's origin and as wide as twice its
// widest reach from it in X or Z, so it covers the Prefab from any side; and
// from its origin up to its top.
struct CardFrame {
    float width = 0.f;
    float height = 0.f;
};
CardFrame card_frame(const engine_core::Vec3& low, const engine_core::Vec3& high);

// render's pixels in twos: each the average of the 2 by 2 it covers,
// weighted by their alpha, its alpha their mean. Then each clear pixel takes
// the color of the nearest drawn one, so filtering and mipmaps at the card's
// edge never mix in black.
runner::ViewPixels shrink_card(const runner::ViewPixels& render);

// Where a card of prefab_name whose PNG bytes are png is saved under the
// resources folder: textures/billboards/<name>.<hash>.png. The hash is of the
// bytes, so a card that changed is a new file, which every cache reads anew.
std::string card_file(const std::string& prefab_name, const std::string& png);

// A Prefab's own Models drawn onto its billboard card, off screen: seen level
// from in front (+Z) through a narrow lens from far off, so its near and far
// parts are nearly one size, framed to card_frame, in the colors its
// Materials and textures give it before any light (the scene lights the card
// as it lights everything), on clear. Every call but the destructor needs a
// GL context current, the same one each time until release.
class BillboardBaker {
public:
    // Cards height pixels tall, as wide as their frame, up to twice that.
    explicit BillboardBaker(int height = 512);
    // Needs no GL: a baker still holding GL objects is one whose context is gone.
    ~BillboardBaker();
    BillboardBaker(const BillboardBaker&) = delete;
    BillboardBaker& operator=(const BillboardBaker&) = delete;

    // The resources folder Mesh and Texture Paths are under.
    void setRoot(const std::filesystem::path& root);
    // prefab's own Models (its LodLayers left out), its box low to high, on
    // its card in out: top row first, straight alpha. True when done, with out
    // empty when there is nothing to draw. False while a texture still loads,
    // or GL cannot draw yet: bake again in a later frame.
    bool bake(const engine_core::VisualPrefab& prefab, const engine_core::Vec3& low, const engine_core::Vec3& high,
              runner::ViewPixels& out);
    // Deletes the GL objects. The next bake makes them again, in whatever context is current then.
    void release();

private:
    bool ensureGraphics();
    // The framebuffer bakes draw into, width by height.
    bool ensureTarget(int width, int height);

    const int height_;
    runner::Renderer renderer_;
    runner::MeshCache meshes_;
    runner::TextureCache textures_;
    unsigned framebuffer_ = 0;
    unsigned color_ = 0;
    int target_width_ = 0;
    int target_height_ = 0;
    bool attempted_ = false;
    bool ready_ = false;
};

}  // namespace ide
