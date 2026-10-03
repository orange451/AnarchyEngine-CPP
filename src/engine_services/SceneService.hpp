#pragma once

#include "DataModel.hpp"

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

bool is_scene_service_class(std::string_view class_name);
// Each service's GUID is its class name in lowercase, the same in every
// place, so a place whose files lack one gets the same service on every read.
std::string scene_service_guid(std::string_view class_name);

// Instances here render in the game. Anything may go in it.
// CurrentCamera is the Camera the studio's scene view last used: set when you
// press in a view or pick its camera, and, while none is set, when a view
// links to a Camera, as it does once a place loads. It is session state: never
// saved, never an undo step, 0 once that Camera is gone, and cleared when the
// place is rebuilt. A change fires Changed, so Properties shows it at once. A
// script may set it too; the views do not follow a script's write.
class Workspace : public SceneService {
public:
    using SceneService::SceneService;
    const char* class_name() const override;

    InstanceId current_camera() const;
    // 0 clears it. False, changing nothing, when id is a live instance that is
    // not a Camera. A new value fires Changed with "CurrentCamera".
    bool set_current_camera(InstanceId id);

private:
    InstanceId current_camera_ = 0;
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
