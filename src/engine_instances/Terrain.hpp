#pragma once

#include "PVInstance.hpp"
#include "terrain/VoxelVolume.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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
    // SimulationThread. What writing DataPath through its registry property
    // does (load, paste, Stop, undo). While read_place restores this Terrain
    // from place bytes it only stores the path: the bytes' token brings the
    // voxels. Otherwise a path another live Terrain holds is a paste: this
    // one takes that Terrain's voxels (shared chunks, nothing copied) and a
    // DataPath of its own at once, without an undo step. With history on, a
    // path no live Terrain holds but the stash knows (a cut source) is a
    // paste too, from the stash's latest voxels for it. Any other path names
    // this Terrain's .avox file, read from under resources_root(). A missing
    // or damaged file leaves the Terrain empty and says so through warn(); a
    // damaged one also gives the Terrain a new DataPath at once, so no save
    // ever writes over it.
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
    // SimulationThread. Every voxel edit goes through here, never through
    // volume() directly: runs edit, and when it succeeds while stopped marks
    // the place unsaved and, on this Terrain's first edit, gives it its
    // DataPath. When the open recording made this Terrain, its record takes
    // the edit too, so redo of that creation brings the voxels back. Returns
    // edit's refusal.
    std::optional<std::string> edit_volume(
        const std::function<std::optional<std::string>(terrain::VoxelVolume&)>& edit);

    // SimulationThread. Writes the authored voxels to DataPath under root when
    // they changed since the last save or load, or the file is missing.
    // Stopped, those are the live voxels; during play, the ones Play's
    // capture holds, never the runtime edits. Never assigns a DataPath.
    std::optional<std::string> save_resources(const std::filesystem::path& root) override;
    // SimulationThread. A save during play of a Terrain that play destroyed:
    // writes the voxels its captured place bytes name (name is for messages).
    // Bytes without a capture, or with no DataPath, write nothing.
    static std::optional<std::string> save_captured(const std::filesystem::path& root, const std::string& name,
                                                    const std::vector<std::byte>* captured);

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
    // terrain/<Name>.<guid>.avox, or with a numeric suffix when that is avoid.
    std::string own_data_path(const std::string& avoid) const;
    // Stores path and reads its file into the voxels (load_data_path).
    void read_data_file(std::string path);
    // edit_volume's: when the open recording created this Terrain, its
    // record takes the voxels and DataPath as they are now.
    void refresh_creation();

    Matrix4 transform_ = matrix4_identity();
    bool can_collide_ = true;
    std::string data_path_;
    terrain::VoxelVolume volume_;
    // True while read_place loads the base properties with a known token.
    bool restoring_ = false;
    // The stash token edit_volume last put in the open recording's record of
    // this Terrain's creation. When that record still holds it, the next edit
    // overwrites that one stash entry in place (write_place reads
    // reuse_token_ while edit_volume refreshes the record).
    std::uint64_t refreshed_token_ = 0;
    std::uint64_t reuse_token_ = 0;
    // What the file at saved_path_ holds, as of the last save or load: the
    // same shared chunks, so a save compares pointers, not cells.
    bool saved_ = false;
    std::string saved_path_;
    terrain::ChunkMap saved_chunks_;
};

}  // namespace engine_core
