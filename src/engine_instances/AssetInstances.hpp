#pragma once

#include "DataModel.hpp"

namespace engine_core {

// Assets, kept under game.Assets. Each lives only under its own category, as
// Containment's rules say: a Texture under Textures, a Model only in a Prefab.
// class_name is defined in AssetInstances.cpp so that file, and its Lua
// registration, stays linked.

// An asset that names a file under the project's resources folder.
class FileAsset : public DataModel {
public:
    FileAsset(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
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
