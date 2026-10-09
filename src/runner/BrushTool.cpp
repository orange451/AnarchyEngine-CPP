#include "BrushTool.hpp"

#include "AssetInstances.hpp"
#include "Brush.hpp"
#include "ChangeHistoryService.hpp"
#include "Engine.hpp"
#include "PhysicsWorld.hpp"
#include "SelectionService.hpp"

#include <jadefx/jadefx.hpp>
#include <jadefx/paint/Color.hpp>
#include <jadefx/scene/controls/Button.hpp>
#include <jadefx/scene/controls/Label.hpp>
#include <jadefx/scene/controls/ToggleButton.hpp>
#include <jadefx/scene/layout/HBox.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace runner {

using engine_core::Brush;
using engine_core::DataModel;
using engine_core::DraggerRay;
using engine_core::InstanceId;
using engine_core::Matrix4;
using engine_core::Vec2;
using engine_core::Vec3;
namespace geo = engine_core::brush;

namespace {

constexpr float kRayLength = 8000.f;
// A press that moves this far, in points, is a drag; less is a click.
constexpr float kDragPixels = 4.f;
// Vertex handles are picked this close, in points.
constexpr float kCornerPixels = 10.f;
constexpr std::array<double, 12> kGrids{0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256};

struct Rgba {
    float r, g, b, a;
};
constexpr Rgba kDraw{1.f, 0.62f, 0.15f, 1.f};
constexpr Rgba kHover{1.f, 1.f, 1.f, 0.45f};
constexpr Rgba kFace{1.f, 0.85f, 0.2f, 1.f};
constexpr Rgba kGridLine{0.75f, 0.8f, 0.9f, 0.22f};
constexpr Rgba kCorner{0.3f, 0.85f, 1.f, 1.f};
constexpr Rgba kCornerHot{1.f, 0.9f, 0.2f, 1.f};
constexpr Rgba kInvalid{1.f, 0.25f, 0.25f, 1.f};
constexpr Rgba kKeep{1.f, 1.f, 1.f, 0.9f};
constexpr Rgba kClipPoint{1.f, 0.35f, 0.9f, 1.f};

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross3(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float len3(Vec3 a) { return std::sqrt(dot3(a, a)); }
Vec3 unit3(Vec3 a) {
    const float l = len3(a);
    return l > 1e-12f ? mul(a, 1.f / l) : Vec3{};
}
float& axis_of(Vec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
float axis_of(const Vec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
Vec3 axis_vec(int axis, float s = 1.f) {
    Vec3 v{};
    axis_of(v, axis) = s;
    return v;
}
geo::DVec3 to_d(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 to_f(geo::DVec3 v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }

// The loop cut through a brush's face at a world point, in the brush's space.
std::optional<geo::LoopCut> loop_of(const Brush& brush, int face, Vec3 at, double grid, bool middle) {
    if (face < 0) {
        return std::nullopt;
    }
    const Vec3 local = engine_core::matrix4_point(engine_core::matrix4_inverse(brush.transform()), at);
    return geo::loop_cut(brush.shape(), static_cast<std::size_t>(face), to_d(local), grid, middle);
}

// Where the ray meets the plane through point with normal; nullopt when parallel or behind.
std::optional<Vec3> ray_plane(const DraggerRay& ray, Vec3 point, Vec3 normal) {
    const float denom = dot3(ray.direction, normal);
    if (std::fabs(denom) < 1e-6f) {
        return std::nullopt;
    }
    const float t = dot3(sub(point, ray.origin), normal) / denom;
    if (t <= 0.f) {
        return std::nullopt;
    }
    return add(ray.origin, mul(ray.direction, t));
}

// How far along the line (origin, unit dir) the point nearest the ray is.
float ray_line(const DraggerRay& ray, Vec3 origin, Vec3 dir) {
    const Vec3 r = unit3(ray.direction);
    const Vec3 w = sub(origin, ray.origin);
    const float b = dot3(dir, r);
    const float d = dot3(dir, w);
    const float e = dot3(r, w);
    const float denom = 1.f - b * b;
    if (denom < 1e-6f) {
        return 0.f;
    }
    return (b * e - d) / denom;
}

void segment(std::vector<float>& out, Vec3 a, Vec3 b, Rgba c) {
    out.insert(out.end(), {a.x, a.y, a.z, c.r, c.g, c.b, c.a, b.x, b.y, b.z, c.r, c.g, c.b, c.a});
}

void box_lines(std::vector<float>& out, Vec3 lo, Vec3 hi, Rgba c) {
    const std::array<Vec3, 8> p{Vec3{lo.x, lo.y, lo.z}, Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, lo.y, hi.z},
                                Vec3{lo.x, lo.y, hi.z}, Vec3{lo.x, hi.y, lo.z}, Vec3{hi.x, hi.y, lo.z},
                                Vec3{hi.x, hi.y, hi.z}, Vec3{lo.x, hi.y, hi.z}};
    for (int i = 0; i < 4; ++i) {
        segment(out, p[i], p[(i + 1) % 4], c);
        segment(out, p[4 + i], p[4 + (i + 1) % 4], c);
        segment(out, p[i], p[4 + i], c);
    }
}

// A brush's edges in world space.
void brush_lines(std::vector<float>& out, const Brush& brush, Rgba c) {
    const Matrix4 world = brush.transform();
    const geo::Shape& shape = brush.shape();
    for (const auto& [a, b] : shape.edges) {
        segment(out, engine_core::matrix4_point(world, to_f(shape.vertices[a])),
                engine_core::matrix4_point(world, to_f(shape.vertices[b])), c);
    }
}

void face_lines(std::vector<float>& out, const Brush& brush, int face, Rgba c) {
    const geo::Shape& shape = brush.shape();
    if (face < 0 || static_cast<std::size_t>(face) >= shape.polygons.size()) {
        return;
    }
    const Matrix4 world = brush.transform();
    const std::vector<std::uint32_t>& loop = shape.polygons[static_cast<std::size_t>(face)].vertices;
    Vec3 middle{};
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const Vec3 a = engine_core::matrix4_point(world, to_f(shape.vertices[loop[i]]));
        const Vec3 b = engine_core::matrix4_point(world, to_f(shape.vertices[loop[(i + 1) % loop.size()]]));
        segment(out, a, b, c);
        middle = add(middle, a);
    }
    if (!loop.empty()) {
        middle = mul(middle, 1.f / static_cast<float>(loop.size()));
        // An X across the face, so it reads as picked, and a tick along its normal.
        const Vec3 a = engine_core::matrix4_point(world, to_f(shape.vertices[loop[0]]));
        const Vec3 b = engine_core::matrix4_point(world, to_f(shape.vertices[loop[loop.size() / 2]]));
        segment(out, a, b, Rgba{c.r, c.g, c.b, c.a * 0.35f});
        const Vec3 n = unit3(engine_core::matrix4_vector(world, to_f(shape.planes[static_cast<std::size_t>(face)].normal)));
        segment(out, middle, add(middle, mul(n, std::max(0.5f, len3(sub(a, b)) * 0.25f))), c);
    }
}

void cross_lines(std::vector<float>& out, Vec3 p, float size, Rgba c) {
    for (int axis = 0; axis < 3; ++axis) {
        segment(out, sub(p, axis_vec(axis, size)), add(p, axis_vec(axis, size)), c);
    }
}

std::string number(double value) {
    char text[32];
    if (std::fabs(value - std::round(value)) < 1e-6) {
        std::snprintf(text, sizeof text, "%.0f", value);
    } else {
        std::snprintf(text, sizeof text, "%.3g", value);
    }
    return text;
}

Brush* brush_of(DataModel& world, InstanceId id) {
    return id == 0 ? nullptr : dynamic_cast<Brush*>(world.instance(id));
}

// Every Material under Assets (or anywhere), for the palette's picker.
void find_materials(DataModel& world, InstanceId at, std::vector<InstanceId>& out) {
    for (InstanceId child : world.get_children(at)) {
        if (dynamic_cast<engine_core::Material*>(world.instance(child)) != nullptr) {
            out.push_back(child);
        }
        find_materials(world, child, out);
    }
}

// A translation then rotation/scale matrix for brush::transform (row-major 3x4).
void linear_matrix(const double rows[3][3], geo::DVec3 pivot, double out[12]) {
    for (int r = 0; r < 3; ++r) {
        out[r * 4 + 0] = rows[r][0];
        out[r * 4 + 1] = rows[r][1];
        out[r * 4 + 2] = rows[r][2];
        const double p[3] = {pivot.x, pivot.y, pivot.z};
        out[r * 4 + 3] = p[r] - (rows[r][0] * p[0] + rows[r][1] * p[1] + rows[r][2] * p[2]);
    }
}

std::shared_ptr<jadefx::Label> small_label(const std::string& text) {
    auto label = jadefx::make<jadefx::Label>(text);
    label->setStyle("font-size: 11px;");
    label->setTextFill(jadefx::Color::rgb8(206, 210, 220));
    return label;
}

std::shared_ptr<jadefx::Button> small_button(const std::string& text) {
    auto button = jadefx::make<jadefx::Button>(text);
    button->setStyle("font-size: 11px; padding: 3px 7px;");
    return button;
}

}  // namespace

// --- Palette -------------------------------------------------------------

BrushToolPalette::BrushToolPalette(BrushTool& tool) : tool_(tool) {
    setSpacing(6.0);
    setStyle("padding: 8px; background-color: rgba(24, 26, 31, 0.92); border-radius: 6px; width: 210px;");

    auto title = jadefx::make<jadefx::Label>("Brushes");
    title->setStyle("font-weight: bold;");
    title->setTextFill(jadefx::Color::rgb8(240, 242, 246));
    getChildren().add(std::move(title));

    auto modes = jadefx::make<jadefx::HBox>();
    modes->setSpacing(4.0);
    const std::array<std::pair<const char*, BrushMode>, 4> list{
        {{"Draw", BrushMode::Draw}, {"Vertex  V", BrushMode::Vertex}, {"Clip  C", BrushMode::Clip}, {"Loop  K", BrushMode::Loop}}};
    for (const auto& [name, mode] : list) {
        auto button = jadefx::make<jadefx::ToggleButton>(name);
        button->setStyle("font-size: 11px; padding: 3px 6px;");
        const BrushMode chosen = mode;
        button->setOnAction([this, chosen](jadefx::ActionEvent&) {
            tool_.setMode(chosen);
            refresh();
            tool_.used();
        });
        modes_.push_back(button.get());
        modes->getChildren().add(std::move(button));
    }
    getChildren().add(std::move(modes));

    auto gridRow = jadefx::make<jadefx::HBox>();
    gridRow->setSpacing(4.0);
    auto smaller = small_button("-");
    smaller->setOnAction([this](jadefx::ActionEvent&) {
        tool_.stepGrid(-1);
        refresh();
        tool_.used();
    });
    auto larger = small_button("+");
    larger->setOnAction([this](jadefx::ActionEvent&) {
        tool_.stepGrid(1);
        refresh();
        tool_.used();
    });
    auto grid = small_label("");
    grid_ = grid.get();
    gridRow->getChildren().add(std::move(smaller));
    gridRow->getChildren().add(std::move(grid));
    gridRow->getChildren().add(std::move(larger));
    getChildren().add(std::move(gridRow));

    getChildren().add(small_label("Shape of the selection"));
    auto shapes = jadefx::make<jadefx::HBox>();
    shapes->setSpacing(4.0);
    const std::array<std::tuple<const char*, int, int>, 5> kinds{
        {{"Box", 0, 0}, {"Cyl 8", 1, 8}, {"Cyl 16", 1, 16}, {"Cone", 2, 12}, {"Ball", 3, 2}}};
    for (const auto& [name, kind, sides] : kinds) {
        auto button = small_button(name);
        const int k = kind;
        const int s = sides;
        button->setOnAction([this, k, s](jadefx::ActionEvent&) {
            tool_.reshapeSelection(k, s);
            tool_.used();
        });
        shapes->getChildren().add(std::move(button));
    }
    getChildren().add(std::move(shapes));

    auto materialRow = jadefx::make<jadefx::HBox>();
    materialRow->setSpacing(4.0);
    auto material = small_button("");
    material->setOnAction([this](jadefx::ActionEvent&) {
        tool_.nextMaterial();
        refresh();
        tool_.used();
    });
    material_ = material.get();
    auto apply = small_button("Apply");
    apply->setOnAction([this](jadefx::ActionEvent&) {
        tool_.applyMaterialToSelection();
        tool_.used();
    });
    materialRow->getChildren().add(std::move(material));
    materialRow->getChildren().add(std::move(apply));
    getChildren().add(std::move(materialRow));

    auto hint = small_label("");
    hint_ = hint.get();
    getChildren().add(std::move(hint));
    refresh();
}

void BrushToolPalette::refresh() {
    for (std::size_t i = 0; i < modes_.size(); ++i) {
        modes_[i]->setSelected(static_cast<std::size_t>(tool_.mode()) == i);
    }
    grid_->setText("Grid  " + number(tool_.grid()) + "   [ ]");
    material_->setText("Material: " + tool_.materialName() + "  \xE2\x96\xB8");
    hint_->setText(tool_.hint());
}

// --- Tool ----------------------------------------------------------------

BrushTool::BrushTool(engine_core::Engine& engine) : engine_(engine) { setMode(BrushMode::Draw); }

std::shared_ptr<BrushToolPalette> BrushTool::makePalette() { return jadefx::make<BrushToolPalette>(*this); }

void BrushTool::toggle() {
    if (active_) {
        turnOff();
        return;
    }
    if (!engine_.paused()) {
        return;  // edit mode only
    }
    active_ = true;
    setMode(BrushMode::Draw);
}

void BrushTool::turnOff() {
    if (!recording_.empty()) {
        endStep(drag_ != Drag::Height);
    }
    active_ = false;
    drag_ = Drag::None;
    held_.clear();
    clipPoints_.clear();
    corners_.clear();
    hover_.reset();
    lines_.clear();
    readout_.clear();
}

void BrushTool::setMode(BrushMode mode) {
    mode_ = mode;
    clipPoints_.clear();
    hoverCorner_ = -1;
    switch (mode) {
    case BrushMode::Draw:
        hint_ = "Drag: draw, move up, click\nClick: select   Drag it: move\nShift-drag a face: push/pull\nAlt-click: paint   B: close";
        break;
    case BrushMode::Vertex:
        hint_ = "Drag a corner (Alt: up/down)\nRed: would not stay convex\nV or Esc: back";
        refreshVertices();
        break;
    case BrushMode::Clip:
        hint_ = "Click 2 or 3 points\nEnter: cut   Ctrl+Enter: side\nBackspace: undo a point";
        break;
    case BrushMode::Loop:
        hint_ = "Point near an edge, click: cut\nacross it, on the grid\nShift: at the middle   K: back";
        break;
    }
}

void BrushTool::stepGrid(int steps) {
    std::size_t at = 0;
    for (std::size_t i = 0; i < kGrids.size(); ++i) {
        if (std::fabs(kGrids[i] - grid_) < 1e-9) {
            at = i;
        }
    }
    const int next = std::clamp(static_cast<int>(at) + steps, 0, static_cast<int>(kGrids.size()) - 1);
    grid_ = kGrids[static_cast<std::size_t>(next)];
}

void BrushTool::setGrid(double grid) { grid_ = grid; }

double BrushTool::snap(double value) const { return std::round(value / grid_) * grid_; }

Vec3 BrushTool::snapVec(Vec3 p) const {
    return {static_cast<float>(snap(p.x)), static_cast<float>(snap(p.y)), static_cast<float>(snap(p.z))};
}

int BrushTool::cameraAxis(bool right, int& sign) const {
    // Column 0 is right, column 2 is back (the camera looks down -Z).
    const float* m = view_.camera.m;
    Vec3 dir = right ? Vec3{m[0], m[1], m[2]} : Vec3{-m[8], -m[9], -m[10]};
    dir.y = 0.f;
    if (std::fabs(dir.x) >= std::fabs(dir.z)) {
        sign = dir.x >= 0.f ? 1 : -1;
        return 0;
    }
    sign = dir.z >= 0.f ? 1 : -1;
    return 2;
}

std::optional<BrushTool::Hit> BrushTool::pick(const DraggerRay& ray, bool brushesOnly) const {
    std::optional<Hit> out;
    engine_.on_simulation([&](DataModel& world) {
        engine_core::PhysicsWorld* physics = world.physics();
        if (physics == nullptr) {
            return;
        }
        engine_core::RayFilter filter;
        const auto hit = physics->raycast(world, ray.origin, mul(ray.direction, kRayLength), filter);
        if (!hit) {
            return;
        }
        Hit h;
        h.instance = hit->instance;
        h.brush = brush_of(world, hit->instance) != nullptr;
        h.face = hit->face;
        h.position = hit->position;
        h.normal = hit->normal;
        if (brushesOnly && !h.brush) {
            return;
        }
        out = h;
    });
    return out;
}

std::vector<InstanceId> BrushTool::selection() const {
    std::vector<InstanceId> out;
    engine_.on_simulation([&](DataModel& world) {
        for (InstanceId id : world.selection().get()) {
            if (brush_of(world, id) != nullptr) {
                out.push_back(id);
            }
        }
    });
    return out;
}

void BrushTool::select(std::vector<InstanceId> ids) const {
    engine_.on_simulation([&](DataModel& world) { world.selection().set(std::move(ids)); });
}

bool BrushTool::selected(InstanceId id) const {
    const std::vector<InstanceId> ids = selection();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void BrushTool::beginStep(const char* name) {
    if (!recording_.empty()) {
        return;
    }
    engine_.on_simulation([&](DataModel& world) {
        if (auto id = world.history().try_begin_recording(name)) {
            recording_ = std::move(*id);
        }
    });
}

void BrushTool::endStep(bool commit) {
    if (recording_.empty()) {
        return;
    }
    std::string recording = std::move(recording_);
    recording_.clear();
    engine_.on_simulation([&](DataModel& world) {
        world.history().finish_recording(recording, commit ? engine_core::FinishRecordingOperation::Commit
                                                           : engine_core::FinishRecordingOperation::Cancel);
    });
}

void BrushTool::captureHeld(int face) {
    held_.clear();
    const std::vector<InstanceId> ids = selection();
    engine_.on_simulation([&](DataModel& world) {
        for (InstanceId id : ids) {
            if (Brush* brush = brush_of(world, id)) {
                held_.push_back(Held{id, brush->transform(), brush->faces(), face});
            }
        }
    });
}

// --- Pointer -------------------------------------------------------------

void BrushTool::hover(const std::optional<DraggerRay>& ray, BrushModifiers mods) {
    if (active_ && !engine_.paused()) {
        turnOff();  // Play started: the tool steps aside
        return;
    }
    if (!active_) {
        return;
    }
    ray_ = ray;
    mods_ = mods;
    if (!ray) {
        hover_.reset();
        lines_.clear();
        readout_.clear();
        return;
    }
    switch (drag_) {
    case Drag::Pending: {
        Vec2 at{};
        if (engine_core::project_point(view_, add(ray->origin, mul(ray->direction, 10.f)), at)) {
            Vec2 from{};
            engine_core::project_point(view_, add(pressRay_.origin, mul(pressRay_.direction, 10.f)), from);
            if (std::hypot(at.x - from.x, at.y - from.y) >= kDragPixels) {
                startDrag(pressMods_);
            }
        }
        break;
    }
    case Drag::Drawing:
        updateDraw(*ray, mods);
        break;
    case Drag::Height:
        updateHeight(*ray, mods);
        break;
    case Drag::Moving:
        updateMove(*ray, mods);
        break;
    case Drag::Resizing:
        updateResize(*ray);
        break;
    case Drag::Vertex:
        updateVertex(*ray, mods);
        break;
    case Drag::None:
        break;
    }
    refreshHover(*ray, mods);
}

void BrushTool::press(const DraggerRay& ray, BrushModifiers mods, int clicks) {
    if (!active_ || !engine_.paused()) {
        return;
    }
    ray_ = ray;
    mods_ = mods;
    if (drag_ == Drag::Height) {
        commitDraw();
        refreshHover(ray, mods);
        return;
    }
    if (mode_ == BrushMode::Clip) {
        clickClip(ray);
        refreshHover(ray, mods);
        return;
    }
    if (mode_ == BrushMode::Loop) {
        applyLoop(ray, mods);
        refreshHover(ray, mods);
        return;
    }
    if (mode_ == BrushMode::Vertex && hoverCorner_ >= 0) {
        dragCorner_ = hoverCorner_;
        captureHeld(-1);
        dragOrigin_ = corners_[static_cast<std::size_t>(dragCorner_)].world;
        dragInvalid_ = false;
        beginStep("Move Vertex");
        drag_ = Drag::Vertex;
        return;
    }
    pressRay_ = ray;
    pressMods_ = mods;
    pressClicks_ = clicks;
    pressHit_ = pick(ray, false);
    if (mods.alt && pressHit_ && pressHit_->brush) {
        paint(*pressHit_, clicks >= 2);
        return;
    }
    drag_ = Drag::Pending;
}

void BrushTool::release(BrushModifiers mods) {
    if (!active_) {
        return;
    }
    mods_ = mods;
    switch (drag_) {
    case Drag::Pending: {
        // A click: select what is under it.
        drag_ = Drag::None;
        if (mode_ != BrushMode::Draw) {
            break;
        }
        if (pressHit_ && pressHit_->brush) {
            std::vector<InstanceId> ids = pressMods_.control ? selection() : std::vector<InstanceId>{};
            const auto found = std::find(ids.begin(), ids.end(), pressHit_->instance);
            if (found != ids.end()) {
                ids.erase(found);
            } else {
                ids.push_back(pressHit_->instance);
            }
            select(std::move(ids));
        } else if (!pressMods_.control) {
            select({});
        }
        break;
    }
    case Drag::Drawing: {
        const Vec3 d = sub(drawEnd_, drawStart_);
        const int u = (planeAxis_ + 1) % 3;
        const int v = (planeAxis_ + 2) % 3;
        if (std::fabs(axis_of(d, u)) < 1e-4f || std::fabs(axis_of(d, v)) < 1e-4f) {
            drag_ = Drag::None;  // no footprint: nothing drawn
        } else {
            drag_ = Drag::Height;  // now the height, until a click
            height_ = lastHeight_;
            if (ray_) {
                updateHeight(*ray_, mods);
            }
        }
        break;
    }
    case Drag::Resizing:
        if (pressMods_.control && std::fabs(dragAmount_) > 1e-6) {
            extrude();
        }
        [[fallthrough]];
    case Drag::Moving:
    case Drag::Vertex:
        drag_ = Drag::None;
        endStep(true);
        held_.clear();
        dragCorner_ = -1;
        if (mode_ == BrushMode::Vertex) {
            refreshVertices();
        }
        break;
    default:
        break;
    }
    if (ray_) {
        refreshHover(*ray_, mods);
    }
}

void BrushTool::startDrag(BrushModifiers mods) {
    const bool onSelected = pressHit_ && pressHit_->brush && selected(pressHit_->instance);
    if (onSelected && mods.shift && pressHit_->face >= 0) {
        // Push or pull the face (and the same face of every selected brush).
        captureHeld(-1);
        dragOrigin_ = pressHit_->position;
        dragAmount_ = 0.0;
        dragInvalid_ = false;
        engine_.on_simulation([&](DataModel& world) {
            const Brush* hit = brush_of(world, pressHit_->instance);
            if (hit == nullptr) {
                return;
            }
            const geo::Plane plane = hit->shape().planes[static_cast<std::size_t>(pressHit_->face)];
            const Matrix4 hitWorld = hit->transform();
            dragNormal_ = unit3(engine_core::matrix4_vector(hitWorld, to_f(plane.normal)));
            const Vec3 point = engine_core::matrix4_point(hitWorld, to_f(plane.normal * plane.distance));
            for (Held& held : held_) {
                const Brush* brush = brush_of(world, held.id);
                const Matrix4 inverse = engine_core::matrix4_inverse(held.transform);
                const Vec3 n = unit3(engine_core::matrix4_vector(inverse, dragNormal_));
                const Vec3 p = engine_core::matrix4_point(inverse, point);
                // The face of this brush on the same plane.
                for (std::size_t i = 0; i < brush->shape().planes.size(); ++i) {
                    const geo::Plane& q = brush->shape().planes[i];
                    if (geo::dot(q.normal, to_d(n)) > 0.9999 &&
                        std::fabs(q.distance - geo::dot(q.normal, to_d(p))) < 1e-3) {
                        held.face = static_cast<int>(i);
                    }
                }
            }
        });
        if (mods.control) {
            // Extrude: a new brush grows out of the face instead.
            beginStep("Extrude Brush");
        } else {
            beginStep("Resize Brush");
        }
        pressMods_ = mods;
        drag_ = Drag::Resizing;
        return;
    }
    if (onSelected) {
        captureHeld(-1);
        dragOrigin_ = pressHit_->position;
        dragDelta_ = {};
        beginStep(mods.control ? "Copy Brushes" : "Move Brushes");
        if (mods.control) {
            duplicate();
            captureHeld(-1);
        }
        drag_ = Drag::Moving;
        return;
    }
    if (mode_ != BrushMode::Draw) {
        drag_ = Drag::None;
        return;
    }
    // Draw on the surface pressed (its nearest axis plane), else on the
    // working plane: horizontal at the last brush's base.
    planeAxis_ = 1;
    planeSign_ = 1.0;
    if (pressHit_) {
        const Vec3 n = pressHit_->normal;
        const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        planeAxis_ = ax > ay && ax > az ? 0 : (az > ay ? 2 : 1);
        planeSign_ = axis_of(n, planeAxis_) >= 0.f ? 1.0 : -1.0;
        // A slope draws on a level plane through the point hit.
        if (std::max({ax, ay, az}) < 0.999f) {
            planeAxis_ = 1;
            planeSign_ = 1.0;
        }
        planePoint_ = pressHit_->position;
        // Snap the plane onto the grid only when it is close: a face off the
        // grid keeps its own height, so a box drawn on it sits on it.
        const float along = axis_of(planePoint_, planeAxis_);
        if (std::fabs(snap(along) - along) < 1e-3) {
            axis_of(planePoint_, planeAxis_) = static_cast<float>(snap(along));
        }
    } else if (const auto at = ray_plane(pressRay_, Vec3{0.f, 0.f, 0.f}, Vec3{0.f, 1.f, 0.f})) {
        planePoint_ = *at;
        planePoint_.y = 0.f;
    } else {
        drag_ = Drag::None;
        return;
    }
    const auto start = ray_plane(pressRay_, planePoint_, axis_vec(planeAxis_));
    if (!start) {
        drag_ = Drag::None;
        return;
    }
    drawStart_ = snapVec(*start);
    axis_of(drawStart_, planeAxis_) = axis_of(planePoint_, planeAxis_);
    drawEnd_ = drawStart_;
    drag_ = Drag::Drawing;
    if (ray_) {
        updateDraw(*ray_, mods);
    }
}

void BrushTool::updateDraw(const DraggerRay& ray, BrushModifiers mods) {
    if (mods.alt) {
        // Alt mid-drag: the footprint is done; the pointer sets the height now.
        drag_ = Drag::Height;
        height_ = lastHeight_;
        updateHeight(ray, mods);
        return;
    }
    const auto at = ray_plane(ray, planePoint_, axis_vec(planeAxis_));
    if (!at) {
        return;
    }
    Vec3 end = snapVec(*at);
    axis_of(end, planeAxis_) = axis_of(planePoint_, planeAxis_);
    const int u = (planeAxis_ + 1) % 3;
    const int v = (planeAxis_ + 2) % 3;
    if (mods.shift) {
        // Square: the longer side wins.
        const float du = axis_of(end, u) - axis_of(drawStart_, u);
        const float dv = axis_of(end, v) - axis_of(drawStart_, v);
        const float side = std::max(std::fabs(du), std::fabs(dv));
        axis_of(end, u) = axis_of(drawStart_, u) + (du < 0.f ? -side : side);
        axis_of(end, v) = axis_of(drawStart_, v) + (dv < 0.f ? -side : side);
    }
    drawEnd_ = end;
}

void BrushTool::updateHeight(const DraggerRay& ray, BrushModifiers mods) {
    const Vec3 normal = axis_vec(planeAxis_, static_cast<float>(planeSign_));
    const float h = ray_line(ray, drawEnd_, normal);
    double height = snap(h);
    if (std::fabs(height) < grid_) {
        height = h < 0.f ? -grid_ : grid_;
    }
    if (mods.shift) {
        // A cube: as tall as the footprint's longer side.
        const int u = (planeAxis_ + 1) % 3;
        const int v = (planeAxis_ + 2) % 3;
        const float side = std::max(std::fabs(axis_of(drawEnd_, u) - axis_of(drawStart_, u)),
                                    std::fabs(axis_of(drawEnd_, v) - axis_of(drawStart_, v)));
        height = height < 0.0 ? -side : side;
    }
    height_ = height;
}

void BrushTool::commitDraw() {
    drag_ = Drag::None;
    const Vec3 normal = axis_vec(planeAxis_, static_cast<float>(planeSign_ * height_));
    const Vec3 far = add(drawEnd_, normal);
    const Vec3 lo{std::min({drawStart_.x, drawEnd_.x, far.x}), std::min({drawStart_.y, drawEnd_.y, far.y}),
                  std::min({drawStart_.z, drawEnd_.z, far.z})};
    const Vec3 hi{std::max({drawStart_.x, drawEnd_.x, far.x}), std::max({drawStart_.y, drawEnd_.y, far.y}),
                  std::max({drawStart_.z, drawEnd_.z, far.z})};
    const Vec3 size = sub(hi, lo);
    if (size.x < 1e-4f || size.y < 1e-4f || size.z < 1e-4f) {
        return;
    }
    lastHeight_ = std::fabs(height_);
    const Vec3 middle = mul(add(lo, hi), 0.5f);
    beginStep("Draw Brush");
    InstanceId made = 0;
    const std::string material = materialGuid_;
    engine_.on_simulation([&](DataModel& world) {
        Brush& brush = world.create<Brush>();
        world.set_name(brush.id(), "Brush");
        brush.set_transform(engine_core::matrix4_translation(middle.x, middle.y, middle.z));
        std::vector<geo::Face> faces = geo::make_box({size.x, size.y, size.z});
        for (geo::Face& face : faces) {
            face.material = material;
        }
        brush.set_faces(std::move(faces));
        world.set_parent(brush.id(), world.scene_service("Workspace"));
        made = brush.id();
    });
    endStep(true);
    if (made != 0) {
        select({made});
    }
}

void BrushTool::updateMove(const DraggerRay& ray, BrushModifiers mods) {
    std::optional<Vec3> at;
    Vec3 delta{};
    if (mods.alt) {
        // Up and down: a vertical plane through the press, facing the camera.
        const float* m = view_.camera.m;
        Vec3 facing{m[8], 0.f, m[10]};
        facing = len3(facing) > 1e-4f ? unit3(facing) : Vec3{0.f, 0.f, 1.f};
        at = ray_plane(ray, dragOrigin_, facing);
        if (!at) {
            return;
        }
        delta = Vec3{0.f, at->y - dragOrigin_.y, 0.f};
    } else {
        at = ray_plane(ray, dragOrigin_, Vec3{0.f, 1.f, 0.f});
        if (!at) {
            return;
        }
        delta = sub(*at, dragOrigin_);
        delta.y = 0.f;
    }
    if (mods.shift) {
        // One axis: whichever the pointer has gone furthest along.
        const int axis = std::fabs(delta.x) >= std::fabs(delta.z) ? 0 : 2;
        axis_of(delta, 2 - axis) = 0.f;
    }
    delta = snapVec(delta);
    dragDelta_ = delta;
    const std::vector<Held> held = held_;
    engine_.on_simulation([&](DataModel& world) {
        for (const Held& item : held) {
            if (Brush* brush = brush_of(world, item.id)) {
                Matrix4 moved = item.transform;
                moved.m[12] += delta.x;
                moved.m[13] += delta.y;
                moved.m[14] += delta.z;
                brush->set_transform(moved);
            }
        }
    });
}

void BrushTool::updateResize(const DraggerRay& ray) {
    double amount = snap(ray_line(ray, dragOrigin_, dragNormal_));
    dragAmount_ = amount;
    const bool extrude = pressMods_.control;
    const std::vector<Held> held = held_;
    bool invalid = false;
    engine_.on_simulation([&](DataModel& world) {
        for (const Held& item : held) {
            Brush* brush = brush_of(world, item.id);
            if (brush == nullptr || item.face < 0) {
                continue;
            }
            if (extrude) {
                continue;
            }
            // Distance in the brush's own space (its Transform may scale).
            const Matrix4 inverse = engine_core::matrix4_inverse(item.transform);
            const float local = len3(engine_core::matrix4_vector(inverse, mul(dragNormal_, static_cast<float>(amount))));
            geo::Built built = geo::move_face(item.faces, static_cast<std::size_t>(item.face), amount >= 0 ? local : -local);
            if (!built.ok()) {
                invalid = true;
                continue;
            }
            brush->apply(std::move(built));
        }
    });
    dragInvalid_ = invalid;
}

void BrushTool::updateVertex(const DraggerRay& ray, BrushModifiers mods) {
    if (dragCorner_ < 0 || static_cast<std::size_t>(dragCorner_) >= corners_.size()) {
        return;
    }
    const Corner corner = corners_[static_cast<std::size_t>(dragCorner_)];
    Vec3 target;
    if (mods.alt) {
        const float* m = view_.camera.m;
        Vec3 facing{m[8], 0.f, m[10]};
        facing = len3(facing) > 1e-4f ? unit3(facing) : Vec3{0.f, 0.f, 1.f};
        const auto at = ray_plane(ray, dragOrigin_, facing);
        if (!at) {
            return;
        }
        target = dragOrigin_;
        target.y = static_cast<float>(snap(at->y));
    } else {
        // On the camera-facing world plane through the corner.
        const float* m = view_.camera.m;
        const Vec3 back{m[8], m[9], m[10]};
        const float ax = std::fabs(back.x), ay = std::fabs(back.y), az = std::fabs(back.z);
        const int axis = ax > ay && ax > az ? 0 : (az > ay ? 2 : 1);
        const auto at = ray_plane(ray, dragOrigin_, axis_vec(axis));
        if (!at) {
            return;
        }
        target = snapVec(*at);
        axis_of(target, axis) = axis_of(dragOrigin_, axis);
    }
    bool invalid = false;
    const std::vector<Held> held = held_;
    engine_.on_simulation([&](DataModel& world) {
        for (const Held& item : held) {
            if (item.id != corner.brush) {
                continue;
            }
            Brush* brush = brush_of(world, item.id);
            if (brush == nullptr) {
                continue;
            }
            const geo::Built start = geo::build(item.faces);
            if (!start.ok()) {
                continue;
            }
            const Matrix4 inverse = engine_core::matrix4_inverse(item.transform);
            // From where the corner was when the drag began: item.faces are the brush then.
            const geo::DVec3 from = to_d(engine_core::matrix4_point(inverse, dragOrigin_));
            const geo::DVec3 to = to_d(engine_core::matrix4_point(inverse, target));
            std::vector<geo::DVec3> points = start.shape.vertices;
            for (geo::DVec3& p : points) {
                if (geo::length(p - from) < 1e-4) {
                    p = to;
                }
            }
            const auto faces = geo::hull_faces(points);
            if (!faces) {
                invalid = true;
                continue;
            }
            geo::Built built = geo::build(*faces);
            // Every corner must still be a corner, or the edit made it concave.
            bool kept = built.ok();
            for (const geo::DVec3& p : points) {
                bool found = false;
                for (const geo::DVec3& q : built.shape.vertices) {
                    if (geo::length(p - q) < 1e-4) {
                        found = true;
                        break;
                    }
                }
                kept = kept && found;
            }
            if (!kept) {
                invalid = true;
                continue;
            }
            // Keep each face's Material where its plane survives.
            for (geo::Face& face : built.faces) {
                const auto plane = geo::plane_of(face);
                for (const geo::Face& old : item.faces) {
                    const auto was = geo::plane_of(old);
                    if (plane && was && geo::dot(plane->normal, was->normal) > 0.9999 &&
                        std::fabs(plane->distance - was->distance) < 1e-4) {
                        face.material = old.material;
                        face.u_axis = old.u_axis;
                        face.v_axis = old.v_axis;
                        face.offset_u = old.offset_u;
                        face.offset_v = old.offset_v;
                        face.scale_u = old.scale_u;
                        face.scale_v = old.scale_v;
                        face.rotation = old.rotation;
                    } else if (face.material.empty() && !old.material.empty()) {
                        face.material = old.material;
                    }
                }
            }
            brush->apply(std::move(built));
        }
    });
    dragInvalid_ = invalid;
    if (!invalid) {
        corners_[static_cast<std::size_t>(dragCorner_)].world = target;
    }
    dragDelta_ = target;
}

void BrushTool::extrude() {
    // A new brush: each held face's polygon swept along its normal.
    const Vec3 offset = mul(dragNormal_, static_cast<float>(dragAmount_));
    std::vector<InstanceId> made;
    const std::vector<Held> held = held_;
    engine_.on_simulation([&](DataModel& world) {
        for (const Held& item : held) {
            const Brush* brush = brush_of(world, item.id);
            if (brush == nullptr || item.face < 0) {
                continue;
            }
            const Matrix4 m = brush->transform();
            const Vec3 origin{m.m[12], m.m[13], m.m[14]};
            std::vector<geo::DVec3> points;
            for (std::uint32_t index : brush->shape().polygons[static_cast<std::size_t>(item.face)].vertices) {
                const Vec3 p = sub(engine_core::matrix4_point(m, to_f(brush->shape().vertices[index])), origin);
                points.push_back(to_d(p));
                points.push_back(to_d(add(p, offset)));
            }
            const auto faces = geo::hull_faces(points);
            if (!faces) {
                continue;
            }
            geo::Built built = geo::build(*faces);
            if (!built.ok()) {
                continue;
            }
            const std::string material = brush->faces()[static_cast<std::size_t>(item.face)].material;
            for (geo::Face& face : built.faces) {
                face.material = material;
            }
            Brush& copy = world.create<Brush>();
            world.set_name(copy.id(), world.name(item.id));
            copy.set_transform(engine_core::matrix4_translation(origin.x, origin.y, origin.z));
            copy.apply(std::move(built));
            world.set_parent(copy.id(), world.parent(item.id));
            made.push_back(copy.id());
        }
    });
    if (!made.empty()) {
        select(std::move(made));
    }
}

void BrushTool::paint(const Hit& hit, bool wholeBrush) {
    const std::string material = materialGuid_;
    beginStep("Paint Brush");
    engine_.on_simulation([&](DataModel& world) {
        Brush* brush = brush_of(world, hit.instance);
        if (brush == nullptr) {
            return;
        }
        std::vector<geo::Face> faces = brush->faces();
        for (std::size_t i = 0; i < faces.size(); ++i) {
            if (wholeBrush || static_cast<int>(i) == hit.face) {
                faces[i].material = material;
            }
        }
        brush->set_faces(std::move(faces));
    });
    endStep(true);
}

void BrushTool::clickClip(const DraggerRay& ray) {
    const std::optional<Hit> hit = pick(ray, false);
    if (!hit) {
        return;
    }
    if (clipPoints_.size() >= 3) {
        clipPoints_.clear();
    }
    if (clipPoints_.empty()) {
        clipNormal_ = hit->normal;
    }
    Vec3 p = snapVec(hit->position);
    // Stay on the face clicked: only the in-face axes snap.
    const float ax = std::fabs(hit->normal.x), ay = std::fabs(hit->normal.y), az = std::fabs(hit->normal.z);
    const int axis = ax > ay && ax > az ? 0 : (az > ay ? 2 : 1);
    axis_of(p, axis) = axis_of(hit->position, axis);
    clipPoints_.push_back(p);
}

void BrushTool::applyClip() {
    if (clipPoints_.size() < 2) {
        return;
    }
    // Two points: the plane through them standing on the first face clicked.
    Vec3 a = clipPoints_[0];
    Vec3 b = clipPoints_[1];
    Vec3 c = clipPoints_.size() >= 3 ? clipPoints_[2] : add(a, clipNormal_);
    Vec3 n = unit3(cross3(sub(b, a), sub(c, a)));
    if (len3(n) < 0.5f) {
        return;
    }
    const std::vector<InstanceId> ids = selection();
    const int side = clipSide_;
    beginStep("Clip Brushes");
    std::vector<InstanceId> made;
    engine_.on_simulation([&](DataModel& world) {
        for (InstanceId id : ids) {
            Brush* brush = brush_of(world, id);
            if (brush == nullptr) {
                continue;
            }
            const Matrix4 inverse = engine_core::matrix4_inverse(brush->transform());
            const geo::DVec3 la = to_d(engine_core::matrix4_point(inverse, a));
            const geo::DVec3 lb = to_d(engine_core::matrix4_point(inverse, b));
            const geo::DVec3 lc = to_d(engine_core::matrix4_point(inverse, c));
            geo::Face back = geo::face_through(la, lb, lc);
            geo::Face front = geo::face_through(la, lc, lb);
            const std::vector<geo::Face> faces = brush->faces();
            geo::Built keepBack = geo::clip(faces, back);
            geo::Built keepFront = geo::clip(faces, front);
            if (side == 2 && keepBack.ok() && keepFront.ok()) {
                Brush& other = world.create<Brush>();
                world.set_name(other.id(), world.name(id));
                other.set_transform(brush->transform());
                other.apply(std::move(keepFront));
                world.set_parent(other.id(), world.parent(id));
                made.push_back(other.id());
                brush->apply(std::move(keepBack));
            } else if (side == 1 ? keepFront.ok() : keepBack.ok()) {
                brush->apply(std::move(side == 1 ? keepFront : keepBack));
            }
        }
    });
    endStep(true);
    clipPoints_.clear();
    if (!made.empty()) {
        std::vector<InstanceId> all = ids;
        all.insert(all.end(), made.begin(), made.end());
        select(std::move(all));
    }
}

void BrushTool::applyLoop(const DraggerRay& ray, BrushModifiers mods) {
    const std::optional<Hit> hit = pick(ray, true);
    if (!hit) {
        return;
    }
    const double grid = grid_;
    std::vector<InstanceId> halves;
    beginStep("Loop Cut");
    engine_.on_simulation([&](DataModel& world) {
        Brush* brush = brush_of(world, hit->instance);
        if (brush == nullptr) {
            return;
        }
        const auto cut = loop_of(*brush, hit->face, hit->position, grid, mods.shift);
        if (!cut) {
            return;
        }
        auto parts = geo::split(brush->faces(), cut->plane);
        if (!parts) {
            return;
        }
        // The far half is a new brush like this one.
        Brush& other = world.create<Brush>();
        world.set_name(other.id(), world.name(brush->id()));
        other.set_transform(brush->transform());
        other.apply(std::move(parts->second));
        other.set_anchored(brush->anchored());
        other.set_color(brush->color());
        other.set_transparency(brush->transparency());
        other.set_can_collide(brush->can_collide());
        other.set_friction(brush->friction());
        other.set_bounciness(brush->bounciness());
        world.set_parent(other.id(), world.parent(brush->id()));
        brush->apply(std::move(parts->first));
        halves = {brush->id(), other.id()};
    });
    endStep(!halves.empty());
    if (!halves.empty()) {
        select(std::move(halves));
    }
}

void BrushTool::duplicate() {
    const std::vector<InstanceId> ids = selection();
    std::vector<InstanceId> copies;
    engine_.on_simulation([&](DataModel& world) {
        for (InstanceId id : ids) {
            const Brush* brush = brush_of(world, id);
            if (brush == nullptr) {
                continue;
            }
            Brush& copy = world.create<Brush>();
            world.set_name(copy.id(), world.name(id));
            copy.set_transform(brush->transform());
            copy.set_faces(brush->faces());
            copy.set_anchored(brush->anchored());
            copy.set_color(brush->color());
            copy.set_transparency(brush->transparency());
            copy.set_can_collide(brush->can_collide());
            copy.set_friction(brush->friction());
            copy.set_bounciness(brush->bounciness());
            world.set_parent(copy.id(), world.parent(id));
            copies.push_back(copy.id());
        }
    });
    if (!copies.empty()) {
        select(std::move(copies));
    }
}

void BrushTool::nudge(Vec3 delta, bool copy) {
    if (selection().empty()) {
        return;
    }
    beginStep(copy ? "Copy Brushes" : "Nudge Brushes");
    if (copy) {
        duplicate();
    }
    const std::vector<InstanceId> ids = selection();
    engine_.on_simulation([&](DataModel& world) {
        for (InstanceId id : ids) {
            if (Brush* brush = brush_of(world, id)) {
                Matrix4 moved = brush->transform();
                moved.m[12] += delta.x;
                moved.m[13] += delta.y;
                moved.m[14] += delta.z;
                brush->set_transform(moved);
            }
        }
    });
    endStep(true);
}

void BrushTool::turn(int axis, int quarterTurns) {
    const std::vector<InstanceId> ids = selection();
    if (ids.empty()) {
        return;
    }
    beginStep("Rotate Brushes");
    engine_.on_simulation([&](DataModel& world) {
        // About the selection's middle, snapped so the result stays on the grid.
        Vec3 lo{1e30f, 1e30f, 1e30f};
        Vec3 hi{-1e30f, -1e30f, -1e30f};
        for (InstanceId id : ids) {
            if (const Brush* brush = brush_of(world, id)) {
                const Matrix4 m = brush->transform();
                for (const geo::DVec3& v : brush->shape().vertices) {
                    const Vec3 p = engine_core::matrix4_point(m, to_f(v));
                    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                }
            }
        }
        const Vec3 pivot = snapVec(mul(add(lo, hi), 0.5f));
        const int q = ((quarterTurns % 4) + 4) % 4;
        const double c = q == 0 ? 1 : q == 2 ? -1 : 0;
        const double s = q == 1 ? 1 : q == 3 ? -1 : 0;
        double rows[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const int a = (axis + 1) % 3;
        const int b = (axis + 2) % 3;
        rows[a][a] = c;
        rows[a][b] = -s;
        rows[b][a] = s;
        rows[b][b] = c;
        for (InstanceId id : ids) {
            Brush* brush = brush_of(world, id);
            if (brush == nullptr) {
                continue;
            }
            // Turn the brush's world-space faces, then keep its Transform a translation.
            const Matrix4 m = brush->transform();
            const Vec3 origin{m.m[12], m.m[13], m.m[14]};
            double matrix[12];
            linear_matrix(rows, to_d(sub(pivot, origin)), matrix);
            geo::Built built = geo::transform(brush->faces(), matrix);
            if (built.ok()) {
                brush->apply(std::move(built));
            }
        }
    });
    endStep(true);
}

void BrushTool::flip(int axis) {
    const std::vector<InstanceId> ids = selection();
    if (ids.empty()) {
        return;
    }
    beginStep("Flip Brushes");
    engine_.on_simulation([&](DataModel& world) {
        double rows[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        rows[axis][axis] = -1;
        for (InstanceId id : ids) {
            if (Brush* brush = brush_of(world, id)) {
                double matrix[12];
                const geo::DVec3 middle = (brush->shape().min + brush->shape().max) * 0.5;
                linear_matrix(rows, middle, matrix);
                geo::Built built = geo::transform(brush->faces(), matrix);
                if (built.ok()) {
                    brush->apply(std::move(built));
                }
            }
        }
    });
    endStep(true);
}

void BrushTool::deleteSelection() {
    const std::vector<InstanceId> ids = selection();
    if (ids.empty()) {
        return;
    }
    beginStep("Delete Brushes");
    engine_.on_simulation([&](DataModel& world) {
        for (InstanceId id : ids) {
            if (world.alive(id)) {
                world.destroy(id);
            }
        }
    });
    endStep(true);
}

void BrushTool::nextMaterial() {
    engine_.on_simulation([&](DataModel& world) {
        materials_.clear();
        find_materials(world, world.scene_service("Assets"), materials_);
        // Default first, then each Material.
        material_ = (material_ + 1) % (materials_.size() + 1);
        if (material_ == 0) {
            materialName_ = "Default";
            materialGuid_.clear();
        } else {
            const InstanceId id = materials_[material_ - 1];
            materialName_ = world.name(id);
            materialGuid_ = world.guid(id);
        }
    });
}

void BrushTool::applyMaterialToSelection() {
    const std::vector<InstanceId> ids = selection();
    if (ids.empty()) {
        return;
    }
    const std::string material = materialGuid_;
    beginStep("Paint Brushes");
    engine_.on_simulation([&](DataModel& world) {
        for (InstanceId id : ids) {
            if (Brush* brush = brush_of(world, id)) {
                std::vector<geo::Face> faces = brush->faces();
                for (geo::Face& face : faces) {
                    face.material = material;
                }
                brush->set_faces(std::move(faces));
            }
        }
    });
    endStep(true);
}

void BrushTool::reshapeSelection(int kind, int sides) {
    const std::vector<InstanceId> ids = selection();
    if (ids.empty()) {
        return;
    }
    beginStep("Reshape Brushes");
    engine_.on_simulation([&](DataModel& world) {
        for (InstanceId id : ids) {
            Brush* brush = brush_of(world, id);
            if (brush == nullptr) {
                continue;
            }
            const geo::DVec3 lo = brush->shape().min;
            const geo::DVec3 hi = brush->shape().max;
            const geo::DVec3 size = hi - lo;
            const geo::DVec3 middle = (lo + hi) * 0.5;
            std::vector<geo::Face> faces = kind == 1   ? geo::make_cylinder(size, sides)
                                           : kind == 2 ? geo::make_cone(size, sides)
                                           : kind == 3 ? geo::make_sphere(size, sides)
                                                       : geo::make_box(size);
            const std::string material = brush->faces().empty() ? std::string() : brush->faces().front().material;
            const double move[12] = {1, 0, 0, middle.x, 0, 1, 0, middle.y, 0, 0, 1, middle.z};
            geo::Built built = geo::transform(faces, move);
            if (!built.ok()) {
                continue;
            }
            for (geo::Face& face : built.faces) {
                face.material = material;
            }
            brush->apply(std::move(built));
        }
    });
    endStep(true);
}

void BrushTool::refreshVertices() {
    corners_.clear();
    const std::vector<InstanceId> ids = selection();
    engine_.on_simulation([&](DataModel& world) {
        for (InstanceId id : ids) {
            if (const Brush* brush = brush_of(world, id)) {
                const Matrix4 m = brush->transform();
                for (const geo::DVec3& v : brush->shape().vertices) {
                    corners_.push_back(Corner{id, engine_core::matrix4_point(m, to_f(v))});
                }
            }
        }
    });
}

// --- Keys ----------------------------------------------------------------

bool BrushTool::key(const jadefx::KeyEvent& event) {
    if (!active_ || !event.pressed || event.meta) {
        return false;
    }
    const int key = event.key;
    const bool ctrl = event.control;
    const bool alt = event.alt;
    const bool shift = event.shift;
    const auto done = [this] {
        if (ray_) {
            refreshHover(*ray_, mods_);
        }
        return true;
    };
    if (key == jadefx::Key::Escape && !shift) {
        if (drag_ == Drag::Height || drag_ == Drag::Drawing) {
            drag_ = Drag::None;
        } else if (drag_ != Drag::None) {
            // Undo what the drag did so far.
            drag_ = Drag::None;
            endStep(false);
            held_.clear();
        } else if (!clipPoints_.empty()) {
            clipPoints_.clear();
        } else if (mode_ != BrushMode::Draw) {
            setMode(BrushMode::Draw);
        } else {
            select({});
        }
        return done();
    }
    if (event.repeat && key != jadefx::Key::Left && key != jadefx::Key::Right && key != jadefx::Key::Up &&
        key != jadefx::Key::Down && key != jadefx::Key::PageUp && key != jadefx::Key::PageDown) {
        return false;
    }
    if ((key == jadefx::Key::Enter || key == jadefx::Key::KpEnter)) {
        if (drag_ == Drag::Height) {
            commitDraw();
            return done();
        }
        if (mode_ == BrushMode::Clip) {
            if (ctrl) {
                clipSide_ = (clipSide_ + 1) % 3;
            } else {
                applyClip();
            }
            return done();
        }
        return false;
    }
    if (mode_ == BrushMode::Clip && key == jadefx::Key::Backspace && !clipPoints_.empty()) {
        clipPoints_.pop_back();
        return done();
    }
    if (!ctrl && !alt) {
        if (key == jadefx::Key::LeftBracket || key == jadefx::Key::Minus) {
            stepGrid(-1);
            return done();
        }
        if (key == jadefx::Key::RightBracket || key == jadefx::Key::Equal) {
            stepGrid(1);
            return done();
        }
        if (key >= jadefx::Key::Digit0 + 1 && key <= jadefx::Key::Digit0 + 9 && !shift) {
            setGrid(std::pow(2.0, key - (jadefx::Key::Digit0 + 1)));
            return done();
        }
        if (key == jadefx::Key::V && !shift) {
            setMode(mode_ == BrushMode::Vertex ? BrushMode::Draw : BrushMode::Vertex);
            return done();
        }
        if (key == jadefx::Key::C && !shift) {
            setMode(mode_ == BrushMode::Clip ? BrushMode::Draw : BrushMode::Clip);
            return done();
        }
        if (key == jadefx::Key::K && !shift) {
            setMode(mode_ == BrushMode::Loop ? BrushMode::Draw : BrushMode::Loop);
            return done();
        }
        if (key == jadefx::Key::Delete || key == jadefx::Key::Backspace) {
            deleteSelection();
            return done();
        }
    }
    if (ctrl && !alt && key == jadefx::Key::D) {
        beginStep("Duplicate Brushes");
        duplicate();
        endStep(true);
        int sign = 1;
        const int axis = cameraAxis(true, sign);
        nudge(axis_vec(axis, static_cast<float>(grid_ * sign)), false);
        return done();
    }
    if (ctrl && !alt && key == jadefx::Key::F) {
        int sign = 1;
        flip(shift ? 1 : cameraAxis(true, sign));
        return done();
    }
    // Arrows: along the ground, as the camera sees it; PgUp/PgDn: up and down.
    const bool arrow = key == jadefx::Key::Left || key == jadefx::Key::Right || key == jadefx::Key::Up ||
                       key == jadefx::Key::Down;
    const bool page = key == jadefx::Key::PageUp || key == jadefx::Key::PageDown;
    if (arrow || page) {
        if (selection().empty()) {
            return false;
        }
        int rightSign = 1;
        int forwardSign = 1;
        const int right = cameraAxis(true, rightSign);
        const int forward = cameraAxis(false, forwardSign);
        if (alt) {
            // Turn 90 degrees: Left/Right about up, Up/Down about the camera's right, PgUp/PgDn about forward.
            if (key == jadefx::Key::Left || key == jadefx::Key::Right) {
                turn(1, key == jadefx::Key::Left ? 1 : -1);
            } else if (arrow) {
                turn(right, (key == jadefx::Key::Up ? 1 : -1) * rightSign);
            } else {
                turn(forward, (key == jadefx::Key::PageUp ? 1 : -1) * forwardSign);
            }
            return done();
        }
        const float step = static_cast<float>(grid_);
        Vec3 delta{};
        if (key == jadefx::Key::Left || key == jadefx::Key::Right) {
            delta = axis_vec(right, step * rightSign * (key == jadefx::Key::Right ? 1.f : -1.f));
        } else if (arrow) {
            delta = axis_vec(forward, step * forwardSign * (key == jadefx::Key::Up ? 1.f : -1.f));
        } else {
            delta = Vec3{0.f, key == jadefx::Key::PageUp ? step : -step, 0.f};
        }
        nudge(delta, ctrl);
        return done();
    }
    return false;
}

// --- Drawing the tool ----------------------------------------------------

void BrushTool::refreshHover(const DraggerRay& ray, BrushModifiers mods) {
    lines_.clear();
    readout_.clear();
    hover_.reset();
    hoverCorner_ = -1;
    const auto label = [&](Vec3 at, std::string text) {
        Vec2 p{};
        if (engine_core::project_point(view_, at, p)) {
            readoutAt_ = p;
            readout_ = std::move(text);
        }
    };
    const auto grid_patch = [&](Vec3 center, int axis) {
        // A fading patch of grid around center on the axis plane.
        const int u = (axis + 1) % 3;
        const int v = (axis + 2) % 3;
        constexpr int kCells = 8;
        const float g = static_cast<float>(grid_);
        Vec3 c = snapVec(center);
        axis_of(c, axis) = axis_of(center, axis);
        for (int i = -kCells; i <= kCells; ++i) {
            const float fade = 1.f - std::fabs(static_cast<float>(i)) / (kCells + 1);
            Rgba color = kGridLine;
            color.a *= fade;
            Vec3 a = c, b = c;
            axis_of(a, u) += i * g;
            axis_of(b, u) += i * g;
            axis_of(a, v) -= kCells * g;
            axis_of(b, v) += kCells * g;
            segment(lines_, a, b, color);
            a = c;
            b = c;
            axis_of(a, v) += i * g;
            axis_of(b, v) += i * g;
            axis_of(a, u) -= kCells * g;
            axis_of(b, u) += kCells * g;
            segment(lines_, a, b, color);
        }
    };

    if (drag_ == Drag::Drawing || drag_ == Drag::Height) {
        const float h = drag_ == Drag::Height ? static_cast<float>(planeSign_ * height_) : 0.f;
        const Vec3 far = add(drawEnd_, axis_vec(planeAxis_, h));
        const Vec3 lo{std::min({drawStart_.x, drawEnd_.x, far.x}), std::min({drawStart_.y, drawEnd_.y, far.y}),
                      std::min({drawStart_.z, drawEnd_.z, far.z})};
        const Vec3 hi{std::max({drawStart_.x, drawEnd_.x, far.x}), std::max({drawStart_.y, drawEnd_.y, far.y}),
                      std::max({drawStart_.z, drawEnd_.z, far.z})};
        grid_patch(drawEnd_, planeAxis_);
        box_lines(lines_, lo, hi, kDraw);
        const Vec3 size = sub(hi, lo);
        if (drag_ == Drag::Height) {
            label(hi, number(size.x) + " x " + number(size.y) + " x " + number(size.z) + "   click to place");
        } else {
            // Only the footprint so far: its two sides on the plane.
            const int u = (planeAxis_ + 1) % 3;
            const int v = (planeAxis_ + 2) % 3;
            label(hi, number(axis_of(size, u)) + " x " + number(axis_of(size, v)));
        }
        return;
    }

    // Picked out here: pick takes the simulation itself, which cannot be taken twice.
    const std::optional<Hit> clipHover = mode_ == BrushMode::Clip ? pick(ray, false) : std::nullopt;
    const std::optional<Hit> loopHover = mode_ == BrushMode::Loop ? pick(ray, true) : std::nullopt;
    engine_.on_simulation([&](DataModel& world) {
        if (mode_ == BrushMode::Loop) {
            const Brush* brush = loopHover ? brush_of(world, loopHover->instance) : nullptr;
            if (brush == nullptr) {
                return;
            }
            brush_lines(lines_, *brush, kKeep);
            const auto cut = loop_of(*brush, loopHover->face, loopHover->position, grid_, mods.shift);
            const auto parts = cut ? geo::split(brush->faces(), cut->plane) : std::nullopt;
            if (!parts) {
                return;
            }
            const Matrix4 m = brush->transform();
            const Vec3 from = engine_core::matrix4_point(m, to_f(cut->from));
            const Vec3 to = engine_core::matrix4_point(m, to_f(cut->to));
            segment(lines_, from, to, kFace);
            // The new face: the back half's polygon on the cut plane.
            const geo::Shape& shape = parts->first.shape;
            Vec3 at{};
            for (std::size_t i = 0; i < shape.polygons.size(); ++i) {
                if (geo::dot(shape.planes[i].normal, cut->plane.normal) < 0.9999 ||
                    std::fabs(shape.planes[i].distance - cut->plane.distance) > 1e-6) {
                    continue;
                }
                const auto& loop = shape.polygons[i].vertices;
                for (std::size_t k = 0; k < loop.size(); ++k) {
                    const Vec3 a = engine_core::matrix4_point(m, to_f(shape.vertices[loop[k]]));
                    const Vec3 b = engine_core::matrix4_point(m, to_f(shape.vertices[loop[(k + 1) % loop.size()]]));
                    segment(lines_, a, b, kClipPoint);
                }
            }
            const double t = cut->plane.distance - geo::dot(cut->plane.normal, cut->from);
            const double len = geo::length(cut->to - cut->from);
            at = add(from, mul(unit3(sub(to, from)), static_cast<float>(t)));
            cross_lines(lines_, at, engine_core::handle_scale(view_, at) * 5.f, kClipPoint);
            label(at, number(t) + " | " + number(len - t));
            return;
        }
        // The selection's faces and corners, and what the pointer is over.
        std::vector<InstanceId> ids;
        for (InstanceId id : world.selection().get()) {
            if (brush_of(world, id) != nullptr) {
                ids.push_back(id);
            }
        }
        if (mode_ == BrushMode::Vertex) {
            float best = kCornerPixels;
            Vec2 pointer{};
            engine_core::project_point(view_, add(ray.origin, mul(ray.direction, 10.f)), pointer);
            for (std::size_t i = 0; i < corners_.size(); ++i) {
                Vec2 at{};
                if (engine_core::project_point(view_, corners_[i].world, at)) {
                    const float d = std::hypot(at.x - pointer.x, at.y - pointer.y);
                    if (d < best && drag_ == Drag::None) {
                        best = d;
                        hoverCorner_ = static_cast<int>(i);
                    }
                }
            }
            for (std::size_t i = 0; i < corners_.size(); ++i) {
                const float size = engine_core::handle_scale(view_, corners_[i].world) * 7.f;
                const bool hot = static_cast<int>(i) == hoverCorner_ || static_cast<int>(i) == dragCorner_;
                cross_lines(lines_, corners_[i].world, size, hot ? (dragInvalid_ ? kInvalid : kCornerHot) : kCorner);
            }
            if (drag_ == Drag::Vertex) {
                label(dragDelta_, number(dragDelta_.x) + ", " + number(dragDelta_.y) + ", " + number(dragDelta_.z) +
                                      (dragInvalid_ ? "   not convex" : ""));
            }
            return;
        }
        if (mode_ == BrushMode::Clip) {
            for (const Vec3& p : clipPoints_) {
                cross_lines(lines_, p, engine_core::handle_scale(view_, p) * 6.f, kClipPoint);
            }
            for (std::size_t i = 1; i < clipPoints_.size(); ++i) {
                segment(lines_, clipPoints_[i - 1], clipPoints_[i], kClipPoint);
            }
            if (clipPoints_.size() >= 2) {
                const Vec3 a = clipPoints_[0];
                const Vec3 b = clipPoints_[1];
                const Vec3 c = clipPoints_.size() >= 3 ? clipPoints_[2] : add(a, clipNormal_);
                for (InstanceId id : ids) {
                    const Brush* brush = brush_of(world, id);
                    const Matrix4 m = brush->transform();
                    const Matrix4 inverse = engine_core::matrix4_inverse(m);
                    const geo::DVec3 la = to_d(engine_core::matrix4_point(inverse, a));
                    const geo::DVec3 lb = to_d(engine_core::matrix4_point(inverse, b));
                    const geo::DVec3 lc = to_d(engine_core::matrix4_point(inverse, c));
                    const auto plane = geo::plane_of(la, lb, lc);
                    if (!plane) {
                        continue;
                    }
                    const geo::Built back = geo::clip(brush->faces(), geo::face_through(la, lb, lc));
                    const geo::Built front = geo::clip(brush->faces(), geo::face_through(la, lc, lb));
                    const auto draw = [&](const geo::Built& part, Rgba color) {
                        if (!part.ok()) {
                            return;
                        }
                        for (const auto& [p, q] : part.shape.edges) {
                            segment(lines_, engine_core::matrix4_point(m, to_f(part.shape.vertices[p])),
                                    engine_core::matrix4_point(m, to_f(part.shape.vertices[q])), color);
                        }
                    };
                    draw(back, clipSide_ == 1 ? kInvalid : kKeep);
                    draw(front, clipSide_ == 0 ? kInvalid : kKeep);
                }
            }
            if (const auto& hit = clipHover) {
                cross_lines(lines_, snapVec(hit->position), engine_core::handle_scale(view_, hit->position) * 4.f,
                            Rgba{kClipPoint.r, kClipPoint.g, kClipPoint.b, 0.5f});
            }
            return;
        }
        if (drag_ == Drag::Moving) {
            segment(lines_, dragOrigin_, add(dragOrigin_, dragDelta_), kFace);
            label(add(dragOrigin_, dragDelta_),
                  "moved " + number(dragDelta_.x) + ", " + number(dragDelta_.y) + ", " + number(dragDelta_.z));
            return;
        }
        if (drag_ == Drag::Resizing) {
            for (const Held& held : held_) {
                if (const Brush* brush = brush_of(world, held.id)) {
                    face_lines(lines_, *brush, held.face, dragInvalid_ ? kInvalid : kFace);
                    const geo::DVec3 size = brush->shape().max - brush->shape().min;
                    label(add(dragOrigin_, mul(dragNormal_, static_cast<float>(dragAmount_))),
                          (dragAmount_ >= 0 ? "+" : "") + number(dragAmount_) + "   " + number(size.x) + " x " +
                              number(size.y) + " x " + number(size.z));
                }
            }
            if (pressMods_.control) {
                // The extruded slab, as it would be made.
                const Vec3 offset = mul(dragNormal_, static_cast<float>(dragAmount_));
                for (const Held& held : held_) {
                    if (const Brush* brush = brush_of(world, held.id); brush != nullptr && held.face >= 0) {
                        const Matrix4 m = brush->transform();
                        const auto& loop = brush->shape().polygons[static_cast<std::size_t>(held.face)].vertices;
                        for (std::size_t i = 0; i < loop.size(); ++i) {
                            const Vec3 a = engine_core::matrix4_point(m, to_f(brush->shape().vertices[loop[i]]));
                            const Vec3 b = engine_core::matrix4_point(
                                m, to_f(brush->shape().vertices[loop[(i + 1) % loop.size()]]));
                            segment(lines_, add(a, offset), add(b, offset), kDraw);
                            segment(lines_, a, add(a, offset), kDraw);
                        }
                    }
                }
            }
            return;
        }
    });
    if (mode_ != BrushMode::Draw || drag_ != Drag::None) {
        return;
    }
    // Idle in Draw: show what a press would do.
    hover_ = pick(ray, false);
    engine_.on_simulation([&](DataModel& world) {
        if (!hover_ || !hover_->brush) {
            if (const auto at = ray_plane(ray, Vec3{}, Vec3{0.f, 1.f, 0.f}); at && !hover_) {
                cross_lines(lines_, snapVec(*at), engine_core::handle_scale(view_, *at) * 5.f, kGridLine);
            }
            return;
        }
        const Brush* brush = brush_of(world, hover_->instance);
        bool isSelected = false;
        for (InstanceId id : world.selection().get()) {
            isSelected = isSelected || id == hover_->instance;
        }
        if (isSelected && mods.shift) {
            face_lines(lines_, *brush, hover_->face, kFace);
        } else if (!isSelected) {
            brush_lines(lines_, *brush, kHover);
        }
    });
}

void BrushTool::appendLines(std::vector<float>& out) const {
    if (!active_) {
        return;
    }
    out.insert(out.end(), lines_.begin(), lines_.end());
}

}  // namespace runner
