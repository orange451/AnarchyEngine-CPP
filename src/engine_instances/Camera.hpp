#pragma once

#include "SpatialObject.hpp"
#include "Vector2.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Where a Scene View looks from. It is a SpatialObject, not a GameObject: it
// draws nothing. The camera
// sits at the translation and looks down its -Z, with its +Y up.
// FieldOfView is the vertical angle it sees, in degrees. It is a saved
// registry property (lua_saved_property), so DataModel saves, loads, undoes,
// and restores it at Stop. A Camera in Workspace has a render snapshot row,
// which carries its Transform and FieldOfView.
// Its Transform is an ordinary property: a write inside a recording is an undo
// step. Flying the view writes it outside any recording, so that is not one,
// though a save writes where the camera is.
class Camera : public SpatialObject {
public:
    static constexpr double kDefaultFieldOfView = 70.0;
    // A new place's Camera, which sees what the Scene View drew before there were Cameras.
    static constexpr double kNewPlaceFieldOfView = 60.0;
    static constexpr double kMinFieldOfView = 1.0;
    static constexpr double kMaxFieldOfView = 120.0;

    using SpatialObject::SpatialObject;

    const char* class_name() const override { return "Camera"; }

    double field_of_view() const { return field_of_view_; }
    // SimulationThread. Clamped to kMinFieldOfView..kMaxFieldOfView. A value
    // that is not finite is refused: returns why and changes nothing.
    std::optional<std::string> set_field_of_view(double degrees);
    // The size in points of the scene view showing this camera, as the view
    // last reported it; (0, 0) before any has. Read-only to scripts, never
    // saved or undone. SimulationThread.
    Vec2 viewport_size() const { return viewport_size_; }
    void set_viewport_size(Vec2 size);

    bool load_property(const std::string& key, const JsonValue& value, std::string& error) override;

protected:
    void on_reuse() override;

private:
    double field_of_view_ = kDefaultFieldOfView;
    Vec2 viewport_size_{};
};

}  // namespace engine_core
