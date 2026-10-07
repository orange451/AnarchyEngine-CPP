#pragma once

#include "PVInstance.hpp"
#include "terrain/VoxelVolume.hpp"

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
    bool terrain() const override { return true; }

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

private:
    Matrix4 transform_ = matrix4_identity();
    bool can_collide_ = true;
    std::string data_path_;
    terrain::VoxelVolume volume_;
};

}  // namespace engine_core
