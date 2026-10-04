#pragma once

#include "ide/IdePane.hpp"
#include "GuiLayer.hpp"
#include "MeshCache.hpp"
#include "TextureCache.hpp"
#include "Renderer.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace engine_core {
class DataModel;
class Engine;
struct VisualSnapshot;
}

namespace runner {

class Runner;
class SceneFeed;

// Scene viewport. Draws each GameObject in Workspace, at any depth, that has
// a Prefab: every Model's Mesh, loaded as AMESH from the project's resources
// folder, at the GameObject's Transform, seen from the view's camera. What it
// draws comes from the engine's published VisualSnapshot, through the
// runner's SceneFeed, never from the DataModel. A GameObject with no Prefab
// draws nothing. Each enabled PointLight, SpotLight, and DirectionalLight in
// Workspace or under Lighting lights them, with Lighting's Ambient, Exposure,
// Saturation, and Gamma. The first Skybox under Lighting is drawn behind
// them and lights them too. Over them lies the floor grid, with the world's
// X and Z axes on it, while the Runner's sceneGrid is on. Over that, each
// selected PhysicsObject in Workspace is outlined as it collides
// (PhysicsWorld::collision_outline), at its GameObject's Transform as the
// snapshot has it, so the outline stays on the drawn mesh during play, or at
// its own Transform when it moves none.
// The view is linked to one Camera, by GUID, and sees from that Camera's
// Transform and FieldOfView as the snapshot has them. The list at the top
// right offers each Camera in Workspace, at any depth, in tree order. A view
// with no link takes the first one, and so does every view when another place
// is opened. A linked Camera that is destroyed or leaves Workspace stays
// linked, and the view keeps drawing from where that Camera last was; if it
// comes back, as an undo brings it, the view follows it again. Before any
// Camera, the view sees from Renderer's fixed camera.
// The studio's first view stays open. Another, from Window > New Scene View, is closable, and draws the same place.
// The corner label is how many times this view is painted per second, averaged
// over a quarter of a second. That count keeps moving while the simulation is paused.
//
// Keys and the mouse over this view go to the place's UserInputService, in edit
// mode and in play. A press here takes keyboard focus and makes this view's
// Camera the Workspace's CurrentCamera; losing focus ends whatever was still
// held. While a script sets MouseBehavior to a lock and this view has focus,
// the pointer is locked in it and its motion goes to GetMouseDelta. Only the
// focused view, the one last clicked, ever locks it. Shift+Esc takes the focus
// away, which frees the pointer until the view is clicked again; MouseBehavior
// stays as the script set it, so that click locks the pointer again.
//
// Over the drawing, under the camera list, the Gui service's
// ScreenGuis are drawn (GuiLayer), in a SubScene so the studio's styles do not
// reach them. The eye left of the camera list turns them off in edit mode, to
// see the scene without them; a test always draws them, and hides the eye and
// the camera list. A press on a GUI element goes to it, and to UserInputService with
// gameProcessedEvent true; a press on a ScreenGui's own area goes on to the
// scene as if no GUI were there.
class GameView : public ide::IdePane {
public:
    explicit GameView(Runner& runner, std::string name = "Scene View", bool closable = false);

    // The next paint reads back what it drew, before the overlays, and hands it to
    // done on this thread. A view that does not paint, such as a hidden tab,
    // does not call done until it paints again, nor does a paint the renderer
    // was not ready to draw.
    void requestCapture(std::function<void(ViewPixels)> done);

    // The linked Camera's GUID. Empty before the view has one.
    const std::string& cameraGuid() const { return cameraGuid_; }
    // Links the view to the Camera with this GUID, as choosing it in the list does.
    void linkCamera(std::string guid);
    // The list at the top right. Its items are the Cameras' names.
    jadefx::ComboBox& cameraList() { return *cameraBox_; }
    // The GUIs drawn over the view.
    GuiLayer& guiLayer() { return *guiLayer_; }
    // The eye left of the camera list. Selected, as it starts, draws the GUIs.
    jadefx::ToggleButton& guiToggle() { return *guiToggle_; }
    // The player's view: no camera list, since a game shows only
    // itself, and the view follows the Workspace's CurrentCamera, as a script
    // sets it, rather than the Camera picked in the list.
    void setPlayerView(bool player);

