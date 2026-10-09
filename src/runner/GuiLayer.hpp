#pragma once

#include "Matrix4.hpp"
#include "SceneDepth.hpp"
#include "GuiTree.hpp"
#include "SnapshotPump.hpp"
#include "types.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_core {
class DataModel;
class Engine;
class GuiValues;
}

namespace runner {

// What placeBillboards places the BillboardGuis by: the camera the renderer
// draws this frame with.
struct BillboardView {
    engine_core::Matrix4 view = engine_core::matrix4_identity();
    float fovYDegrees = 60.f;
    // The 3D pane in window points: what Renderer::draw is given.
    double paneX = 0, paneY = 0, paneWidth = 0, paneHeight = 0;
};

// The Gui service's ScreenGuis, as JadeFX nodes over a Scene View. Each
// ScreenGui under the service, directly or through Folders, fills the layer;
// each GuiBase inside one, through a chain of GuiBases, is a node of its
// class's element type (screengui, pane, imagepane, hbox, vbox, label,
// button, textfield) whose id is its Name and whose classes are its ClassList. The
// CSS instances under a GuiBase, joined in child order, are that node's
// stylesheet, and those directly under the service are the layer's, so they
// style every ScreenGui and every BillboardGui the layer draws.
//
// The layer is the root of a jadefx::SubScene whose user-agent stylesheet is
// defaultStylesheet, so the studio's theme and stylesheets do not reach the
// game's nodes, and :root is the layer.
//
// sync reads the tree under a short read lock, on the UI thread, and changes
// only the nodes whose instance changed: a node keeps its state, such as a
// TextField's caret, while its instance does. Typing in a TextField writes
// Text back. Mouse events on a node fire its instance's events on the
// simulation thread, and the layer hands the mouse to its Scene View as
// GuiInput says. A ScreenGui's own area does not take the mouse.
//
// An ImagePane's node draws its Image's file, by the Texture's Path under the
// project's resources folder, as a JadeFX background image, upside down when
// the Texture's FlipY is set. The files load
// after the lock is let go, so decoding never holds up the simulation; each is
// looked at again at most once a second and reloaded when it changed. One
// that is missing or does not decode draws nothing, and says why in Output
// once per version of the file.
//
// Every BillboardGui in Workspace or Core that is not inside another GUI is a
// node too, element type billboardgui, built and updated as a ScreenGui is.
// placeBillboards puts each where the frame's snapshot says, by the camera
// the renderer draws that frame with: centred on its anchor's point on screen
// and laid out with the points one world unit covers there as its available
// width and height, so a percentage on the billboard is world units. Their
// paint order is depth tested far to near, then AlwaysOnTop far to near, then
// the ScreenGuis. A depth-tested billboard takes no mouse while the scene
// under the cursor (setCursorDepth) is nearer than it.
class GuiLayer : public jadefx::Pane {
public:
    GuiLayer(engine_core::Engine& engine, GuiInput input);
    ~GuiLayer() override;

    const char* getElementType() const override { return "gui-layer"; }

    // The game UI's user-agent stylesheet: no outlines, backgrounds, or
    // padding. The hover wash, the focus ring, and a TextField's caret and
    // selection stay. Any CSS a project has wins over it.
    static const char* defaultStylesheet();

    // Brings the nodes up to date with the Gui service. Skipped, keeping the
    // nodes as they are, when the DataModel is busy.
    void sync();

    // The node for an instance, or null when it is not drawn. For tests.
    jadefx::Node* nodeFor(engine_core::InstanceId id) const;

    // Places each drawn BillboardGui by its row in the frame's snapshot, as
    // seen by view. A billboard with no row, or behind the camera, is hidden.
    void placeBillboards(const std::vector<engine_core::VisualBillboard>& rows, const BillboardView& view);
    // The cursor's scene depth from the last paint, or none. A depth-tested
    // billboard farther than it takes no mouse.
    void setCursorDepth(std::optional<float> depth);
    // What setCursorDepth last gave. For tests.
    std::optional<float> cursorDepth() const { return cursorDepth_; }
    // Children in paint order, for tests.
    std::vector<jadefx::Node*> paintOrder() const;
    // For the occluded draw: the depth a billboard node draws at, and whether
    // it is depth tested.
    struct PlacedBillboard {
        float depth;
        bool alwaysOnTop;
    };
    // Null for a node that is not a placed billboard.
    const PlacedBillboard* placedFor(const jadefx::Node* node) const;
    // The depth the Scene View's last draw left, which a depth-tested
    // billboard is drawn behind; texture 0 when nothing hides it.
    void setSceneDepth(const SceneDepth& depth) { sceneDepth_ = depth; }

protected:
    // Every ScreenGui fills the layer, and each placed billboard is centred on
    // its anchor's point.
    void layoutChildren() override;
    // The children in paint order, which painting and picking follow, so a
    // reorder never takes a node out of the layer and focus and presses
    // survive it. A child restack has not ordered yet comes after, in list order.
    void visitChildren(const std::function<void(jadefx::Node*)>& visitor) override;
    // The children in paint order, each depth-tested billboard hidden where
    // the scene's depth is nearer than it.
    void renderChildren(jadefx::UiRenderer& renderer, float opacity) override;

private:
    // A drawn BillboardGui's node and where this frame puts it.
    struct Placement {
        engine_core::InstanceId id = 0;
        std::shared_ptr<jadefx::Node> node;
        // False until placeBillboards finds it in front of the camera.
        bool placed = false;
        bool alwaysOnTop = false;
        // Its anchor's point on screen, in window points.
        double x = 0;
        double y = 0;
        double pixelsPerUnit = 0;
        double distance = 0;
        PlacedBillboard drawn{};
    };

    // The ScreenGuis under id, a service or a Folder, in tree order.
    void collectScreens(engine_core::InstanceId id, std::vector<std::shared_ptr<jadefx::Node>>& out);
    // The drawn BillboardGuis' nodes, made or brought up to date, in id order.
    void collectBillboards(std::vector<engine_core::InstanceId>& ids, std::vector<std::shared_ptr<jadefx::Node>>& nodes);
    // Puts the billboards in paint order, then the ScreenGuis, in order_.
    // The children list changes only by the nodes that leave or arrive, since
    // taking a node out drops the focus and any press inside it.
    void restack();
    // sync's work under the read lock.
    void syncTree();

    engine_core::Engine& engine_;
    engine_core::DataModel& game_;
    std::shared_ptr<GuiInput> input_;
    // Builds and keeps the nodes for every ScreenGui and BillboardGui the layer draws.
    std::unique_ptr<GuiTree> tree_;
    std::vector<Placement> placements_;
    std::vector<std::shared_ptr<jadefx::Node>> screens_;
    // The children in paint order, as restack last put them. Held, so a
    // pointer here is never left dangling.
    std::vector<std::shared_ptr<jadefx::Node>> order_;
    // The children list's nodes, sorted by address, as restack last set it.
    std::vector<jadefx::Node*> members_;
    std::optional<float> cursorDepth_;
    SceneDepth sceneDepth_;
    // The service's CSS at the last sync, so it is parsed again only when it changes.
    std::string css_;
    // The root's GUID at the last sync. Another place starts the nodes over.
    std::string placeGuid_;
};

}  // namespace runner
