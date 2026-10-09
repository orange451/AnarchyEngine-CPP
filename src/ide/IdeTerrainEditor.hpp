#pragma once

#include "ChangeFlag.hpp"
#include "IdeExplorer.hpp"
#include "IdePane.hpp"
#include "TerrainMaterials.hpp"

#include "DataModel.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ide {

// What the Configure Terrain tab asks of the studio. Each write is one undo
// step, a replacement of voxels included.
struct TerrainEditorHost {
    // Adds a TerrainMaterial to terrain holding material, or none for 0, and
    // reports the new one, or why none was made, through result.
    std::function<void(engine_core::InstanceId terrain, engine_core::InstanceId material,
                       std::shared_ptr<InsertResult> result)>
        add;
    // Points entry at material, or at nothing when material is 0.
    std::function<void(engine_core::InstanceId entry, engine_core::InstanceId material)> set_material;
    std::function<void(engine_core::InstanceId id, std::string name)> rename;
    // Removes entry, first replacing its voxels when choice says so.
    std::function<void(engine_core::InstanceId entry, RemoveChoice choice)> remove;
    // Voxels on the unassigned Id from take Id to (0: the default) across terrain.
    std::function<void(engine_core::InstanceId terrain, int from, int to)> replace_unassigned;
    // A short message for the person, such as why an add did nothing.
    std::function<void(std::string text)> notice;
    // Task 5: the header's "· N MB textures" figure, from
    // TerrainTextures::memory_bytes, and the published set's revision (so
    // refresh() notices a new build even when nothing else about the
    // Terrain changed). Both empty in a test that does not wire an Engine
    // in: the header then shows no textures figure.
    std::function<std::size_t(engine_core::InstanceId terrain)> texture_memory_bytes;
    std::function<std::uint64_t(engine_core::InstanceId terrain)> texture_revision;
};

class AssetPicker;

// Configures one Terrain: which Material each voxel Id draws and collides as.
//
// The header names the Terrain, counts its TerrainMaterials out of the 255 it
// may hold, and holds Add Material. Each TerrainMaterial is a card in a grid
// that wraps to the page's width, in Id order: its Id, its Name, an In use
// badge while some voxel has its Id, and a slot for its Material. Clicking the
// slot opens a picker of every Material in Assets; None clears it. The New
// Material tile after the cards adds one too, and opens its Material picker.
//
// Voxels whose Id no TerrainMaterial holds draw as the default material. A row
// under the header lists those Ids, and its Replace… moves their voxels onto
// one of this Terrain's materials, or onto the default.
//
// Removing a TerrainMaterial no voxel uses happens at once. Removing one in
// use asks first: Replace… moves its voxels onto another material, Keep Cells
// leaves them on its Id, drawing as the default, and Cancel does nothing.
//
// A click on a card selects its TerrainMaterial, which Properties then shows.
// Delete removes the selected one, and F2 or Enter, a double-click on the
// name, or Rename on the card's menu renames in place. The tab shows the
// Terrain's name. A deleted Terrain leaves a note in the page.
class IdeTerrainEditor : public IdePane {
public:
    IdeTerrainEditor(engine_core::DataModel& world, engine_core::InstanceId terrain, TerrainEditorHost host = {});
    ~IdeTerrainEditor() override;

    engine_core::InstanceId terrain() const { return terrain_; }
    // The Terrain as last read.
    const TerrainMaterialsView& view() const { return view_; }

    // Adds a TerrainMaterial and opens its Material picker. What Add Material does.
    void addMaterial();
    // Removes entry, asking first when some voxel uses its Id.
    void requestRemove(engine_core::InstanceId entry);
    // Opens entry's Material picker.
    void openMaterialPicker(engine_core::InstanceId entry);
    void closePicker();
    bool pickerOpen() const;
    // Starts renaming entry in place.
    void beginRename(engine_core::InstanceId entry);

