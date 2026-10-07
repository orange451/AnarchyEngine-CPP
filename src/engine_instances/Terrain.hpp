#pragma once

#include "PVInstance.hpp"
#include "terrain/VoxelVolume.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {

class TerrainMaterial;

// An island of voxels that scripts sculpt into any shape. Its Transform
// places the island; the voxels live in its VoxelVolume, in Terrain-local
// space. Its TerrainMaterial children say which Material each voxel Id
// draws and collides as.
//
// Transform   Matrix4  identity. Saved. Moves and turns the island; a
//                      Transform with scale or shear is refused.
// VoxelSize   number   read-only, always 1: the size of one cell in studs.
// CanCollide  boolean  true. Saved.
// DataPath    string   hidden, saved: the island's .avox file under resources.
class Terrain : public PVInstance {
public:
    Terrain(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override { return "Terrain"; }

    Matrix4 transform() const override { return transform_; }
    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_transform(const Matrix4& transform);
    std::optional<std::string> set_pv_transform(const Matrix4& transform) override { return set_transform(transform); }
    double voxel_size() const { return volume_.voxel_size(); }
    bool can_collide() const { return can_collide_; }
    std::optional<std::string> set_can_collide(bool value);
    const std::string& data_path() const { return data_path_; }
    // SimulationThread. Load, Stop, and undo write it; it is not an edit.
    void set_data_path(std::string path) { data_path_ = std::move(path); }
    // SimulationThread. What writing DataPath through its registry property
    // does (load, paste, Stop, undo). While read_place restores this Terrain
    // from place bytes it only stores the path: the bytes' token brings the
    // voxels. Otherwise a path another live Terrain holds is a paste: this
    // one takes that Terrain's voxels (shared chunks, nothing copied) and a
    // DataPath of its own at once, without an undo step. With history on, a
    // path no live Terrain holds but the stash knows (a cut source) is a
    // paste too, from the stash's latest voxels for it. Any other path is
    // stored.
    void load_data_path(std::string path);

    // The TerrainMaterial children, by Id. Those on Id 0 are left out.
    std::vector<TerrainMaterial*> materials() const;
    TerrainMaterial* material_by_id(int id) const;
    std::vector<TerrainMaterial*> materials_for(InstanceId material) const;
    // SimulationThread. Makes one on the lowest free Id, named after material
    // (or "TerrainMaterial"); material 0 leaves its Material nil.
    std::optional<std::string> add_material(InstanceId material, TerrainMaterial*& out);
    // The lowest Id no TerrainMaterial child holds; 0 when all 255 are taken.
    int free_id() const;

    // SimulationThread.
    terrain::VoxelVolume& volume() { return volume_; }
    const terrain::VoxelVolume& volume() const { return volume_; }

protected:
    void on_reuse() override;
    // SimulationThread. [u32 base length][DataModel's bytes][u64 TerrainStash
    // token]: the voxels travel as a token naming a stashed chunk map, never
    // as voxel data.
    void write_place(std::vector<std::byte>& out) const override;
    // An unknown or missing token leaves the voxels as they are, and DataPath
    // is then written as a load writes it.
    void read_place(const std::byte* data, std::size_t size) override;

private:
    Matrix4 transform_ = matrix4_identity();
    bool can_collide_ = true;
    std::string data_path_;
    terrain::VoxelVolume volume_;
    // The token of the last write_place while stopped: Play's capture names
    // the authored voxels, which a save during play writes (Task 7).
    mutable std::uint64_t authored_token_ = 0;
    // True while read_place loads the base properties with a known token.
    bool restoring_ = false;
};

}  // namespace engine_core
