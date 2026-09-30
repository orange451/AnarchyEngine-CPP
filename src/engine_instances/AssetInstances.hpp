#pragma once

#include "DataModel.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace engine_core {

// Assets, kept under game.Assets. Each lives only under its own category, as
// Containment's rules say: a Texture under Textures, a Model only in a Prefab.
// class_name is defined in AssetInstances.cpp so that file, and its Lua
// registration, stays linked.

// Why a Path is refused, or empty. A Path is relative to the resources folder,
// with '/' between names: not absolute, no drive letter, no '\', and no "..".
std::optional<std::string> resource_path_error(std::string_view path);

// An asset that names a file under the project's resources folder, as Path.
// Nothing checks that the file exists; nothing loads resources yet.
class FileAsset : public DataModel {
public:
    FileAsset(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    const std::string& path() const { return path_; }
    // SimulationThread. Returns why a path is refused, changing nothing.
    std::optional<std::string> set_path(std::string path);

protected:
    void on_reuse() override { path_.clear(); }

private:
    std::string path_;
};

class Texture : public FileAsset {
public:
    using FileAsset::FileAsset;
    const char* class_name() const override;
};

class Mesh : public FileAsset {
public:
    using FileAsset::FileAsset;
    const char* class_name() const override;
};

class Sound : public FileAsset {
public:
    using FileAsset::FileAsset;
    const char* class_name() const override;
};

// An asset whose properties point at other assets.
class ReferenceAsset : public DataModel {
public:
    ReferenceAsset(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
};

// A PBR material.
class Material : public ReferenceAsset {
public:
    using ReferenceAsset::ReferenceAsset;
    const char* class_name() const override;
};

// Joins a Mesh and a Material. Lives only in a Prefab.
class Model : public ReferenceAsset {
public:
    using ReferenceAsset::ReferenceAsset;
    const char* class_name() const override;
};

// A template made of Models, its only children.
class Prefab : public DataModel {
public:
    Prefab(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override;
};

}  // namespace engine_core
