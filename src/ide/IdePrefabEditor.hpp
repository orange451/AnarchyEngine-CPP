#pragma once

#include "ChangeFlag.hpp"
#include "IdeExplorer.hpp"
#include "IdePane.hpp"
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
    // Adds a Model to prefab holding mesh and material, each 0 for none, and
    // reports the new Model, or why none was made, through result.
    std::function<void(engine_core::InstanceId prefab, engine_core::InstanceId mesh, engine_core::InstanceId material,
                       std::shared_ptr<InsertResult> result)>
        add_model;
    // Points model's part at target, or at nothing when target is 0.
    std::function<void(engine_core::InstanceId model, ModelPart part, engine_core::InstanceId target)> set_part;
    std::function<void(engine_core::InstanceId id, std::string name)> rename;
    // Deletes the Models.
    std::function<void(const std::vector<engine_core::InstanceId>& ids)> remove;
    // A short message for the person, such as why a drop did nothing.
    std::function<void(std::string text)> notice;
};

class PartPicker;

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
class IdePrefabEditor : public IdePane {
public:
    IdePrefabEditor(engine_core::DataModel& world, engine_core::InstanceId prefab, PrefabEditorHost host = {});
    ~IdePrefabEditor() override;

    engine_core::InstanceId prefab() const { return prefab_; }
    // The Models as last read.
    const std::vector<ModelView>& models() const { return models_; }

    // Adds a Model and opens its Mesh picker. What Add Model does.
    void addModel();
    // Opens model's picker for part. guided chains to the Material picker after a Mesh pick.
    void openPicker(engine_core::InstanceId model, ModelPart part, bool guided = false);
    void closePicker();
    bool pickerOpen() const;
    // Starts renaming model in place.
    void beginRename(engine_core::InstanceId model);
    // Adds a Model holding what the dragged ids hold, or fills model's slots
    // with them, or just part's slot. What a drop does. False, with a notice,
    // when there is no Mesh or Material among them.
    bool dropOnto(const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId model = 0,
                  const ModelPart* part = nullptr);

    // For tests: the widgets a person would click.
    jadefx::Node* cardNode(engine_core::InstanceId model) const;
    jadefx::Node* slotNode(engine_core::InstanceId model, ModelPart part) const;
    jadefx::Node* slotClearNode(engine_core::InstanceId model, ModelPart part) const;
    jadefx::Node* addButton() const;
    jadefx::Node* newModelTile() const;
    jadefx::Node* emptyState() const;
    // The open picker's search field and its row for asset, or None's row for 0.
    jadefx::TextField* pickerField() const;
    jadefx::Node* pickerRow(engine_core::InstanceId asset) const;
    jadefx::TextField* renameField(engine_core::InstanceId model) const;

protected:
    void layoutChildren() override;
    void handleKey(jadefx::KeyEvent& event) override;

private:
    struct Card;

    // Reads the Prefab and its Models when the tree or a watched property moved.
    void refresh();
    // Makes and drops cards to match models_, and fills each from its view.
    void sync_cards();
    std::shared_ptr<Card> make_card(const ModelView& view);
    void fill_card(Card& card, const ModelView& view);
    void fill_slot(Card& card, ModelPart part, const PartView& view);
    void show_header();
    void show_selection();
    void clicked_card(engine_core::InstanceId model, const jadefx::MouseEvent& event);
    void show_card_menu(engine_core::InstanceId model, double x, double y);
    void finish_rename(bool apply);
    // What the picker does with a pick: sets the part, and in a guided run opens the Material picker next.
    void picked(engine_core::InstanceId model, ModelPart part, engine_core::InstanceId asset, bool guided);
    // The selected Models this Prefab has.
    std::vector<engine_core::InstanceId> selected_models() const;
    // Wires drops onto node: a card when model is set, one slot when part is too, else empty space.
    void accept_drops(jadefx::Node& node, engine_core::InstanceId model, const ModelPart* part);
    // Outlines node as the drop target, and only node; null clears.
    void mark_drop(jadefx::Node* node);
    std::vector<engine_core::InstanceId> dragged(const jadefx::DragEvent& event) const;
    const ModelView* model_view(engine_core::InstanceId model) const;

    engine_core::DataModel& world_;
    engine_core::InstanceId prefab_;
    PrefabEditorHost host_;

    std::string prefab_name_;
    bool prefab_alive_ = true;
    std::vector<ModelView> models_;
    std::uint64_t seen_tree_ = ~std::uint64_t{0};
    ChangeFlag edited_;
    std::uint64_t watch_ = 0;
    std::vector<engine_core::InstanceId> watched_;
    std::uint64_t selection_seen_ = ~std::uint64_t{0};
    std::vector<engine_core::InstanceId> selected_;

    std::shared_ptr<jadefx::Label> title_;
    std::shared_ptr<jadefx::Label> subtitle_;
    std::shared_ptr<jadefx::Button> add_button_;
    std::shared_ptr<jadefx::ScrollPane> scroll_;
    std::shared_ptr<jadefx::FlowPane> grid_;
    std::shared_ptr<jadefx::Node> new_tile_;
    std::shared_ptr<jadefx::Node> empty_;
    std::shared_ptr<jadefx::Node> gone_;
    std::vector<std::shared_ptr<Card>> cards_;

    std::shared_ptr<PartPicker> picker_;
    std::shared_ptr<jadefx::Menu> menu_;
    jadefx::Node* drop_mark_ = nullptr;

    // An Add Model on its way through the simulation thread.
    std::shared_ptr<InsertResult> pending_add_;
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
