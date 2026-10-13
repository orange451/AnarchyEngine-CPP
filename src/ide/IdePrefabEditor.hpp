#pragma once

#include "ChangeFlag.hpp"
#include "IdeExplorer.hpp"
#include "IdePane.hpp"
#include "PrefabLods.hpp"
#include "PrefabModels.hpp"

#include "DataModel.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ide {

// What the Prefab editor asks of the studio. Each write is one undo step.
struct PrefabEditorHost {
    // Adds a Model to holder (the Prefab, or one of its LodLayers) holding
    // mesh and material, each 0 for none, and reports the new Model, or why
    // none was made, through result.
    std::function<void(engine_core::InstanceId holder, engine_core::InstanceId mesh, engine_core::InstanceId material,
                       std::shared_ptr<InsertResult> result)>
        add_model;
    // Adds a LodLayer to prefab (run_add_lod), reported through result.
    std::function<void(engine_core::InstanceId prefab, std::shared_ptr<InsertResult> result)> add_lod;
    // Each sets one of a LodLayer's properties (run_set_lod_*).
    std::function<void(engine_core::InstanceId layer, double distance)> set_lod_distance;
    std::function<void(engine_core::InstanceId layer, engine_core::LodKind kind)> set_lod_kind;
    // texture 0 clears it.
    std::function<void(engine_core::InstanceId layer, engine_core::InstanceId texture)> set_lod_texture;
    std::function<void(engine_core::InstanceId layer, float width, float height)> set_lod_size;
    // Makes the card baked for layer, saved at path under the resources
    // folder, its Texture and Size (run_set_lod_card).
    std::function<void(engine_core::InstanceId layer, std::string path, float width, float height)> set_lod_card;
    // Points model's part at target, or at nothing when target is 0.
    std::function<void(engine_core::InstanceId model, ModelPart part, engine_core::InstanceId target)> set_part;
    std::function<void(engine_core::InstanceId id, std::string name)> rename;
    // Deletes the Models.
    std::function<void(const std::vector<engine_core::InstanceId>& ids)> remove;
    // A short message for the person, such as why a drop did nothing.
    std::function<void(std::string text)> notice;
};

class AssetPicker;
class BillboardBaker;

// Edits one Prefab: its Models, and the Mesh and Material each one pairs.
//
// The header names the Prefab, sums up its Models, and holds Add Model. Each
// Model is a card in a grid that wraps to the page's width: its name, whether
// it is ready to draw, and a slot for its Mesh and one for its Material. A
// slot shows the asset it holds, or asks for one. Clicking a slot opens a
// picker of every Mesh or Material in Assets: typing filters it, Up and Down
// move, Enter picks, and Escape closes. None clears the slot, and × on a
// filled slot does too.
//
// Add Model, or the New Model tile after the cards, adds a Model and opens
// its Mesh picker; picking a Mesh then opens its Material picker, so two picks
// make a whole Model. A Model named after its Mesh, or still called Model,
// takes the name of the Mesh it is given.
//
// Assets drag in from the Assets pane: onto a slot to fill it, onto a card to
// fill the slot that fits, or onto empty space to add a Model holding them.
// What is under the pointer is outlined while it would take the drop.
//
// A click on a card selects its Model, which Properties then shows; Ctrl (Cmd)
// adds or removes one. Delete removes the selected Models, and F2 or Enter,
// a double-click on the name, or Rename on the card's menu renames in place.
// The tab shows the Prefab's name. A deleted Prefab leaves a note in the page.
//
// Below the Models, which are what it draws up close, the LOD section lists
// its LodLayers nearest first, each a card: the distance it takes over from,
// typed in place, and what it draws from there on. Model swaps in its own
// Models, cards as above with their own New Model tile; Billboard draws a
// Texture on a card turned to face the camera, its slot picked or dropped
// on like a Model's, with its width and height, or baked: Bake card draws
// the Prefab's own Models onto a card, saves it in the resources folder, and
// makes it the layer's Texture and Size. None draws nothing. Add LOD adds one
// farther out. A click on a layer's card selects it, and Delete removes it
// with its Models. Every change is one undo step.
class IdePrefabEditor : public IdePane {
public:
    IdePrefabEditor(engine_core::DataModel& world, engine_core::InstanceId prefab, PrefabEditorHost host = {});
    ~IdePrefabEditor() override;

    engine_core::InstanceId prefab() const { return prefab_; }
    // The Models as last read.
    const std::vector<ModelView>& models() const { return models_; }
    // The LodLayers as last read, nearest first.
    const std::vector<LodView>& lods() const { return lods_; }

