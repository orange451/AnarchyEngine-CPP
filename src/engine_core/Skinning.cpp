#include "Skinning.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "LuaApi.hpp"

namespace engine_core {

std::shared_ptr<const Skeleton> prefab_skeleton(const DataModel& game, const std::string& prefab_guid) {
    if (prefab_guid.empty()) {
        return nullptr;
    }
    const std::optional<InstanceId> prefab = game.find_guid(prefab_guid);
    if (!prefab || dynamic_cast<const Prefab*>(game.instance(*prefab)) == nullptr) {
        return nullptr;
    }
    for (InstanceId child = game.first_child(*prefab); child != 0; child = game.next_sibling(child)) {
        const auto* model = dynamic_cast<const Model*>(game.instance(child));
        if (model == nullptr) {
            continue;
        }
        const LuaSlot mesh_ref = model->reference(Model::kMeshReference);
        const auto* mesh =
            mesh_ref.kind == LuaSlot::Kind::Instance ? dynamic_cast<const Mesh*>(game.instance(mesh_ref.id)) : nullptr;
        if (mesh == nullptr) {
            continue;
        }
        if (std::shared_ptr<const Skeleton> skeleton = mesh->skeleton()) {
            return skeleton;
        }
    }
    return nullptr;
}

bool mesh_poses_with(const Mesh& mesh, const Skeleton& skeleton) {
    const std::shared_ptr<const Skeleton> own = mesh.skeleton();
    return own != nullptr && own->signature == skeleton.signature;
}

}  // namespace engine_core