    // For tests: the widgets a person would click.
    jadefx::Node* cardNode(engine_core::InstanceId entry) const;
    jadefx::Node* materialSlot(engine_core::InstanceId entry) const;
    jadefx::Node* deleteButton(engine_core::InstanceId entry) const;
    // Null while no voxel uses entry's Id.
    jadefx::Node* inUseBadge(engine_core::InstanceId entry) const;
    jadefx::Node* addTile() const;
    jadefx::Label* counter() const;
    // Null while every Id voxels use has a TerrainMaterial.
    jadefx::Node* unassignedRow() const;
    // Null while the Terrain exists.
    jadefx::Node* goneNote() const;
    jadefx::TextField* renameField(engine_core::InstanceId entry) const;
    // Null when not asking.
    jadefx::Alert* removeAlert() const;
    // The open picker's row for choice; 0 is Default, or None in a Material picker.
    jadefx::Node* pickerRow(engine_core::InstanceId choice) const;

protected:
    void layoutChildren() override;
    void handleKey(jadefx::KeyEvent& event) override;

private:
    struct Card;

    // Reads the Terrain when the tree, a watched instance, or the voxels moved.
    void refresh();
    // Makes and drops cards to match view_, and fills each from its view.
    void sync_cards();
    std::shared_ptr<Card> make_card(const TerrainMaterialView& view);
    void fill_card(Card& card, const TerrainMaterialView& view);
    void show_header();
    void show_unassigned();
    void show_selection();
    void clicked_card(engine_core::InstanceId entry);
    void show_card_menu(engine_core::InstanceId entry, double x, double y);
    void finish_rename(bool apply);
    // Opens, under anchor, a picker of this Terrain's TerrainMaterials but
    // except, and Default. pick runs with the chosen material Id, 0 for Default.
    void open_id_picker(jadefx::Node& anchor, engine_core::InstanceId except, std::function<void(int)> pick);
    // The selected TerrainMaterials this Terrain has.
    std::vector<engine_core::InstanceId> selected_entries() const;
    const TerrainMaterialView* entry_view(engine_core::InstanceId entry) const;
    // The Terrain's voxel revision, or 0 when it is gone. The caller holds a read lock.
    std::uint64_t voxel_revision() const;
    // The host's texture_revision(terrain_), or 0 with no host callback.
    std::uint64_t texture_revision() const;

    engine_core::DataModel& world_;
    engine_core::InstanceId terrain_;
    TerrainEditorHost host_;

    TerrainMaterialsView view_;
    std::uint64_t seen_tree_ = ~std::uint64_t{0};
    std::uint64_t seen_texture_revision_ = ~std::uint64_t{0};
    ChangeFlag edited_;
    std::uint64_t watch_ = 0;
    std::vector<engine_core::InstanceId> watched_;
    std::uint64_t selection_seen_ = ~std::uint64_t{0};
    std::vector<engine_core::InstanceId> selected_;

    std::shared_ptr<jadefx::Label> title_;
    std::shared_ptr<jadefx::Label> subtitle_;
    std::shared_ptr<jadefx::Button> add_button_;
    // The header, then the unassigned row while there is one.
    std::shared_ptr<jadefx::VBox> top_;
    std::shared_ptr<jadefx::HBox> unassigned_;
    std::shared_ptr<jadefx::Label> unassigned_text_;
    std::shared_ptr<jadefx::Button> unassigned_replace_;
    std::shared_ptr<jadefx::ScrollPane> scroll_;
    std::shared_ptr<jadefx::FlowPane> grid_;
    std::shared_ptr<jadefx::Node> new_tile_;
    std::shared_ptr<jadefx::Node> gone_;
    std::vector<std::shared_ptr<Card>> cards_;

    std::shared_ptr<AssetPicker> picker_;
    std::shared_ptr<jadefx::Menu> menu_;
    // The question asked before removing a TerrainMaterial in use.
    std::shared_ptr<jadefx::Alert> alert_;
    // The TerrainMaterial alert_ asks about, while it asks.
    engine_core::InstanceId asking_ = 0;

    // An Add Material on its way through the simulation thread.
    std::shared_ptr<InsertResult> pending_add_;
    // A Material picker to open once the card it belongs to is on screen.
    struct PendingPicker {
        engine_core::InstanceId entry = 0;
        // Frames spent waiting for the slot to be laid out in view.
        int waited = 0;
    };
    std::optional<PendingPicker> pending_picker_;

    engine_core::InstanceId renaming_ = 0;
    // A card or the New Material tile took the click that bubbles on to empty space.
    bool took_click_ = false;
};

}  // namespace ide
