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
// Storage, or Scripts. A DataModel that is not an Instance, like Game.
class SceneService : public Service {
public:
    using Service::Service;

    bool is_scene_service() const override { return true; }
};

// The scene service classes, in the order game holds them.
inline constexpr const char* kSceneServiceClasses[] = {"Workspace", "Lighting", "Storage", "Scripts"};

bool is_scene_service_class(std::string_view class_name);
// Each service's GUID is its class name in lowercase, the same in every
// place, so a place whose files lack one gets the same service on every read.
std::string scene_service_guid(std::string_view class_name);

// Instances here render in the game. Anything may go in it.
class Workspace : public SceneService {
public:
    using SceneService::SceneService;
    const char* class_name() const override;
};

// Assets a place keeps to use at runtime. Nothing here renders or runs.
class Storage : public SceneService {
public:
    using SceneService::SceneService;
    const char* class_name() const override;
};

// Scripts that run at runtime. A Script runs only under Workspace or Scripts.
class Scripts : public SceneService {
public:
    using SceneService::SceneService;
    const char* class_name() const override;
};

}  // namespace engine_core
