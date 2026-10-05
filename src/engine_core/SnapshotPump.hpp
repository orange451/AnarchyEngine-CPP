#pragma once

#include "DataModel.hpp"
#include "DenseIdSet.hpp"
#include "DraggerMath.hpp"
#include "types.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace anarchy::amesh {
struct Data;
}

namespace engine_core {


// What a light's row shines. A PointLight or SpotLight sits at the row's
// world translation, and a SpotLight points down the row's -Z. A
// DirectionalLight is not a GameObject: its row's world is the identity, and
// direction says where it is.
struct VisualLight {
    enum class Kind : std::uint8_t { None, Point, Spot, Directional };
    // None when the row is not a light.
    Kind kind = Kind::None;
    bool enabled = false;
    // Linear, as the Color3 holds it.
    float color[3] = {1.f, 1.f, 1.f};
    float intensity = 0.f;
    // 0 for a DirectionalLight, which reaches everywhere.
    float radius = 0.f;
    // A SpotLight's OuterFOV in degrees, and InnerFOVScale. 0 for a PointLight.
    float outer_fov = 0.f;
    float inner_fov_scale = 0.f;
    // Its Shadows, and a DirectionalLight's ShadowDistance (0 otherwise).
    bool shadows = false;
    float shadow_distance = 0.f;
    // A DirectionalLight's Direction, toward the light, as given. 0 otherwise.
    float direction[3] = {0.f, 0.f, 0.f};
};

// A GameObject in Workspace, or a DirectionalLight there, which has no Transform.
struct VisualInstance {
    InstanceId id = 0;
    Matrix4 world = matrix4_identity();
    bool alive = true;
    WriteOrigin transform_origin = WriteOrigin::Simulation;
    // What the row draws: its entry in VisualSnapshot::prefabs. 0 is no
    // Prefab, and a GameObject without one draws nothing.
    std::uint32_t prefab = 0;
    // A Camera's FieldOfView, in degrees. 0 when the row is not a Camera.
    float field_of_view = 0.f;
    // The GameObject's Color, which multiplies each Material Color it draws,
    // and its Transparency, clamped to 0..1, which stacks on each Material's.
    ColorRgb color{1.f, 1.f, 1.f, 1.f};
    float transparency = 0.f;
    // The GameObject's Scale, which multiplies the size its Prefab draws at.
    float scale = 1.f;
    VisualLight light;
};

// An active Dragger as a scene view draws it: where its handles sit, the one
// the mouse is over, and the one being dragged.
struct VisualDragger {
    DraggerFrame frame{};
    DraggerHandle hovered = DraggerHandle::None;
    DraggerHandle active = DraggerHandle::None;
};

// A BillboardGui runner::GuiLayer draws: drawn() and Visible. anchor is its
// anchor_instance()'s world translation; when that instance has a row in the
// same snapshot, it is that row's, after overrides, so a billboard and what
// it floats over are always where the same frame put them.
struct VisualBillboard {
    InstanceId id = 0;
    // 0 for the world origin.
    InstanceId anchor_instance = 0;
    Vec3 anchor{};
    bool always_on_top = false;
};

// The first Skybox under Lighting, in tree order, as the renderer reads it.
// present is false with no Skybox, and image is empty when it has no Image;
// either way the renderer draws no sky.
struct VisualSky {
    bool present = false;
    // Texture Paths, relative to the resources folder. Empty for none.
    std::string image;
    float exposure = 1.f;
    // Multiplies the light the sky gives surfaces, not the sky as drawn.
    float light_scale = 1.f;
    // Degrees about the world's Y axis, 0 up to 360.
    float rotation = 0.f;
    // As the Color3 holds it.
    ColorRgb tint{1.f, 1.f, 1.f, 1.f};
};

// The first BloomEffect under Lighting, in tree order, as the renderer reads
// it. present is false with none, and the renderer draws no bloom.
struct VisualBloom {
    bool present = false;
    bool enabled = true;
    float intensity = 0.05f;
    // Pixels at a 1080-pixel-tall view.
    float size = 24.f;
    float threshold = 0.f;
};

// The first ScreenSpaceReflections under Lighting, in tree order, as the
// renderer reads it. present is false with none, and nothing is traced.
struct VisualReflections {
    bool present = false;
    bool enabled = true;
    float intensity = 1.f;
    // Studs.
    float max_distance = 50.f;
    float max_roughness = 0.3f;
};

// Lighting's properties the renderer reads, found again at every Prepare.
// Each is as Lighting has it; a place with no Lighting has the defaults.
struct VisualLighting {
    ColorRgb ambient{0.5f, 0.5f, 0.5f, 1.f};
    float exposure = 1.f;
    float saturation = 1.2f;
    float gamma = 2.2f;
    // Enum.AntialiasingMode's value: None 0, FXAA 1.
    int antialiasing = 1;
};

// One Model's Mesh, as the renderer loads it: a file, or the geometry this
// play session's edits made (Mesh::session_geometry), which wins while it lasts.
struct VisualMesh {
    // Relative to the project's resources folder. Empty when session is set.
    std::string path;
    // Immutable once published. revision is unique across every Mesh, so a
    // renderer uploads again only when it changes.
    std::shared_ptr<const anarchy::amesh::Data> session;
    std::uint64_t revision = 0;
    // The Mesh, which a renderer can key its upload of session by.
    InstanceId mesh = 0;
    // The Model's Material: its DiffuseTexture's Path, relative to the
    // resources folder, and its Color. An empty path, as with no Material or
    // no DiffuseTexture, draws the Color alone; no Material is white.
    std::string diffuse_texture;
    ColorRgb color{};
    // Its other textures' Paths, each empty for none, and its numbers, each
    // clamped to 0..1. No Material has a Material's defaults.
    std::string normal_texture;
    std::string roughness_texture;
    std::string metalness_texture;
    std::string emissive_texture;
    ColorRgb emissive{0.f, 0.f, 0.f, 1.f};
    float metalness = 0.f;
    float roughness = 0.4f;
    float reflectivity = 0.5f;
    float transparency = 0.f;
};

// What one Prefab draws, found again at every Prepare, so an edit to its
// Models or their Meshes shows on the next frame.
struct VisualPrefab {
    // One per Model, in child order. A Model with no Mesh, or a Mesh with
    // neither a Path nor session geometry, adds nothing, and so does a Prefab
    // no live instance holds.
    std::vector<VisualMesh> meshes;
};

// Path C. Applied after the DataModel copy. Gone on the next Prepare
// unless the job submits it again. Physics never sees it.
struct SnapshotOverride {
    InstanceId id = 0;
    VisualField field = VisualField::Transform;
    Matrix4 transform = matrix4_identity();
};

struct VisualSnapshot {
    std::uint64_t frame = 0;
    Matrix4 camera = matrix4_identity();
    std::vector<VisualInstance> instances;
    // Indexed by VisualInstance::prefab. Entry 0 is always empty.
    std::vector<VisualPrefab> prefabs;
    VisualLighting lighting;
    VisualSky sky;
    VisualBloom bloom;
    VisualReflections reflections;
    // Rebuilt at every take_changes: there are few, and hover moves with the mouse.
    std::vector<VisualDragger> draggers;
    // Rebuilt at every take_changes, like draggers.
    std::vector<VisualBillboard> billboards;
    // DataModel::resources_root as the snapshot was taken: the folder the
    // paths above are under.
    std::filesystem::path resources_root;
};

// Double buffer plus the one-frame override list.
// Only RenderThread calls these methods. front() is the buffer Perform reads.
// find() returns a pointer into that buffer; it dies at the next publish.
class SnapshotPump {
public:
    void reserve(std::size_t instances);