    // Adds a Model to holder (the Prefab, or 0 for it, or one of its
    // LodLayers) and opens its Mesh picker. What Add Model does.
    void addModel(engine_core::InstanceId holder = 0);
    // Adds a LodLayer past the farthest. What Add LOD does.
    void addLod();
    // Opens a Billboard layer's Texture picker.
    void openTexturePicker(engine_core::InstanceId layer);
    // Bakes a card for a Billboard layer from its Prefab's Models, the next
    // time the editor paints (it needs GL). What Bake card does.
    void bakeCard(engine_core::InstanceId layer);
    // The layer a bake is waiting to draw, or 0.
    engine_core::InstanceId baking() const { return baking_; }
    // Opens model's picker for part. guided chains to the Material picker after a Mesh pick.
    void openPicker(engine_core::InstanceId model, ModelPart part, bool guided = false);
    void closePicker();
    bool pickerOpen() const;
    // Starts renaming model in place.
    void beginRename(engine_core::InstanceId model);
    // Adds a Model to holder (0 for the Prefab) holding what the dragged ids
    // hold, or fills model's slots with them, or just part's slot. What a drop
    // does. False, with a notice, when there is no Mesh or Material among them.
    bool dropOnto(const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId model = 0,
                  const ModelPart* part = nullptr, engine_core::InstanceId holder = 0);
    // Sets a Billboard layer's Texture to the first Texture dragged. False,
    // with a notice, when there is none.
    bool dropTexture(const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId layer);

    // For tests: the widgets a person would click.
    jadefx::Node* cardNode(engine_core::InstanceId model) const;
    jadefx::Node* slotNode(engine_core::InstanceId model, ModelPart part) const;
    jadefx::Node* slotClearNode(engine_core::InstanceId model, ModelPart part) const;
    jadefx::Node* addButton() const;
    jadefx::Node* emptyState() const;
    // The open picker's search field and its row for asset, or None's row for 0.
    jadefx::TextField* pickerField() const;
    jadefx::Node* pickerRow(engine_core::InstanceId asset) const;
    jadefx::TextField* renameField(engine_core::InstanceId model) const;
    jadefx::Node* addLodButton() const;
    jadefx::Node* lodCard(engine_core::InstanceId layer) const;
    jadefx::TextField* lodDistanceField(engine_core::InstanceId layer) const;
    jadefx::Node* lodKindButton(engine_core::InstanceId layer, engine_core::LodKind kind) const;
    jadefx::Node* lodTextureSlot(engine_core::InstanceId layer) const;
    // axis 0 is the width, 1 the height.
    jadefx::TextField* lodSizeField(engine_core::InstanceId layer, int axis) const;
    jadefx::Node* lodNewModelTile(engine_core::InstanceId layer) const;
    jadefx::Node* lodBakeButton(engine_core::InstanceId layer) const;

protected:
    void layoutChildren() override;
    void handleKey(jadefx::KeyEvent& event) override;
    // Draws a waiting bake: GL is current while the pane paints.
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void sceneChanged(jadefx::Scene* previous) override;

private:
    struct Card;
    struct LodCard;

    // Reads the Prefab and its Models when the tree or a watched property moved.
    void refresh();
    // Makes and drops cards to match models_, and fills each from its view.
    void sync_cards();
    std::shared_ptr<Card> make_card(const ModelView& view);
    void fill_card(Card& card, const ModelView& view);
    void fill_slot(Card& card, ModelPart part, const PartView& view);
    // A New Model tile that adds to holder (0 for the Prefab).
    std::shared_ptr<jadefx::Node> make_new_tile(engine_core::InstanceId holder);
    // Lays out grid as models' cards, then tile, keeping each card still
    // shown and putting every card it shows in kept.
    void sync_grid(jadefx::FlowPane& grid, const std::vector<ModelView>& models, const std::shared_ptr<jadefx::Node>& tile,
                   std::vector<std::shared_ptr<Card>>& kept);
    std::shared_ptr<LodCard> make_lod_card(const LodView& view);
    void fill_lod_card(LodCard& card, const LodView& view, std::vector<std::shared_ptr<Card>>& kept);
    void clicked_lod(engine_core::InstanceId layer, const jadefx::MouseEvent& event);
    // Scrolls the page just enough to show node.
    void reveal(const jadefx::Node& node);
    // Draws, saves, and hands on the card baking_ waits for, or tries again next paint.
    void bake_pending();
    // Ends the bake, saying why when there is a why.
    void end_bake(const std::string& why);
    void show_header();
    void show_selection();
    void clicked_card(engine_core::InstanceId model, const jadefx::MouseEvent& event);
    void show_card_menu(engine_core::InstanceId model, double x, double y);
    void finish_rename(bool apply);
    // What the picker does with a pick: sets the part, and in a guided run opens the Material picker next.
    void picked(engine_core::InstanceId model, ModelPart part, engine_core::InstanceId asset, bool guided);
    // The selected Models this Prefab has.
    std::vector<engine_core::InstanceId> selected_models() const;
    // Wires drops onto node: a card when model is set, one slot when part is
    // too, else empty space, which adds a Model to holder (0 for the Prefab).
    void accept_drops(jadefx::Node& node, engine_core::InstanceId model, const ModelPart* part,
                      engine_core::InstanceId holder = 0);
    // Wires Texture drops onto a Billboard layer's slot.
    void accept_texture_drops(jadefx::Node& node, engine_core::InstanceId layer);
    // Outlines node as the drop target, and only node; null clears.
    void mark_drop(jadefx::Node* node);
    std::vector<engine_core::InstanceId> dragged(const jadefx::DragEvent& event) const;
    // A Model of the Prefab's or of one of its layers.
    const ModelView* model_view(engine_core::InstanceId model) const;
    const LodView* lod_view(engine_core::InstanceId layer) const;
    const LodCard* lod_card(engine_core::InstanceId layer) const;

