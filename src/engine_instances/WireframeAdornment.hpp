#pragma once

#include "Color.hpp"
#include "DataModel.hpp"
#include "InstanceRef.hpp"
#include "Vector3.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {

// Colored lines drawn in the 3D view, as the studio's own tools draw theirs:
// over the scene, and faint where something nearer hides them. A tool keeps
// one in Core and redraws it with Clear and AddLine as the pointer moves.
// Drawn while in Workspace or Core.
//
// Adornee       PVInstance?  nil. Saved. The lines are in its space and follow it; nil is the world's.
// Color3        Color3       white. Saved. The color of a line added without its own.
// Transparency  number       0. Saved. 0 to 1, for every line.
// Visible       boolean      true. Saved.
//
// The lines themselves are not saved, undone, or restored at Stop: whoever
// added them draws them again.
class WireframeAdornment : public DataModel {
public:
    static constexpr std::size_t kMaxLines = 65536;

    struct Line {
        Vec3 from{};
        Vec3 to{};
        // Whether color is the line's own, or Color3 applies.
        bool own_color = false;
        ColorRgb color{};
    };

    WireframeAdornment(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);

    const char* class_name() const override;
    bool wireframe() const override { return true; }

    LuaSlot adornee() const;
    // Adornee when it is live and a PVInstance, else 0.
    InstanceId adornee_id() const;
    ColorRgb color() const { return color_; }
    double transparency() const { return transparency_; }
    bool visible() const { return visible_; }
    const std::vector<Line>& lines() const { return lines_; }
    // In Workspace or Core, and Visible.
    bool drawn() const;

    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_adornee(const LuaSlot& value);
    std::optional<std::string> set_color(ColorRgb color);
    std::optional<std::string> set_transparency(double value);
    std::optional<std::string> set_visible(bool visible);
    // Refused past kMaxLines, or for points that are not finite.
    std::optional<std::string> add_line(const Line& line);
    void clear_lines() { lines_.clear(); }

protected:
    void on_reuse() override;

private:
    InstanceRef adornee_ref_;
    ColorRgb color_{};
    double transparency_ = 0.0;
    bool visible_ = true;
    std::vector<Line> lines_;
};

}  // namespace engine_core
