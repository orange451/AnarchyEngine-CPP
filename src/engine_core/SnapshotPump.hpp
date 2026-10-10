#pragma once

#include "BrushVisuals.hpp"
#include "DataModel.hpp"
#include "DenseIdSet.hpp"
#include "DraggerMath.hpp"
#include "DynamicSky.hpp"
#include "Skeleton.hpp"
#include "TerrainWorld.hpp"
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
    // Linear: the Color3, which is sRGB, decoded.
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
    // How a skinned GameObject stands: shared with it, and immutable. Null
    // when its Prefab has no skeleton.
    std::shared_ptr<const Pose> pose;
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
    DraggerMode mode = DraggerMode::Translation;
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

// The first Skybox under Lighting, in tree order, when it comes before every
// DynamicSky, as the renderer reads it. present is false with no Skybox, and
// image is empty when it has no Image; either way the renderer draws no sky.
struct VisualSky {
    bool present = false;
    // Texture Paths, relative to the resources folder. Empty for none.
    std::string image;
    // The Texture's FlipY.
    bool image_flip_y = false;
    float exposure = 1.f;
    // Multiplies the light the sky gives surfaces, not the sky as drawn.
    float light_scale = 1.f;
    // Degrees about the world's Y axis, 0 up to 360.
    float rotation = 0.f;
    // As the Color3 holds it.
    ColorRgb tint{1.f, 1.f, 1.f, 1.f};
};

// The first DynamicSky under Lighting, in tree order, as the renderer reads
// it, when it comes before every Skybox; then VisualSky::present is false.
// present is false otherwise, and the values are DynamicSky's defaults.
struct VisualDynamicSky {
    bool present = false;
    float time_of_day = static_cast<float>(DynamicSky::kDefaultTimeOfDay);
    float latitude = static_cast<float>(DynamicSky::kDefaultLatitude);
    float brightness = static_cast<float>(DynamicSky::kDefaultBrightness);
    bool shadows = DynamicSky::kDefaultShadows;
    float cloud_cover = static_cast<float>(DynamicSky::kDefaultCloudCover);
    float cloud_density = static_cast<float>(DynamicSky::kDefaultCloudDensity);
    // Units per second; Y is ignored.
    Vec3 wind = DynamicSky::kDefaultWindDirection;
    // Texture Paths, relative to the resources folder. Empty for none.
    std::string sun_texture;
    std::string moon_texture;
    // Each Texture's FlipY.
    bool sun_flip_y = false;
    bool moon_flip_y = false;
    // Each Texture's Streaming is AlwaysLoaded.
    bool sun_always_loaded = false;
    bool moon_always_loaded = false;
    // Degrees across.
    float sun_size = static_cast<float>(DynamicSky::kDefaultSunSize);
    float moon_size = static_cast<float>(DynamicSky::kDefaultMoonSize);
    // Enum.EffectQuality's value: Low 0, Medium 1, High 2.
    int reflection_quality = static_cast<int>(DynamicSky::kDefaultReflectionQuality);
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
    // Units.
    float max_distance = 50.f;
    float max_roughness = 0.3f;
};

// The first AmbientOcclusionEffect under Lighting, in tree order, as the
// renderer reads it. present is false with none, and nothing is shaded.
struct VisualAmbientOcclusion {
    bool present = false;
    bool enabled = true;
    float intensity = 1.f;
    // Units.
    float radius = 1.f;
    // Enum.EffectQuality's value: Low 0, Medium 1, High 2.
    int quality = 1;
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
    // Enum.EffectQuality's value: Low 0, Medium 1, High 2.
    int terrain_quality = 2;
    // Enum.ToneMappingMode's value: Classic 0, Cinematic 1.
    int tone_mapping = 0;
    // Enum.ShadingModel's value: Standard 0, Fast 1.
    int shading_model = 0;
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
    // Whether the row's pose moves it: its Mesh has the Prefab's skeleton
    // (mesh_poses_with). Otherwise it draws unposed, in its bind pose.
    bool skinned = false;
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
    // Each texture's FlipY, false for none.
    bool diffuse_flip_y = false;
    bool normal_flip_y = false;
    bool roughness_flip_y = false;
    bool metalness_flip_y = false;
    bool emissive_flip_y = false;
    // Each texture's Streaming is AlwaysLoaded, false for none.
    bool diffuse_always_loaded = false;
    bool normal_always_loaded = false;
    bool roughness_always_loaded = false;
    bool metalness_always_loaded = false;
    bool emissive_always_loaded = false;
    ColorRgb emissive{0.f, 0.f, 0.f, 1.f};
    float metalness = 0.f;
    float roughness = 0.4f;
    float reflectivity = 0.5f;
    float transparency = 0.f;
};

