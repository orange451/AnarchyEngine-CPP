#include "TerrainBrush.hpp"

#include "ChangeHistoryService.hpp"
#include "Engine.hpp"
#include "PhysicsWorld.hpp"
#include "SceneService.hpp"
#include "SelectionService.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "terrain/ShapeDistance.hpp"

#include <jadefx/jadefx.hpp>
#include <jadefx/scene/controls/Button.hpp>
#include <jadefx/scene/controls/Label.hpp>
#include <jadefx/scene/controls/Slider.hpp>
#include <jadefx/scene/controls/ToggleButton.hpp>
#include <jadefx/paint/Color.hpp>
#include <jadefx/scene/layout/HBox.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace runner {

using engine_core::DataModel;
using engine_core::InstanceId;
using engine_core::Terrain;
using engine_core::Vec3;
using engine_core::terrain::Shape;
using engine_core::terrain::VoxelVolume;

namespace {

// Grow, Smooth, and Paint apply this often while the button is held.
constexpr double kApplySeconds = 1.0 / 20.0;
constexpr float kRayLength = 4000.f;

Vec3 add(Vec3 a, Vec3 b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scale(Vec3 a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }

Terrain* terrain_of(DataModel& world, InstanceId id) {
    return id == 0 ? nullptr : dynamic_cast<Terrain*>(world.instance(id));
}

// The selected Terrain (or the Terrain of a selected TerrainMaterial), else
// the first Terrain in Workspace.
InstanceId find_terrain(DataModel& world) {
    for (InstanceId id : world.selection().get()) {
        if (terrain_of(world, id) != nullptr) {
            return id;
        }
        const InstanceId parent = world.parent(id);
        if (terrain_of(world, parent) != nullptr) {
            return parent;
        }
    }
    const InstanceId workspace = world.scene_service("Workspace");
    std::vector<InstanceId> pending{workspace};
    while (!pending.empty()) {
        const InstanceId at = pending.back();
        pending.pop_back();
        for (InstanceId child : world.get_children(at)) {
            if (terrain_of(world, child) != nullptr) {
                return child;
            }
            pending.push_back(child);
        }
    }
    return 0;
}

std::shared_ptr<jadefx::Label> small_label(const std::string& text) {
    auto label = jadefx::make<jadefx::Label>(text);
    label->setStyle("font-size: 11px;");
    label->setTextFill(jadefx::Color::rgb8(220, 222, 228));
    return label;
}

}  // namespace

// --- Palette -------------------------------------------------------------

TerrainToolPalette::TerrainToolPalette(TerrainBrush& brush) : brush_(brush) {
    setSpacing(6.0);
    setStyle("padding: 8px; background-color: rgba(24, 26, 31, 0.92); border-radius: 6px; width: 190px;");

    auto title = jadefx::make<jadefx::Label>("Terrain");
    title->setStyle("font-weight: bold;");
    title->setTextFill(jadefx::Color::rgb8(240, 242, 246));
    title_ = title.get();
    getChildren().add(std::move(title));

    auto row = jadefx::make<jadefx::HBox>();
    row->setSpacing(4.0);
    const std::array<std::pair<const char*, TerrainTool>, 4> tools{{{"Add", TerrainTool::Add},
                                                                    {"Grow", TerrainTool::Grow},
                                                                    {"Smooth", TerrainTool::Smooth},
                                                                    {"Paint", TerrainTool::Paint}}};
    for (const auto& [name, tool] : tools) {
        auto button = jadefx::make<jadefx::ToggleButton>(name);
        button->setStyle("font-size: 11px; padding: 3px 6px;");
        const TerrainTool chosen = tool;
        button->setOnAction([this, chosen](jadefx::ActionEvent&) {
            brush_.setTool(chosen);
            refresh();
            brush_.used();
        });
        tools_.push_back(button.get());
        row->getChildren().add(std::move(button));
    }
    getChildren().add(std::move(row));

    auto sizeLabel = small_label("");
    sizeLabel_ = sizeLabel.get();
    getChildren().add(std::move(sizeLabel));
    auto size = jadefx::make<jadefx::Slider>(1.0, 48.0, brush_.size());
    size->setOnValueChanged([this] {
        brush_.setSize(static_cast<float>(size_->getValue()));
        refresh();
    });
    size_ = size.get();
    getChildren().add(std::move(size));

    auto strengthLabel = small_label("");
    strengthLabel_ = strengthLabel.get();
    getChildren().add(std::move(strengthLabel));
    auto strength = jadefx::make<jadefx::Slider>(0.05, 1.0, brush_.strength());
    strength->setOnValueChanged([this] {
        brush_.setStrength(static_cast<float>(strength_->getValue()));
        refresh();
    });
    strength_ = strength.get();
    getChildren().add(std::move(strength));

    auto material = jadefx::make<jadefx::Button>("");
    material->setStyle("font-size: 11px; width: 100%;");
    material->setOnAction([this](jadefx::ActionEvent&) {
        brush_.nextMaterial();
        refresh();
        brush_.used();
    });
    material_ = material.get();
    getChildren().add(std::move(material));

    auto history = jadefx::make<jadefx::HBox>();
    history->setSpacing(4.0);
    auto undo = jadefx::make<jadefx::Button>("Undo");
    undo->setStyle("font-size: 11px;");
    undo->setOnAction([this](jadefx::ActionEvent&) {
        brush_.undo();
        brush_.used();
    });
    auto redo = jadefx::make<jadefx::Button>("Redo");
    redo->setStyle("font-size: 11px;");
    redo->setOnAction([this](jadefx::ActionEvent&) {
        brush_.redo();
        brush_.used();
    });
    history->getChildren().add(std::move(undo));
    history->getChildren().add(std::move(redo));
    getChildren().add(std::move(history));

    getChildren().add(small_label("Shift: dig / erase.  T: close."));
    refresh();
}

void TerrainToolPalette::refresh() {
    title_->setText("Terrain: " + brush_.terrainName());
    for (std::size_t i = 0; i < tools_.size(); ++i) {
        tools_[i]->setSelected(static_cast<std::size_t>(brush_.tool()) == i);
    }
    char text[64];
    std::snprintf(text, sizeof text, "Size  %.0f units", brush_.size());
    sizeLabel_->setText(text);
    std::snprintf(text, sizeof text, "Strength  %.0f%%", brush_.strength() * 100.f);
    strengthLabel_->setText(text);
    material_->setText("Material: " + brush_.materialName() + "  ▸");
}

// --- Brush ---------------------------------------------------------------

TerrainBrush::TerrainBrush(engine_core::Engine& engine) : engine_(engine) {}

std::shared_ptr<TerrainToolPalette> TerrainBrush::makePalette() {
    return jadefx::make<TerrainToolPalette>(*this);
}

bool TerrainBrush::toggle() {
    if (active_) {
        turnOff();
        return true;
    }
    if (!engine_.paused()) {
        return false;   // edit mode only: during Play the game owns the terrain
    }
    InstanceId found = 0;
    engine_.on_simulation([&](DataModel& world) {
        found = find_terrain(world);
        if (found != 0) {
            terrainName_ = world.name(found);
            refreshMaterials(world);
        }
    });
    if (found == 0) {
        return false;
    }
    terrain_ = found;
    active_ = true;
    return true;
}

void TerrainBrush::turnOff() {
    if (held_) {
        endStroke();
    }
    active_ = false;
    held_ = false;
    boxing_ = false;
    hit_ = false;
}

void TerrainBrush::refreshMaterials(DataModel& world) {
    const std::string keep = material_ < materials_.size() ? materials_[material_].name : std::string();
    materials_.clear();
    if (Terrain* terrain = terrain_of(world, terrain_ != 0 ? terrain_ : find_terrain(world))) {
        for (engine_core::TerrainMaterial* entry : terrain->materials()) {
            materials_.push_back(MaterialChoice{entry->material_id(), world.name(entry->id())});
        }
    }
    if (materials_.empty()) {
        materials_.push_back(MaterialChoice{0, "Default"});
    }
    material_ = 0;
    for (std::size_t i = 0; i < materials_.size(); ++i) {
        if (materials_[i].name == keep) {
            material_ = i;
        }
    }
}

std::string TerrainBrush::materialName() const {
    return material_ < materials_.size() ? materials_[material_].name : std::string("Default");
}

void TerrainBrush::nextMaterial() {
    engine_.on_simulation([&](DataModel& world) { refreshMaterials(world); });
    if (!materials_.empty()) {
        material_ = (material_ + 1) % materials_.size();
    }
}

void TerrainBrush::pick(const engine_core::DraggerRay& ray) {
    hit_ = false;
    const InstanceId terrain = terrain_;
    engine_.on_simulation([&](DataModel& world) {
        engine_core::PhysicsWorld* physics = world.physics();
        if (physics == nullptr || terrain_of(world, terrain) == nullptr) {
            return;
        }
        engine_core::RayFilter filter;
        filter.include = true;
        filter.instances = {terrain};
        if (const auto hit = physics->raycast(world, ray.origin, scale(ray.direction, kRayLength), filter)) {
            hit_ = true;
            hitPosition_ = hit->position;
            hitNormal_ = hit->normal;
        }
    });
}

void TerrainBrush::hover(const std::optional<engine_core::DraggerRay>& ray) {
    ray_ = ray;
    if (active_ && !engine_.paused()) {
        turnOff();   // Play started: the tools step aside
    }
    if (!active_ || !ray) {
        hit_ = false;
        return;
    }
    if (tool_ == TerrainTool::Add) {
        hit_ = false;
        if (boxing_ && std::fabs(ray->direction.y) > 1e-5f) {
            const float t = -ray->origin.y / ray->direction.y;
            if (t > 0.f) {
                boxEnd_ = add(ray->origin, scale(ray->direction, t));
            }
        }
        return;
    }
    pick(*ray);
}

void TerrainBrush::beginStroke() {
    // One undo step for the whole stroke: every edit it makes joins it
    // (Terrain::edit_volume notes the voxels from before the first).
    engine_.on_simulation([&](DataModel& world) {
        if (auto id = world.history().try_begin_recording("Sculpt Terrain")) {
            recording_ = std::move(*id);
        }
    });
}

void TerrainBrush::endStroke() {
    if (recording_.empty()) {
        return;
    }
    std::string recording = std::move(recording_);
    recording_.clear();
    engine_.on_simulation([&](DataModel& world) {
        world.history().finish_recording(recording, engine_core::FinishRecordingOperation::Commit);
    });
}

void TerrainBrush::press(const engine_core::DraggerRay& ray, bool shift) {
    if (!active_ || !engine_.paused()) {
        return;
    }
    ray_ = ray;
    if (tool_ == TerrainTool::Add) {
        if (std::fabs(ray.direction.y) < 1e-5f) {
            return;
        }
        const float t = -ray.origin.y / ray.direction.y;
        if (t <= 0.f) {
            return;
        }
        boxStart_ = add(ray.origin, scale(ray.direction, t));
        boxEnd_ = boxStart_;
        boxing_ = true;
        return;
    }
    pick(ray);
    beginStroke();
    held_ = true;
    lastApply_ = 0.0;
    applyBall(shift);
}

void TerrainBrush::release() {
    if (boxing_) {
        boxing_ = false;
        applyBox();
    }
    if (held_) {
        held_ = false;
        endStroke();
    }
}

void TerrainBrush::tick(double now_seconds, bool shift) {
    if (!active_ || !held_ || !engine_.paused()) {
        return;
    }
    if (now_seconds - lastApply_ < kApplySeconds) {
        return;
    }
    lastApply_ = now_seconds;
    if (ray_) {
        pick(*ray_);
    }
    applyBall(shift);
}

void TerrainBrush::applyBall(bool shift) {
    if (!hit_) {
        return;
    }
    const InstanceId terrain = terrain_;
    const TerrainTool tool = tool_;
    const float radius = size_ * 0.5f;
    const float strength = strength_;
    const std::uint8_t material =
        static_cast<std::uint8_t>(material_ < materials_.size() ? materials_[material_].id : 0);
    const Vec3 position = hitPosition_;
    const Vec3 normal = hitNormal_;
    engine_.on_simulation([&](DataModel& world) {
        Terrain* found = terrain_of(world, terrain);
        if (found == nullptr) {
            return;
        }
        const engine_core::Matrix4 inverse = engine_core::matrix4_inverse(found->transform());
        const Vec3 local = engine_core::matrix4_point(inverse, position);
        Vec3 localNormal = engine_core::matrix4_vector(inverse, normal);
        const float length = std::sqrt(localNormal.x * localNormal.x + localNormal.y * localNormal.y +
                                       localNormal.z * localNormal.z);
        localNormal = length > 1e-6f ? scale(localNormal, 1.f / length) : Vec3{0.f, 1.f, 0.f};
        found->edit_volume([&](VoxelVolume& volume) -> std::optional<std::string> {
            Shape shape;
            shape.kind = Shape::Kind::Ball;
            shape.radius = radius;
            switch (tool) {
            case TerrainTool::Grow: {
                // Mostly sunk below the surface, so each application raises
                // (or, with Shift, lowers) the ground by a fraction of the
                // radius rather than stamping a whole ball.
                const float sink = radius * (1.f - 0.3f * strength);
                if (shift) {
                    shape.center = add(local, scale(localNormal, sink));
                    return volume.subtract(shape);
                }
                shape.center = add(local, scale(localNormal, -sink));
                return volume.fill(shape, material);
            }
            case TerrainTool::Smooth:
                return volume.smooth(local, radius, strength);
            case TerrainTool::Paint:
                shape.center = local;
                return volume.paint(shape, shift ? 0 : material);
            case TerrainTool::Add:
                return std::nullopt;
            }
            return std::nullopt;
        });
    });
}

void TerrainBrush::applyBox() {
    const float width = std::fabs(boxEnd_.x - boxStart_.x);
    const float depth = std::fabs(boxEnd_.z - boxStart_.z);
    if (width < 0.5f || depth < 0.5f) {
        return;
    }
    beginStroke();
    const InstanceId terrain = terrain_;
    const float height = size_;
    const std::uint8_t material =
        static_cast<std::uint8_t>(material_ < materials_.size() ? materials_[material_].id : 0);
    const Vec3 center{(boxStart_.x + boxEnd_.x) * 0.5f, height * 0.5f, (boxStart_.z + boxEnd_.z) * 0.5f};
    engine_.on_simulation([&](DataModel& world) {
        Terrain* found = terrain_of(world, terrain);
        if (found == nullptr) {
            return;
        }
        const engine_core::Matrix4 inverse = engine_core::matrix4_inverse(found->transform());
        const Vec3 local = engine_core::matrix4_point(inverse, center);
        found->edit_volume([&](VoxelVolume& volume) {
            Shape shape;
            shape.kind = Shape::Kind::Block;
            shape.frame = engine_core::matrix4_translation(local.x, local.y, local.z);
            shape.size = Vec3{width, height, depth};
            return volume.fill(shape, material);
        });
    });
    endStroke();
}

bool TerrainBrush::undo() {
    if (!engine_.paused()) {
        return false;
    }
    engine_.on_simulation([](DataModel& world) { world.history().undo(); });
    return true;
}

bool TerrainBrush::redo() {
    if (!engine_.paused()) {
        return false;
    }
    engine_.on_simulation([](DataModel& world) { world.history().redo(); });
    return true;
}

void TerrainBrush::appendOutline(std::vector<float>& points) const {
    auto segment = [&](Vec3 a, Vec3 b) { points.insert(points.end(), {a.x, a.y, a.z, b.x, b.y, b.z}); };
    if (!active_) {
        return;
    }
    if (tool_ == TerrainTool::Add) {
        if (!boxing_) {
            return;
        }
        const float x0 = std::min(boxStart_.x, boxEnd_.x), x1 = std::max(boxStart_.x, boxEnd_.x);
        const float z0 = std::min(boxStart_.z, boxEnd_.z), z1 = std::max(boxStart_.z, boxEnd_.z);
        const float y0 = 0.f, y1 = size_;
        const std::array<Vec3, 8> c{Vec3{x0, y0, z0}, Vec3{x1, y0, z0}, Vec3{x1, y0, z1}, Vec3{x0, y0, z1},
                                    Vec3{x0, y1, z0}, Vec3{x1, y1, z0}, Vec3{x1, y1, z1}, Vec3{x0, y1, z1}};
        for (int i = 0; i < 4; ++i) {
            segment(c[i], c[(i + 1) % 4]);
            segment(c[4 + i], c[4 + (i + 1) % 4]);
            segment(c[i], c[4 + i]);
        }
        return;
    }
    if (!hit_) {
        return;
    }
    // A wire ball: three great circles, and a tick along the normal.
    constexpr int kSegments = 32;
    const float r = size_ * 0.5f;
    const Vec3 o = hitPosition_;
    for (int axis = 0; axis < 3; ++axis) {
        for (int i = 0; i < kSegments; ++i) {
            const float a0 = 6.2831853f * i / kSegments;
            const float a1 = 6.2831853f * (i + 1) / kSegments;
            auto point = [&](float a) {
                const float u = std::cos(a) * r, v = std::sin(a) * r;
                return axis == 0 ? Vec3{o.x + u, o.y + v, o.z} : axis == 1 ? Vec3{o.x + u, o.y, o.z + v}
                                                                          : Vec3{o.x, o.y + u, o.z + v};
            };
            segment(point(a0), point(a1));
        }
    }
    segment(o, add(o, scale(hitNormal_, r)));
}

}  // namespace runner
