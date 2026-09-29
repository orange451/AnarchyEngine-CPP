#pragma once

#include "DataModel.hpp"

#include <string>
#include <string_view>

namespace engine_core {

// A child of game that scripts and the explorer see: Workspace, Lighting,
// Storage, or Scripts. A DataModel that is not an Instance, like Game. Game
// makes one of each, and they live as long as the world: they cannot be
// moved, renamed, or destroyed, and a script cannot make one.
class SceneService : public DataModel {
public:
    SceneService(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    bool is_scene_service() const override { return true; }
    // Paste only: a service takes children, and cannot be cut, renamed, or deleted.
    void context_actions(std::vector<ContextAction>& out) const override;
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