    // How often the view painted, averaged over the last quarter second: the
    // frames a second, and the time between paints in milliseconds. 0 before
    // the first average.
    int framesPerSecond() const { return measuredFps_.load(); }
    double frameMilliseconds() const { return measuredFrameMs_.load(); }
    // The view painted lately, so those numbers are what it does now. A tab
    // not in front, or a minimized window, stops painting.
    bool frameTimeCurrent() const;

protected:
    void layoutChildren() override;
    // The viewport clear covers children drawn in the normal pass.
    void renderChildren(jadefx::UiRenderer& renderer, float opacity) override;
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void sceneChanged(jadefx::Scene* previous) override;
    void handleMousePressed(const jadefx::MouseEvent& event) override;
    void handleMouseReleased(const jadefx::MouseEvent& event) override;
    void handleMouseDragged(const jadefx::MouseEvent& event) override;
    void handleMouseMoved(const jadefx::MouseEvent& event) override;
    void handleScroll(jadefx::ScrollEvent& event) override;
    void handleKey(jadefx::KeyEvent& event) override;
    void handleFocusLost() override;

private:
    void notePaint();
    // Makes this view's linked Camera the Workspace's CurrentCamera; with
    // onlyIfNone, only while the Workspace has none.
    // Tells this view's camera the view's size, when either changed.
    void reportViewportSize();
    void noteCurrentCamera(bool onlyIfNone = false);
    // Locks or frees the scene's pointer to match MouseBehavior, and hands the
    // scene's pointer motion to UserInputService while locked. Two views can
    // share one Scene, so only the focused view locks it or reads it as let
    // go; an unfocused view only ever releases a lock of its own.
    void syncPointerLock();
    // Walks Workspace for the Cameras, and resolves the link. Skipped when the
    // DataModel is busy; the previous list and link stay. A link that resolves
    // to a new Camera makes it the CurrentCamera when the Workspace has none.
    void refreshWorkspace();
    void readWorkspace();
    // Fills outlines_ from the selection, under readWorkspace's lock. An
    // outline is made again only when what it was made from changed.
    void readSelectedBodies();
    // The outlines in world space, into the renderer, each placed by the
    // snapshot's row of its GameObject when it has one.
    void collectOutlines(const engine_core::VisualSnapshot& snapshot);
    // Builds the snapshot's Dragger handles for this view's camera and size.
    void collectHandles(const engine_core::VisualSnapshot& snapshot);
    // Shows the GUIs, the eye, and the camera list as the toggle, a test, and
    // the player's view want them.
    void refreshOverlays();
    // Puts the Cameras in the list, and selects the linked one, when either changed.
    void refreshCameraList();
    // Points the renderer at the linked Camera's snapshot row, when it has one.
    void followCamera(const engine_core::VisualSnapshot& snapshot);
    // Fills meshDraws_ and lightDraws_ from the feed's newest snapshot, and
    // gives the renderer its Lighting. GL context current.
    void collectMeshes();
    bool ensureGraphics();
    // A window point as UserInputService wants it: points from this view's top-left.
    float localX(double x) const;
    float localY(double y) const;

