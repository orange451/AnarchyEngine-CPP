#pragma once

#include "SceneService.hpp"

namespace engine_core {

// A service the Game Explorer does not show: Assets and its five categories.
// The Assets pane is where they are browsed. Scripts reach them as any child,
// as game.Assets.Textures.
class GameService : public Service {
public:
    using Service::Service;

    bool hidden_in_explorer() const override { return true; }
};

// Holds the five asset categories and nothing else.
class Assets : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

// Each category holds its own asset class and Folders, at any depth.
class Materials : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

class Prefabs : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

class Meshes : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

class Textures : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

class Audio : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

class Animations : public GameService {
public:
    using GameService::GameService;
    const char* class_name() const override;
};

}  // namespace engine_core