// A Material's look, filled into out as resolve_prefabs fills a Model's: its
// textures' paths and its numbers; null is the default material.
class Material;
void fill_visual_material(const DataModel& game, const Material* material, VisualMesh& out);

// One draw of Brush geometry: one Material's triangles of a baked cell (world
// space, world identity) or of one Brush (its own space). look.session is the
// mesh, shared and immutable, and look.revision is unique to it; lod picks the
// Material's triangles (LOD 0 is the whole mesh, for shadows).
struct VisualBrushDraw {
    VisualMesh look;
    std::uint32_t lod = 0;
    Matrix4 world = matrix4_identity();
    ColorRgb tint{};
    float transparency = 0.f;
    // One draw of each mesh casts its whole LOD 0; the others cast nothing.
    bool casts_shadow = false;
    // The Brush, or 0 for a cell.
    InstanceId owner = 0;
};

// What one Prefab draws, found again at every Prepare, so an edit to its
// Models or their Meshes shows on the next frame.
struct VisualPrefab {
    // One per Model, in child order. A Model with no Mesh, or a Mesh with
    // neither a Path nor session geometry, adds nothing, and so does a Prefab
    // no live instance holds.
    std::vector<VisualMesh> meshes;
    // The skeleton its GameObjects pose (prefab_skeleton), or null.
    std::shared_ptr<const Skeleton> skeleton;
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
    VisualDynamicSky dynamic_sky;
    VisualBloom bloom;
    VisualReflections reflections;
    VisualAmbientOcclusion occlusion;
    // Rebuilt at every take_changes: there are few, and hover moves with the mouse.
    std::vector<VisualDragger> draggers;
    // Rebuilt at every take_changes, like draggers.
    std::vector<VisualBillboard> billboards;
    // Every drawn WireframeAdornment's lines, in world space, ready for the
    // renderer's line pass: two points a line, x y z r g b a each. Rebuilt
    // at every take_changes, like draggers.
    std::vector<float> wire_lines;
    // Every Terrain in Workspace, as TerrainWorld shows it. Pointers only: the
    // chunk list and look are immutable and shared with the simulation.
    std::vector<TerrainView> terrains;
    // Every Brush in Workspace (BrushVisuals): anchored opaque ones baked into
    // cells, the rest one by one.
    std::vector<VisualBrushDraw> brushes;
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
    // The TerrainWorld whose views() resolve_terrains reads each take_changes.
    // Set once, outside the per-frame windows (Engine's constructor sets it,
    // and clears it to null in its destructor before terrain_ is torn down).
    void set_terrain_world(const TerrainWorld* terrains);

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
    void resolve_wireframes(DataModel& game);
    // Copies terrain_world_'s views() (pointers only -- see VisualSnapshot::terrains)
    // into base_.terrains. TerrainWorld::update already ran on SimulationThread
    // before take_changes is called, so there is nothing more to read from game here.
    void resolve_terrains(DataModel& game);
    BrushVisuals brushes_;
    // Points each row's anchor at its anchor_instance's row in dst, after overrides.
    void anchor_billboards(VisualSnapshot& dst) const;
    void release_prefab(std::uint32_t entry);
    // Points inst at guid's entry, when it names another.
    void set_row_prefab(VisualInstance& inst, const std::string& guid);
    // Fills base_.prefabs from each entry's Prefab, as the DataModel is now.
    void resolve_prefabs(DataModel& game);
    // Gives each row whose Prefab has a skeleton its GameObject's pose, as
    // the DataModel is now, and clears every other row's.
    void resolve_poses(DataModel& game);
    // Fills base_.lighting, base_.sky, base_.dynamic_sky, base_.bloom,
    // base_.reflections, and base_.occlusion from the place's Lighting, as
    // the DataModel is now.
    void resolve_lighting(DataModel& game);
    // The first T under root, depth first in child order, or null.
    template <class T>
    const T* find_first(const DataModel& game, InstanceId root);
    // The first instance under root, in tree order, that is any of T.
    template <class... T>
    const DataModel* find_first_of(const DataModel& game, InstanceId root);

    VisualSnapshot base_{};
    VisualSnapshot buffers_[2]{};
    int front_ = 0;
    std::uint64_t next_frame_ = 1;
    std::atomic<std::uint64_t> published_frame_{0};
    std::vector<SnapshotOverride> overrides_;
    // Not owned. Null until Engine's constructor calls set_terrain_world.
    const TerrainWorld* terrain_world_ = nullptr;
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
    // The WireframeAdornments resolve_wireframes walks.
    std::vector<InstanceId> wireframe_ids_;
};

}  // namespace engine_core