    Renderer renderer_;
    // The IDE's; it outlives the view. Its sceneGrid is read each paint.
    Runner* runner_ = nullptr;
    // The runner's; it outlives the engine that writes it.
    SceneFeed* feed_ = nullptr;
    MeshCache meshes_;
    TextureCache textures_;
    // Per frame: each snapshot Prefab's loaded meshes, with their textures and
    // colors, then one draw per row and mesh at that row's Transform.
    std::vector<std::vector<MeshDraw>> prefabMeshes_;
    std::vector<MeshDraw> meshDraws_;
    // Per frame: each enabled PointLight, SpotLight, and DirectionalLight in the snapshot.
    std::vector<LightDraw> lightDraws_;
    // The session game. The runner keeps it alive for this view.
    engine_core::DataModel* game_ = nullptr;
    // The walk through Workspace that fills cameraScratch_.
    std::vector<engine_core::InstanceId> walkScratch_;
    struct CameraChoice {
        engine_core::InstanceId id = 0;
        std::string guid;
        std::string name;
        bool operator==(const CameraChoice& other) const {
            return id == other.id && guid == other.guid && name == other.name;
        }
    };
    // The Cameras in Workspace, in tree order, as of the last walk.
    std::vector<CameraChoice> cameras_;
    std::vector<CameraChoice> cameraScratch_;
    // The link. cameraId_ is its Camera while that is in Workspace, else 0.
    std::string cameraGuid_;
    engine_core::InstanceId cameraId_ = 0;
    // The camera and size last written to that camera's ViewportSize.
    engine_core::InstanceId sizedCamera_ = 0;
    double sizedWidth_ = -1.0;
    double sizedHeight_ = -1.0;
    // The root's GUID at the last walk. Another means another place is open.
    std::string placeGuid_;
    // A selected PhysicsObject in Workspace, as of the last read.
    struct BodyOutline {
        engine_core::InstanceId id = 0;
        // What lines was made from: the Shape, Size times its GameObject's
        // Scale, and Anchored, where the
        // shape is centered, and the Mesh, its file, and its session
        // geometry's revision, with the points and triangles read from it.
        // No Mesh for any Shape but a Hull or a Custom.
        int shape = -1;
        engine_core::Vec3 size{};
        bool anchored = false;
        engine_core::Vec3 center{};
        engine_core::InstanceId mesh = 0;
        std::string meshPath;
        std::uint64_t meshRevision = 0;
        std::vector<engine_core::Vec3> meshPoints;
        std::vector<std::uint32_t> meshTriangles;
        // Segments in the body's space.
        std::vector<engine_core::Vec3> lines;
        // The GameObject it moves, or 0, and where the body is: that
        // GameObject's Transform, else its own.
        engine_core::InstanceId driven = 0;
        engine_core::Matrix4 transform = engine_core::matrix4_identity();
    };
    std::vector<BodyOutline> outlines_;
    std::vector<BodyOutline> outlineScratch_;
    // The selection and the revision it was read at.
    std::vector<engine_core::InstanceId> selected_;
    std::uint64_t selectionSeen_ = ~std::uint64_t{0};
    // Per frame: the outlines in world space, as Renderer::setOutlines takes them.
    std::vector<float> outlinePoints_;
    // The camera followCamera last followed, which the handles are sized for.
    engine_core::Matrix4 viewCamera_ = engine_core::matrix4_identity();
    float viewFov_ = 0.f;
    // Per frame: every Dragger's handles, and one Dragger's at a time.
    std::vector<engine_core::HandleVertex> handleVertices_;
    std::vector<engine_core::HandleVertex> handleScratch_;
    jadefx::ComboBox* cameraBox_ = nullptr;
    jadefx::ToggleButton* guiToggle_ = nullptr;
    GuiLayer* guiLayer_ = nullptr;
    // Holds guiLayer_ as its root.
    jadefx::SubScene* guiScene_ = nullptr;
    // What the list shows now, so it is rebuilt only on a change.
    std::vector<CameraChoice> listed_;
    std::string listedGuid_;
    // The engine that owns game_. Each paint tells its render thread a frame happened.
    engine_core::Engine* engine_ = nullptr;
    // Whether this view last locked its scene's pointer.
    bool pointerLocked_ = false;
    // setPlayerView: the link follows the Workspace's CurrentCamera.
    bool followCurrentCamera_ = false;
    // setPlayerView: no camera list or eye, and the GUIs always drawn.
    bool playerView_ = false;
    // Paints in the current window. framesPerSecond and frameMilliseconds read the finished average.
    std::chrono::steady_clock::time_point paintWindowStart_{};
    int paintWindowFrames_ = 0;
    bool paintWindowOpen_ = false;
    std::atomic<int> measuredFps_{0};
    std::atomic<double> measuredFrameMs_{0};
    std::chrono::steady_clock::time_point lastMeasured_{};
    bool graphicsAttempted_ = false;
    bool graphicsReady_ = false;
    static constexpr int kGraphicsTries = 5;
    int graphicsTries_ = 0;
    std::chrono::steady_clock::time_point graphicsRetryAt_{};
    // Waiting for the next paint's pixels. UI thread only.
    std::vector<std::function<void(ViewPixels)>> captures_;
};

}  // namespace runner
