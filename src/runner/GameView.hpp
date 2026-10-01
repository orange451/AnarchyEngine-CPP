#pragma once

#include "ide/IdePane.hpp"
#include "GuiLayer.hpp"
#include "MeshCache.hpp"
#include "TextureCache.hpp"
#include "Renderer.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
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
// Saturation, and Gamma.
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
// Over the drawing, under the FPS label and the camera list, the Gui service's
// ScreenGuis are drawn (GuiLayer), in a SubScene so the studio's styles do not
// reach them. A press on a GUI element goes to it, and to UserInputService with
// gameProcessedEvent true; a press on a ScreenGui's own area goes on to the
// scene as if no GUI were there.
class GameView : public ide::IdePane {
public:
    explicit GameView(Runner& runner, std::string name = "Scene View", bool closable = false);

    // The next paint reads back what it drew, before the label, and hands it to
    // done on this thread. A view that does not paint, such as a hidden tab,
    // does not call done until it paints again.
    void requestCapture(std::function<void(ViewPixels)> done);

    // The linked Camera's GUID. Empty before the view has one.
    const std::string& cameraGuid() const { return cameraGuid_; }
    // Links the view to the Camera with this GUID, as choosing it in the list does.
    void linkCamera(std::string guid);
    // The list at the top right. Its items are the Cameras' names.
    jadefx::ComboBox& cameraList() { return *cameraBox_; }
    // The GUIs drawn over the view.
    GuiLayer& guiLayer() { return *guiLayer_; }

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
    void refreshFpsLabel();
    // Makes this view's linked Camera the Workspace's CurrentCamera; with
    // onlyIfNone, only while the Workspace has none.
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
    // The root's GUID at the last walk. Another means another place is open.
    std::string placeGuid_;
    jadefx::ComboBox* cameraBox_ = nullptr;
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
    // Paints in the current window. The label reads the finished average.
    std::chrono::steady_clock::time_point paintWindowStart_{};
    int paintWindowFrames_ = 0;
    bool paintWindowOpen_ = false;
    std::atomic<int> measuredFps_{0};
    jadefx::Label* fpsLabel_ = nullptr;
    int shownFps_ = -1;
    bool graphicsAttempted_ = false;
    bool graphicsReady_ = false;
    static constexpr int kGraphicsTries = 5;
    int graphicsTries_ = 0;
    std::chrono::steady_clock::time_point graphicsRetryAt_{};
    // Waiting for the next paint's pixels. UI thread only.
    std::vector<std::function<void(ViewPixels)>> captures_;
};

}  // namespace runner
