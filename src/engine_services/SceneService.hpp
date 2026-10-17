#pragma once

#include "DataModel.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace engine_core {

// A service: made with the world, under game or under another service, as
// Containment's kServices lists it. It lives as long as the world: it cannot
// be moved, renamed, or destroyed, and a script cannot make one.
class Service : public DataModel {
public:
    Service(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    bool is_service() const override { return true; }
    // Paste only: a service takes children, and cannot be cut, renamed, or deleted.
    void context_actions(std::vector<ContextAction>& out) const override;
};

// A child of game that scripts and the explorer see: Workspace, Lighting,
// Storage, Scripts, or Gui. A DataModel that is not an Instance, like Game.
class SceneService : public Service {
public:
    using Service::Service;

    bool is_scene_service() const override { return true; }
};

// The scene service classes, in the order game holds them.
inline constexpr const char* kSceneServiceClasses[] = {"Workspace", "Lighting", "Storage", "Scripts", "Gui"};

// Instances here render in the game. Anything may go in it.
// CurrentCamera is the Camera the studio's scene view last used: set when you
// press in a view or pick its camera, and, while none is set, when a view
// links to a Camera, as it does once a place loads. It is session state: never
// saved, never an undo step, 0 once that Camera is gone, and cleared when the
// place is rebuilt. A change fires Changed, so Properties shows it at once. A
// script may set it too; the views do not follow a script's write.
//
// Gravity is how fast PhysicsWorld speeds every body and PlayerController
// downward, in units per second per second: a saved registry property
// (lua_saved_property), so it is saved, undone, and restored at Stop. Below 0
// pulls up; 0 is none. A change during play takes effect on the next step.
//
// CollisionGroups is the place's collision groups (DataModel::collision_groups)
// as CollisionGroups::save_text writes them: a hidden saved string, so a save
// writes it, a load reads it, Stop restores it, and undo puts it back. The
// registry is its only record; a write rebuilds the registry
// (CollisionGroups::load_text), and edit_collision_groups notes every change.
class Workspace : public SceneService {
public:
    static constexpr double kDefaultGravity = 9.81;

    using SceneService::SceneService;
    const char* class_name() const override;

    InstanceId current_camera() const;
    // 0 clears it. False, changing nothing, when id is a live instance that is
    // not a Camera. A new value fires Changed with "CurrentCamera".
    bool set_current_camera(InstanceId id);

    double gravity() const { return gravity_; }
    // Runs on SimulationThread. A value that is not finite is refused: it
    // returns why and changes nothing. Otherwise empty.
    std::optional<std::string> set_gravity(double value);

    // The most particles every ParticleEmitter in Workspace may have alive at
    // once. Over it, the emitters farthest from the camera spawn fewer first;
    // live particles are never removed. Refuses a value that is not a whole
    // number, 0 or more.
    static constexpr int kDefaultParticleBudget = 20000;
    int particle_budget() const { return particle_budget_; }
    std::optional<std::string> set_particle_budget(double value);

    // Notes a change to the collision groups, as save_text gave them before
    // and after (CollisionGroupEdits.hpp calls it): undo, Changed, and an
    // unsaved place, as any saved property's setter does.
    void note_collision_groups(std::string before, std::string after);

private:
    InstanceId current_camera_ = 0;
    double gravity_ = kDefaultGravity;
    int particle_budget_ = kDefaultParticleBudget;
};

// Assets a place keeps to use at runtime. Nothing here renders or runs.
class Storage : public SceneService {
public:
    using SceneService::SceneService;
    const char* class_name() const override;
};

// Scripts that run at runtime. A Script runs only under Workspace, Scripts, or Gui.
class Scripts : public SceneService {
public:
    using SceneService::SceneService;
    const char* class_name() const override;
};

// The screen GUIs: every ScreenGui in it, directly or through Folders, is drawn
// over each Scene View, in edit mode and in play. Anything may go in it, and a
// Script in it runs, as in Workspace.
// Its class, and its name in scripts, is Gui (game.Gui).
class GuiService : public SceneService {
public:
    using SceneService::SceneService;
    const char* class_name() const override;
};

// Inside Storage: what never leaves the server, such as a networked game's
// secret logic, tools, and data. Nothing in it replicates to clients and no
// script in it runs. In single-player it is a place to keep things.
class ServerStorage : public Service {
public:
    using Service::Service;
    const char* class_name() const override;
};

// Inside Storage: what a networked game keeps for its clients, such as GUIs to
// clone, effects, and client modules. It replicates like the rest of the
// place; no script in it runs. In single-player it is a place to keep things.
class ClientStorage : public Service {
public:
    using Service::Service;
    const char* class_name() const override;
};

// The studio's own tools: plugins and what they make, such as the Move tool's
// Dragger. A child of game the explorer does not show and game scripts cannot
// reach. Nothing under it is saved or undone, and New, Open, Play, and Stop
// leave it as it is. Not a scene service: no play script runs under it.
class Core : public Service {
public:
    using Service::Service;
    const char* class_name() const override;
    bool hidden_in_explorer() const override { return true; }
};

}  // namespace engine_core
