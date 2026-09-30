#pragma once

#include "GameObject.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Where a Scene View looks from. Its Transform is a GameObject's: the camera
// sits at the translation and looks down its -Z, with its +Y up.
// FieldOfView is the vertical angle it sees, in degrees. It is a saved
// registry property (lua_saved_property), so DataModel saves, loads, undoes,
// and restores it at Stop. A Camera in Workspace has a render snapshot row
// like any GameObject, and the row carries FieldOfView too.
// Its Transform writes are not undo steps (transform_in_history), though they save.
class Camera : public GameObject {
public:
    static constexpr double kDefaultFieldOfView = 70.0;
    // A new place's Camera, which sees what the Scene View drew before there were Cameras.
    static constexpr double kNewPlaceFieldOfView = 60.0;
    static constexpr double kMinFieldOfView = 1.0;
    static constexpr double kMaxFieldOfView = 120.0;

    using GameObject::GameObject;

    const char* class_name() const override { return "Camera"; }

    // Flying the camera is looking around, not editing.
    bool transform_in_history() const override { return false; }

    double field_of_view() const { return field_of_view_; }
    // SimulationThread. Clamped to kMinFieldOfView..kMaxFieldOfView. A value
    // that is not finite is refused: returns why and changes nothing.
    std::optional<std::string> set_field_of_view(double degrees);

protected:
    void on_reuse() override;

private:
    double field_of_view_ = kDefaultFieldOfView;
};

}  // namespace engine_core
