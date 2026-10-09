#pragma once

// Terrain tools prototype: the Scene View's terrain mode. T toggles it; a
// small palette picks Add, Grow, Smooth, or Paint, a size, a strength, and a
// material. Grow, Smooth, and Paint work under a ball at the pointer; Add
// drags a box on the grid floor. Each stroke is one step of the studio's
// undo (ChangeHistoryService, a Custom mutation): voxel chunks are shared
// and immutable, so a step holds the chunk maps before and after the
// stroke, not a copy of any voxels.
//
// UI thread. Every voxel read or write runs on the simulation side through
// Engine::on_simulation.

#include "DataModel.hpp"
#include "DraggerMath.hpp"
#include "terrain/VoxelVolume.hpp"

#include <jadefx/scene/layout/VBox.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {
class Engine;
}

namespace jadefx {
class Label;
class Slider;
class ToggleButton;
class Button;
}  // namespace jadefx

namespace runner {

enum class TerrainTool { Add, Grow, Smooth, Paint };

class TerrainBrush;

// The palette: tool buttons, Size and Strength sliders, the material, undo.
class TerrainToolPalette : public jadefx::VBox {
public:
    explicit TerrainToolPalette(TerrainBrush& brush);
    void refresh();

private:
    TerrainBrush& brush_;
    std::vector<jadefx::ToggleButton*> tools_;
    jadefx::Label* title_ = nullptr;
    jadefx::Label* sizeLabel_ = nullptr;
    jadefx::Label* strengthLabel_ = nullptr;
    jadefx::Button* material_ = nullptr;
    jadefx::Slider* size_ = nullptr;
    jadefx::Slider* strength_ = nullptr;
};

class TerrainBrush {
public:
    explicit TerrainBrush(engine_core::Engine& engine);

    std::shared_ptr<TerrainToolPalette> makePalette();

    bool active() const { return active_; }
    // Turns terrain mode on (on the selected Terrain, else the first one in
    // Workspace) or off. False when it could not turn on: no Terrain.
    bool toggle();
    void turnOff();

    // The pointer's world ray; null when it left the view.
    void hover(const std::optional<engine_core::DraggerRay>& ray);
    void press(const engine_core::DraggerRay& ray, bool shift);
    void release();
    // Per frame while the button is held: Grow, Smooth, and Paint apply at a
    // steady rate.
    void tick(double now_seconds, bool shift);

    bool undo();
    bool redo();

    // The cursor (a wire ball, or the box being dragged), as line segments in
    // world space, for Renderer::setOutlines.
    void appendOutline(std::vector<float>& points) const;

    TerrainTool tool() const { return tool_; }
    void setTool(TerrainTool tool) { tool_ = tool; }
    float size() const { return size_; }
    void setSize(float size) { size_ = size; }
    float strength() const { return strength_; }
    void setStrength(float strength) { strength_ = strength; }
    std::string materialName() const;
    void nextMaterial();
    std::string terrainName() const { return terrainName_; }
    // Called after each palette action, so the view takes the keyboard back
    // (Ctrl+Z and T keep working after a click on the palette).
    void setOnUsed(std::function<void()> used) { onUsed_ = std::move(used); }
    void used() const {
        if (onUsed_) {
            onUsed_();
        }
    }

private:
    struct MaterialChoice {
        int id = 0;
        std::string name;
    };

    void refreshMaterials(engine_core::DataModel& world);
    void pick(const engine_core::DraggerRay& ray);
    void applyBall(bool shift);
    void applyBox();
    void beginStroke();
    void endStroke();

    engine_core::Engine& engine_;
    bool active_ = false;
    engine_core::InstanceId terrain_ = 0;
    std::string terrainName_;
    TerrainTool tool_ = TerrainTool::Grow;
    float size_ = 8.f;
    float strength_ = 0.5f;
    std::vector<MaterialChoice> materials_;
    std::size_t material_ = 0;

    // The pointer's hit on the Terrain (world space), if any.
    bool hit_ = false;
    engine_core::Vec3 hitPosition_{};
    engine_core::Vec3 hitNormal_{0.f, 1.f, 0.f};
    // Add: the drag's corners on the grid floor (y = 0).
    std::optional<engine_core::DraggerRay> ray_;
    bool held_ = false;
    bool boxing_ = false;
    engine_core::Vec3 boxStart_{};
    engine_core::Vec3 boxEnd_{};
    double lastApply_ = 0.0;

    // The stroke in progress: its Terrain's chunks before it began.
    bool stroking_ = false;
    engine_core::terrain::ChunkMap strokeBefore_;
    std::function<void()> onUsed_;
};

}  // namespace runner
