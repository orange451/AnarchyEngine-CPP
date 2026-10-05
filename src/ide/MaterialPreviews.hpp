#pragma once

#include "Color.hpp"
#include "runner/ViewCapture.hpp"
#include "types.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_core {
class DataModel;
}

namespace jadefx {
class Image;
}

namespace ide {

// What a Material's preview is drawn from: its properties as the Scene View
// draws them, numbers held between 0 and 1, and the Path of each Texture it
// uses, empty for none. The defaults are a new Material's.
struct MaterialLook {
    engine_core::ColorRgb color{1.f, 1.f, 1.f, 1.f};
    engine_core::ColorRgb emissive{0.f, 0.f, 0.f, 1.f};
    float metalness = 0.f;
    float roughness = 0.4f;
    float reflectivity = 0.5f;
    float transparency = 0.f;
    std::string diffuse_texture;
    std::string normal_texture;
    std::string roughness_texture;
    std::string metalness_texture;
    std::string emissive_texture;

    bool operator==(const MaterialLook& other) const;
    bool operator!=(const MaterialLook& other) const { return !(*this == other); }
};

// id's look, or nothing when id is not a live Material. Callers hold the world's read lock.
std::optional<MaterialLook> material_look(const engine_core::DataModel& world, engine_core::InstanceId id);

// A ball's render, top row first, with the ball centered and radius times
// half the render's width across, as size by size pixels: clear outside the
// circle, its edge smoothed, and each pixel's color averaged from the ball alone.
runner::ViewPixels cut_ball(const runner::ViewPixels& render, double radius, int size);

// Each Material's preview image, drawn by draw a few a pass, since a draw
// needs the GL context and takes a frame's time. get queues a Material whose
// look has no preview yet, and answers the last preview meanwhile, so an edit
// does not flash the class's icon. A draw that fails keeps no image and is
// not tried again until the look changes. A draw not ready yet, as while GL
// prepares the renderer, ends the pass and is tried again in the next.
class MaterialPreviews {
public:
    // Passes a Material's draw may be not ready for before it counts as failed.
    static constexpr int kMaxWaits = 60;

    // Puts look's preview in image, null when it cannot be drawn, and returns
    // true. False when it drew nothing yet and should be asked again in a later pass.
    using Draw = std::function<bool(const MaterialLook& look, std::shared_ptr<jadefx::Image>& image)>;

    MaterialPreviews(Draw draw, int per_pass);

    // material's preview as last drawn, or null before one is. Queues a draw
    // when it was last drawn for another look, or never.
    std::shared_ptr<jadefx::Image> get(engine_core::InstanceId material, const MaterialLook& look);
    // Draws up to per_pass queued previews. True when it drew any.
    bool draw_pending();
    // Forgets every Material not in in_use, and drops its queued draw.
    void retain(const std::vector<engine_core::InstanceId>& in_use);
    // Whether no draw waits.
    bool idle() const { return queue_.empty(); }

private:
    struct Entry {
        std::shared_ptr<jadefx::Image> image;
        // The look image was drawn for; unset before the first draw.
        std::optional<MaterialLook> drawn;
        // The look the queued draw is for.
        MaterialLook wanted;
        bool queued = false;
        // Passes the queued draw was not ready for.
        int waits = 0;
    };

    Draw draw_;
    int per_pass_;
    std::unordered_map<engine_core::InstanceId, Entry> entries_;
    std::deque<engine_core::InstanceId> queue_;
};

}  // namespace ide
