#pragma once

#include "PrefabLods.hpp"
#include "SnapshotPump.hpp"
#include "Vector3.hpp"
#include "runner/MeshCache.hpp"
#include "runner/Renderer.hpp"
#include "runner/TextureCache.hpp"
#include "runner/ViewCapture.hpp"

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

// A baked card's maps, all one size, top row first. Each clear pixel carries
// the nearest drawn one's values, so filtering and mipmaps at the card's edge
// never mix in what is not there.
struct CardMaps {
    // Its colors as sRGB; alpha is the cutout, where it was drawn.
    runner::ViewPixels color;
    // A tangent-space normal map: RGB the unit normal times 0.5 plus 0.5, x
    // along the card's right, y up it, z out of it, as surface.glsl reads one.
    runner::ViewPixels normal;
    // Gray.
    runner::ViewPixels roughness;
    runner::ViewPixels metalness;
    // The glow as sRGB.
    runner::ViewPixels emissive;
    // Averaged over what was drawn: a Material holds it as a number alone.
    float reflectivity = 0.5f;
};

// gbuffer, drawn at twice a card's size, shrunk to the card: each pixel the
// average of the 2 by 2 it covers, weighted by their alpha (a normal made unit
// length again), its alpha their mean. Then each clear pixel takes the
// nearest drawn one's values.
CardMaps shrink_card(const runner::GBufferPixels& gbuffer);

// Where map ("color", "normal", ...) of a card of prefab_name whose PNG bytes
// are png is saved under the resources folder:
// textures/billboards/<name>.<map>.<hash>.png. The hash is of the bytes, so a
// map that changed is a new file, which every cache reads anew.
std::string card_file(const std::string& prefab_name, const std::string& map, const std::string& png);

// maps' PNGs saved under root as card_file names them, a map only where the
// card varies: one roughness or one metalness everywhere is the Material's
// number instead, no glow no map, and a flat card no normal map. files says
// what was saved, and the Material's numbers; its size is the caller's to set.
// False, with why set, when a file could not be written.
bool save_card(const CardMaps& maps, const std::filesystem::path& root, const std::string& prefab_name,
               CardFiles& files, std::string& why);

// A Prefab's own Models drawn onto its billboard card, off screen: seen level
// from in front (+Z) through a narrow lens from far off, so its near and far
// parts are nearly one size, framed to card_frame. What it keeps is the
// G-buffer, the surfaces before any light: their colors, normals, roughness,
// metalness, and glow, so the scene lights the card as it lights everything.
// Every call but the destructor needs a GL context current, the same one each
// time until release.
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
    // its card in out. True when done, with out.color empty when there is
    // nothing to draw. False while a texture still loads, or GL cannot draw
    // yet: bake again in a later frame.
    bool bake(const engine_core::VisualPrefab& prefab, const engine_core::Vec3& low, const engine_core::Vec3& high,
              CardMaps& out);
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
