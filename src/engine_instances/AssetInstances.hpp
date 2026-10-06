#pragma once

#include "DataModel.hpp"
#include "InstanceRef.hpp"
#include "amesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anarchy::amesh {
struct Data;
}

namespace engine_core {

// Assets, kept under game.Assets. Each lives only under its own category, as
// Containment's rules say: a Texture under Textures, a Model only in a Prefab.
// class_name is defined in AssetInstances.cpp so that file, and its Lua
// registration, stays linked.

// Why a Path is refused, or empty. A Path is relative to the resources folder,
// with '/' between names: not absolute, no drive letter, no '\', and no "..".
std::optional<std::string> resource_path_error(std::string_view path);

// An asset that names a file under the project's resources folder, as Path.
// Nothing checks that the file exists. The Scene View loads a Mesh's file as AMESH;
// nothing else loads resources yet.
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

    // What the Add methods and Clear do.
    //
    // Stopped: reads the AMESH file at Path, or starts empty when there is no
    // Path or no file yet, lets edit change it, and writes it back. A Mesh with
    // no Path gets meshes/<Name>.<guid>.amesh first. Needs an open project,
    // whose resources folder takes the file. Undo puts back a Path it set, not
    // the file.
    //
    // Playing: changes the session geometry instead, starting from the file
    // the first time. The file and Path are left alone, and Stop drops what
    // the session made, so the Mesh draws its file again.
    //
    // Never reads from a file that is not AMESH, or adds to one with LODs.
    // Returns why nothing changed.
    std::optional<std::string> edit_geometry(const std::function<void(anarchy::amesh::Data&)>& edit);

    // What this play session's edits made, shared with the renderer. data is
    // replaced whole by each edit and never changed after, so a frame can hold
    // it while the next edit runs. revision is unique across every Mesh.
    struct SessionGeometry {
        std::shared_ptr<const anarchy::amesh::Data> data;
        std::uint64_t revision = 0;
    };
    // Empty when this session made none: the Mesh draws its file.
    SessionGeometry session_geometry() const;

    // Every vertex position of what the Mesh draws now: this session's
    // geometry while playing, else its AMESH file. With triangles, also its
    // finest LOD's triangles, three indices into out each. Returns why there
    // are none.
    std::optional<std::string> vertex_positions(std::vector<Vec3>& out,
                                                std::vector<std::uint32_t>* triangles = nullptr) const;

    // The box around every vertex position of what the Mesh draws now, as
    // vertex_positions gives them. False, setting nothing, when it has none.
    // Measured again only when its session geometry changes, or its Path, the
    // resources folder, or the file's time on disk. Needs the DataModel lock;
    // a read lock is enough.
    bool bounds(Vec3& low, Vec3& high) const;
    // OriginOffset: from the Mesh's origin to the middle of bounds. (0, 0, 0)
    // when it has none.
    Vec3 origin_offset() const;

    // The convex pieces in the AMESH file, when it has some of recipe and this
    // session made no geometry. Read again only when file_stamp changes. Needs
    // the DataModel lock; a read lock is enough.
    bool file_pieces(std::uint32_t recipe, std::vector<anarchy::amesh::ConvexPiece>& out) const;

    // Reads the file, sets its pieces, and writes it back as edit_geometry
    // does, keeping its LODs. Refused while playing or with no file. Not an
    // undo step. Returns why nothing changed.
    std::optional<std::string> store_pieces(std::uint32_t recipe, std::vector<anarchy::amesh::ConvexPiece> pieces);

    // The file as it is now: Path, time on disk, and size, as one string.
    // Empty with no Path or no file.
    std::string file_stamp() const;

protected:
    void on_reuse() override;

private:
    // Reads the AMESH file at path under root into out. An empty path, or no
    // file, leaves out empty.
    std::optional<std::string> read_file(const std::filesystem::path& root, const std::string& path,
                                         anarchy::amesh::Data& out, bool allow_lods = false) const;

    // Writes data to path under root beside the file, then renames it over, so
    // a reader never sees half a mesh.
    static std::optional<std::string> write_file(const std::filesystem::path& root, const std::string& path,
                                                 const anarchy::amesh::Data& data);

    SessionGeometry session_;
    // world_generation() when session_ was made. Stop bumps it, and the copy lapses.
    std::uint32_t session_generation_ = 0;

    // bounds, as last measured, and what from: a session's revision, or else
    // the file and its time on disk. Readers share the DataModel lock, so the
    // cache has its own.
    mutable std::mutex bounds_mutex_;
    mutable bool bounds_measured_ = false;
    mutable std::uint64_t bounds_revision_ = 0;
    mutable std::filesystem::path bounds_file_;
    mutable std::filesystem::file_time_type bounds_stamp_{};
    mutable bool bounds_found_ = false;
    mutable Vec3 bounds_low_{};
    mutable Vec3 bounds_high_{};

