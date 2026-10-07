#pragma once

#include "DataModel.hpp"
#include "InstanceRef.hpp"

#include <optional>
#include <string>

namespace engine_core {

class Terrain;

// One entry in a Terrain's material list: voxels whose Id is this one's draw
// and collide as its Material. It lives only in a Terrain, has no row in the
// Game Explorer, and keeps the Terrain it was first given; Terrain's
// add_material makes them, never Instance.new. Several may share a Material.
//
// Id        number     1 to 255, read-only to scripts, saved. Fixed for its
//                      life, except when it arrives in a Terrain where another
//                      already holds it (see on_parent_changed).
// Material  Material?  what its voxels look like. Nil draws the default.
//
// Name starts as its Material's name, and follows a new Material while it is
// still the old one's name (or "TerrainMaterial").
class TerrainMaterial : public DataModel {
public:
    static constexpr int kMaxId = 255;

    TerrainMaterial(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}
    const char* class_name() const override { return "TerrainMaterial"; }
    bool hidden_in_explorer() const override { return true; }
    bool parent_locked() const override { return true; }

    // 0 until it is given one, or when it arrived in a Terrain that had all
    // 255 taken; an Id of 0 draws nothing of its own.
    int material_id() const { return material_id_; }
    // SimulationThread. Load, paste, and Terrain::add_material only; scripts
    // read Id. Refuses anything but 1 to 255.
    std::optional<std::string> set_material_id(int value);
    // SimulationThread. The Id property's write: load, Stop, undo, paste, and
    // accepting changes from disk. value is 0 to 255; 0 means unassigned.
    // Outside a Terrain it is stored as it is (0 without recording), and
    // on_parent_changed settles it on arrival. Inside one, the arrival rule
    // applies at once: 0, or an Id another TerrainMaterial of that Terrain
    // holds, takes the lowest free Id instead.
    void load_material_id(int value);

    // The Material slot, as a script reads it.
    LuaSlot material() const;
    // The live Material, or 0 when there is none or it is gone.
    InstanceId material_instance() const;
    // SimulationThread. nil clears; anything but a Material is refused.
    std::optional<std::string> set_material(const LuaSlot& value);

protected:
    void on_reuse() override;
    // Whoever already holds an Id keeps it: arriving in a Terrain where the
    // Id is 0 or a sibling holds it takes the Terrain's lowest free Id, as an
    // unrecorded edit (it may run inside an undo).
    void on_parent_changed(InstanceId previous, InstanceId next) override;

private:
    // The Terrain this is a child of, or null.
    const Terrain* terrain_parent() const;
    // The arrival rule, shared by on_parent_changed and load_material_id:
    // when wanted is 0 or another TerrainMaterial of terrain holds it, this
    // one takes terrain's lowest free Id (its own counting as free) as an
    // unrecorded edit, and it returns true. Otherwise it changes nothing and
    // returns false.
    bool settle_id(const Terrain& terrain, int wanted);

    int material_id_ = 0;
    InstanceRef material_;
};

}  // namespace engine_core
