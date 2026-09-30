#pragma once

#include "DataModel.hpp"
#include "InstanceRef.hpp"

#include <array>
#include <cstddef>
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

// One reference property: its name and the class it holds.
struct ReferenceSpec {
    const char* property;
    const char* klass;
};

// An asset whose saved properties are references to other assets, held by
// GUID (InstanceRef). Its subclass lists them once, in reference_specs.
class ReferenceAsset : public DataModel {
public:
    static constexpr std::size_t kMaxReferences = 4;

    ReferenceAsset(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    // The GUID in text; the live target, if any, in id, with kind Instance, else Nil.
    LuaSlot reference(std::size_t index) const;
    // SimulationThread. nil clears. A live instance of the property's class is
    // stored by GUID; one of another class is refused. A slot naming a GUID
    // (a load, Stop, or undo) is stored as it is.
    std::optional<std::string> set_reference(std::size_t index, const LuaSlot& value);

protected:
    virtual const ReferenceSpec* reference_specs(std::size_t& count) const = 0;
    void on_reuse() override;

private:
    std::array<InstanceRef, kMaxReferences> refs_;
};

// A PBR material: DiffuseTexture, NormalTexture, RoughnessTexture, and
// MetalnessTexture, each a Texture or nil.
class Material : public ReferenceAsset {
public:
    using ReferenceAsset::ReferenceAsset;
    const char* class_name() const override;

protected:
    const ReferenceSpec* reference_specs(std::size_t& count) const override;
};

// Joins a Mesh and a Material. Lives only in a Prefab.
class Model : public ReferenceAsset {
public:
    using ReferenceAsset::ReferenceAsset;
    const char* class_name() const override;

protected:
    const ReferenceSpec* reference_specs(std::size_t& count) const override;
};

// A template made of Models, its only children.
class Prefab : public DataModel {
public:
    Prefab(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override;
};

}  // namespace engine_core