    // file_pieces, as last read, and the file_stamp it was read at.
    mutable std::mutex pieces_mutex_;
    mutable std::string pieces_stamp_;
    mutable std::uint32_t pieces_recipe_ = 0;
    mutable std::vector<anarchy::amesh::ConvexPiece> pieces_;
};

class Sound : public FileAsset {
public:
    using FileAsset::FileAsset;
    const char* class_name() const override;

    // TimeLength: how long Path's file under root plays, in seconds; 0 with
    // no Path or root, or a file that is missing or does not decode. The file
    // is read again only when Path, root, or its time on disk changes. Any thread.
    double time_length(const std::filesystem::path& root) const;

private:
    mutable std::mutex length_mutex_;
    // The file and its time on disk when length_ was read.
    mutable std::filesystem::path length_file_;
    mutable std::filesystem::file_time_type length_stamp_{};
    mutable double length_ = 0;
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
    static constexpr std::size_t kMaxReferences = 5;

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

// A PBR material: DiffuseTexture, NormalTexture, RoughnessTexture,
// MetalnessTexture, and EmissiveTexture, each a Texture or nil; Color, a
// Color3 that tints the surface; Emissive, a Color3 of light the surface
// gives off itself; and Metalness, Roughness, Reflectivity, and Transparency,
// each 0 to 1 on its slider. Metalness, Roughness, and Emissive scale their
// textures. Those four take any
// finite number, as a Roblox Transparency does; whatever draws them reads
// them clamped to 0..1.
class Material : public ReferenceAsset {
public:
    // reference() indices.
    static constexpr std::size_t kDiffuseTextureReference = 0;
    static constexpr std::size_t kNormalTextureReference = 1;
    static constexpr std::size_t kRoughnessTextureReference = 2;
    static constexpr std::size_t kMetalnessTextureReference = 3;
    static constexpr std::size_t kEmissiveTextureReference = 4;

    static constexpr double kDefaultReflectivity = 0.5;
    static constexpr double kDefaultTransparency = 0.0;
    static constexpr double kDefaultMetalness = 0.0;
    static constexpr double kDefaultRoughness = 0.4;
    static constexpr ColorRgb kDefaultColor{1.f, 1.f, 1.f, 1.f};
    static constexpr ColorRgb kDefaultEmissive{0.f, 0.f, 0.f, 1.f};

    using ReferenceAsset::ReferenceAsset;
    const char* class_name() const override;

    double reflectivity() const { return reflectivity_; }
    double transparency() const { return transparency_; }
    double metalness() const { return metalness_; }
    double roughness() const { return roughness_; }
    ColorRgb color() const { return color_; }
    ColorRgb emissive() const { return emissive_; }
    // SimulationThread. A value that is not finite is refused: returns why and changes nothing.
    std::optional<std::string> set_reflectivity(double value);
    std::optional<std::string> set_transparency(double value);
    std::optional<std::string> set_metalness(double value);
    std::optional<std::string> set_roughness(double value);
    std::optional<std::string> set_color(ColorRgb color);
    std::optional<std::string> set_emissive(ColorRgb color);

protected:
    const ReferenceSpec* reference_specs(std::size_t& count) const override;
    void on_reuse() override;

private:
    std::optional<std::string> set_number(const char* property, double& slot, double value);
    std::optional<std::string> set_color3(const char* property, ColorRgb& slot, ColorRgb color);

    double reflectivity_ = kDefaultReflectivity;
    double transparency_ = kDefaultTransparency;
    double metalness_ = kDefaultMetalness;
    double roughness_ = kDefaultRoughness;
    ColorRgb color_ = kDefaultColor;
    ColorRgb emissive_ = kDefaultEmissive;
};

// Joins a Mesh and a Material. Lives only in a Prefab.
class Model : public ReferenceAsset {
public:
    // reference() indices.
    static constexpr std::size_t kMeshReference = 0;
    static constexpr std::size_t kMaterialReference = 1;

    using ReferenceAsset::ReferenceAsset;
    const char* class_name() const override;

protected:
    const ReferenceSpec* reference_specs(std::size_t& count) const override;
};

// A template made of Models, its only children. Edit, its primary action, opens it in a Prefab editor.
class Prefab : public DataModel {
public:
    Prefab(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override;
    void context_actions(std::vector<ContextAction>& out) const override;

    // The box around the bounds of every Model's Mesh (Mesh::bounds), as the
    // Prefab draws them all at its origin. False, setting nothing, when none
    // has any. Needs the DataModel lock; a read lock is enough.
    bool bounds(Vec3& low, Vec3& high) const;
    // OriginOffset: from the Prefab's origin to the middle of bounds, so it
    // moves as Models come and go or their Meshes change. (0, 0, 0) when it
    // has none.
    Vec3 origin_offset() const;
};

}  // namespace engine_core