    void begin_prerender_window(DataModel& game);
    void end_prerender_window(DataModel& game);

    // Path C. No DataModel write.
    void override_visual(const SnapshotOverride& override);
    void set_camera(const Matrix4& camera);

    // Copies dirty DataModel fields into the base snapshot. Needs the DataModel
    // lock, and is the only step here that does.
    void take_changes(DataModel& game);
    // Clones the base snapshot into the back buffer and applies overrides onto
    // the back buffer only. Touches no DataModel state, so it runs after the
    // lock is released. Does not swap.
    void finish_copy();
    // take_changes, then finish_copy. Call publish() after.
    void prepare_copy(DataModel& game);

    void publish();

    const VisualSnapshot& front() const;
    const VisualInstance* find(InstanceId id) const;
    std::uint64_t published_frame() const { return published_frame_.load(); }

private:
    // A Prefab GUID some row names. Rows share an entry, so the Prefab's
    // Models are found once per Prepare however many rows draw it.
    struct PrefabEntry {
        std::string guid;
        InstanceId cached = 0;
        std::uint32_t rows = 0;
    };

    VisualInstance* base_find(InstanceId id);
    void erase_base(InstanceId id);
    void apply_live(DataModel& game, const Invalidation& change);
    void resync(DataModel& game);
    void blit(VisualSnapshot& dst) const;
    void apply_overrides(VisualSnapshot& dst);
    // The row's entry for guid, counting the row; 0 for an empty guid.
    std::uint32_t acquire_prefab(const std::string& guid);
    // The active Draggers' rows, from the live tree.
    void resolve_draggers(DataModel& game);
    // The drawn, visible BillboardGuis' rows, from the live tree.
    void resolve_billboards(DataModel& game);
    // Points each row's anchor at its anchor_instance's row in dst, after overrides.
    void anchor_billboards(VisualSnapshot& dst) const;
    void release_prefab(std::uint32_t entry);
    // Points inst at guid's entry, when it names another.
    void set_row_prefab(VisualInstance& inst, const std::string& guid);
    // Fills base_.prefabs from each entry's Prefab, as the DataModel is now.
    void resolve_prefabs(DataModel& game);
    // Fills base_.lighting, base_.sky, base_.bloom, and base_.reflections from the
    // place's Lighting, as the DataModel is now.
    void resolve_lighting(DataModel& game);
    // The first T under root, depth first in child order, or null.
    template <class T>
    const T* find_first(const DataModel& game, InstanceId root);

    VisualSnapshot base_{};
    VisualSnapshot buffers_[2]{};
    int front_ = 0;
    std::uint64_t next_frame_ = 1;
    std::atomic<std::uint64_t> published_frame_{0};
    std::vector<SnapshotOverride> overrides_;
    bool camera_pending_ = false;
    Matrix4 pending_camera_ = matrix4_identity();
    bool window_open_ = false;
    // The ids with a row in base_.instances, position for position.
    DenseIdSet base_ids_;
    // Indexed like VisualSnapshot::prefabs; entry 0 is unused. A GUID's entry
    // is freed when its last row leaves it, and reused. These allocate only
    // when a row names a Prefab no other row does.
    std::vector<PrefabEntry> prefab_entries_;
    std::vector<std::uint32_t> free_prefab_entries_;
    std::unordered_map<std::string, std::uint32_t> prefab_by_guid_;
    // find_first's walk, kept so a Prepare allocates nothing.
    std::vector<InstanceId> lighting_walk_;
    // The Draggers resolve_draggers walks, kept so it does not allocate each frame.
    std::vector<InstanceId> dragger_ids_;
    // The BillboardGuis resolve_billboards walks, kept so it does not allocate each frame.
    std::vector<InstanceId> billboard_ids_;
};

}  // namespace engine_core