    engine_core::DataModel& world_;
    engine_core::InstanceId prefab_;
    PrefabEditorHost host_;

    std::string prefab_name_;
    bool prefab_alive_ = true;
    std::vector<ModelView> models_;
    std::vector<LodView> lods_;
    std::uint64_t seen_tree_ = ~std::uint64_t{0};
    ChangeFlag edited_;
    std::uint64_t watch_ = 0;
    std::vector<engine_core::InstanceId> watched_;
    std::uint64_t selection_seen_ = ~std::uint64_t{0};
    std::vector<engine_core::InstanceId> selected_;

    std::shared_ptr<jadefx::Label> title_;
    std::shared_ptr<jadefx::Label> subtitle_;
    std::shared_ptr<jadefx::Button> add_button_;
    std::shared_ptr<jadefx::Button> add_lod_button_;
    std::shared_ptr<jadefx::ScrollPane> scroll_;
    // The page: the base layer's heading (while there are LODs) and grid, then the LOD section.
    std::shared_ptr<jadefx::VBox> page_;
    std::shared_ptr<jadefx::HBox> base_heading_;
    std::shared_ptr<jadefx::Label> base_range_;
    std::shared_ptr<jadefx::FlowPane> grid_;
    std::shared_ptr<jadefx::HBox> lod_heading_;
    std::shared_ptr<jadefx::VBox> lod_section_;
    std::vector<std::shared_ptr<LodCard>> lod_cards_;
    std::shared_ptr<jadefx::Node> new_tile_;
    std::shared_ptr<jadefx::Node> empty_;
    std::shared_ptr<jadefx::Node> gone_;
    std::vector<std::shared_ptr<Card>> cards_;

    std::shared_ptr<AssetPicker> picker_;
    std::shared_ptr<jadefx::Menu> menu_;
    jadefx::Node* drop_mark_ = nullptr;

    // An Add Model on its way through the simulation thread.
    std::shared_ptr<InsertResult> pending_add_;
    // An Add LOD on its way, and a layer's card to bring into view once laid out.
    std::shared_ptr<InsertResult> pending_lod_;
    engine_core::InstanceId reveal_lod_ = 0;
    // Frames spent waiting for that card.
    int reveal_waited_ = 0;
    // A Bake card waiting for the next paint, and the paints it has waited for textures.
    engine_core::InstanceId baking_ = 0;
    int bake_waits_ = 0;
    // False when a bake ended while painting, until the cards show it.
    bool bake_shown_ = true;
    std::unique_ptr<BillboardBaker> baker_;
    // A picker to open once the card it belongs to is on screen.
    struct PendingPicker {
        engine_core::InstanceId model = 0;
        ModelPart part = ModelPart::Mesh;
        bool guided = false;
        // Frames spent waiting for the slot to be laid out in view.
        int waited = 0;
    };
    std::optional<PendingPicker> pending_picker_;

    engine_core::InstanceId renaming_ = 0;
    // A card or the New Model tile took the click that bubbles on to empty space.
    bool took_click_ = false;
    // The last card clicked, where a click with Shift would extend from.
    engine_core::InstanceId anchor_ = 0;
};

}  // namespace ide
