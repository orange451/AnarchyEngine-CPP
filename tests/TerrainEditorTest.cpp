#include "ide/IdeTerrainEditor.hpp"
#include "ide/ScopedRecording.hpp"
#include "ide/TerrainMaterials.hpp"
#include "SelectionService.hpp"

#include "AssetInstances.hpp"
#include "Game.hpp"
#include "ScriptRuntime.hpp"
#include "Terrain.hpp"
#include "TerrainMaterial.hpp"
#include "TerrainTextures.hpp"
#include "terrain/VoxelVolume.hpp"
#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The Configure Terrain tab in a headless scene, and the data behind it: what
// read_terrain_materials shows, and what add/set/remove/replace_unassigned do
// to a Terrain's TerrainMaterials and voxels.
namespace {

using engine_core::InstanceId;
namespace terrain = engine_core::terrain;

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

// Marks this thread as the simulation thread while it lives, as Terrain's
// setters and TerrainMaterial's setters need.
struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

terrain::Shape Ball(float x, float y, float z, float radius) {
    terrain::Shape shape;
    shape.kind = terrain::Shape::Kind::Ball;
    shape.center = engine_core::Vec3{x, y, z};
    shape.radius = radius;
    return shape;
}

// A place whose Materials hold Rock and Grass, and whose Workspace holds one
// empty Terrain. The host writes straight to the game, standing in for the
// simulation thread.
struct Rig {
    SimRole role;
    engine_core::ScriptRuntime runtime;
    engine_core::Game game;
    InstanceId rock = 0;
    InstanceId grass = 0;
    InstanceId terrain_id = 0;

    Rig() {
        rock = make<engine_core::Material>("Rock", game.service("Materials"));
        grass = make<engine_core::Material>("Grass", game.service("Materials"));
        engine_core::Terrain& placed = game.create<engine_core::Terrain>();
        terrain_id = placed.id();
        game.set_parent(terrain_id, game.scene_service("Workspace"));
    }

    template <typename T>
    InstanceId make(const char* name, InstanceId parent) {
        T& object = game.create<T>();
        game.set_name(object.id(), name);
        game.set_parent(object.id(), parent);
        return object.id();
    }

    engine_core::Terrain& terrain() {
        return *dynamic_cast<engine_core::Terrain*>(game.instance(terrain_id));
    }

    // Through Terrain::edit_volume, as a real sculpt does.
    void fill_ball(float x, float y, float z, float r, int id) {
        terrain().edit_volume([&](terrain::VoxelVolume& volume) {
            return volume.fill(Ball(x, y, z, r), static_cast<std::uint8_t>(id));
        });
    }

    int cell_id(int x, int y, int z) {
        return terrain().volume().cell(terrain::CellCoord{x, y, z}).material;
    }
};

void data_helpers() {
    Rig rig;
    std::string error;
    const InstanceId rock = ide::add_terrain_material(rig.game, rig.terrain_id, rig.rock, error);
    const InstanceId grass = ide::add_terrain_material(rig.game, rig.terrain_id, rig.grass, error);
    auto view = ide::read_terrain_materials(rig.game, rig.terrain_id);
    Expect(view.alive && view.materials.size() == 2, "both are listed");
    Expect(view.materials[0].material_id == 1 && view.materials[0].name == "Rock",
           "Rock is Id 1, named after its Material");
    Expect(!view.materials[0].in_use, "nothing uses it yet");

    rig.fill_ball(0, 0, 0, 5, 1);     // Terrain::edit_volume fill with Id 1
    rig.fill_ball(100, 0, 0, 5, 9);   // an Id no TerrainMaterial holds
    view = ide::read_terrain_materials(rig.game, rig.terrain_id);
    Expect(view.materials[0].in_use && !view.materials[1].in_use, "In use follows the voxels");
    Expect(view.unassigned_in_use == std::vector<int>{9}, "Id 9 is used but unassigned");

    Expect(!ide::set_terrain_material(rig.game, grass, rig.rock), "Set points it at another Material");
    Expect(ide::read_terrain_materials(rig.game, rig.terrain_id).materials[1].material == rig.rock, "and it shows");

    Expect(!ide::remove_terrain_material(rig.game, rock, {ide::RemoveChoice::Kind::Replace, 2}),
           "Replace then remove");
    Expect(rig.cell_id(0, 0, 0) == 2, "Rock's voxels became Grass's Id");
    Expect(!ide::replace_unassigned(rig.game, rig.terrain_id, 9, 0), "unassigned Id 9 to the default");
    Expect(rig.cell_id(100, 0, 0) == 0, "and it is the default now");

    const InstanceId again = ide::add_terrain_material(rig.game, rig.terrain_id, 0, error);
    rig.fill_ball(200, 0, 0, 5, 1);
    Expect(!ide::remove_terrain_material(rig.game, again, {}), "Keep Cells removes only the TerrainMaterial");
    Expect(rig.cell_id(200, 0, 0) == 1, "its voxels keep Id 1");
}

constexpr double kWidth = 960;
constexpr double kHeight = 640;

// The Rig with the tab open on its Terrain. The host writes straight to the
// game, standing in for the simulation thread, and notes what it was asked.
struct PaneRig : Rig {
    std::vector<std::string> notices;
    std::vector<std::pair<InstanceId, std::string>> renames;
    std::vector<InstanceId> removed;
    std::vector<ide::RemoveChoice> remove_choices;
    std::vector<std::pair<int, int>> replaced;
    int adds = 0;
    int sets = 0;
    std::shared_ptr<ide::IdeTerrainEditor> editor;
    std::shared_ptr<jadefx::Scene> scene;
    double time = 0;

    PaneRig() {
        ide::TerrainEditorHost host;
        host.add = [this](InstanceId terrain, InstanceId material, std::shared_ptr<ide::InsertResult> result) {
            ++adds;
            std::string error;
            result->id = ide::add_terrain_material(game, terrain, material, error);
            result->error = error;
            result->done = true;
        };
        host.set_material = [this](InstanceId entry, InstanceId material) {
            ++sets;
            if (std::optional<std::string> error = ide::set_terrain_material(game, entry, material)) {
                notices.push_back(*error);
            }
        };
        host.rename = [this](InstanceId id, std::string name) {
            game.set_name(id, name);
            renames.emplace_back(id, std::move(name));
        };
        host.remove = [this](InstanceId entry, ide::RemoveChoice choice) {
            removed.push_back(entry);
            remove_choices.push_back(choice);
            if (std::optional<std::string> error = ide::remove_terrain_material(game, entry, choice)) {
                notices.push_back(*error);
            }
        };
        host.replace_unassigned = [this](InstanceId terrain, int from, int to) {
            replaced.emplace_back(from, to);
            if (std::optional<std::string> error = ide::replace_unassigned(game, terrain, from, to)) {
                notices.push_back(*error);
            }
        };
        host.notice = [this](std::string text) { notices.push_back(std::move(text)); };
        editor = jadefx::make<ide::IdeTerrainEditor>(game, terrain_id, std::move(host));
        editor->setPrefWidthRatio(1);
        editor->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(editor, kWidth, kHeight);
        frames(2);
    }

    void frames(int count = 1) {
        for (int i = 0; i < count; ++i) {
            scene->layout(kWidth, kHeight, time);
            time += 0.05;
        }
    }

    // Presses and releases the middle of node. JadeFX counts presses on one
    // spot within 0.4 s of real time as one run, so a move away comes first.
    void click(jadefx::Node* node) {
        Expect(node != nullptr, "the node to click is on screen");
        if (node == nullptr) {
            return;
        }
        click_at(node->getAbsoluteX() + node->getWidth() * 0.5, node->getAbsoluteY() + node->getHeight() * 0.5);
    }

    void click_at(double x, double y) {
        scene->noteMove(x, y);
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
        frames();
    }

    // A card's top edge, away from its name, slot, and delete icon.
    void click_card(InstanceId entry) {
        jadefx::Node* card = editor->cardNode(entry);
        Expect(card != nullptr, "the card to click is on screen");
        if (card != nullptr) {
            click_at(card->getAbsoluteX() + card->getWidth() * 0.5, card->getAbsoluteY() + 6);
        }
    }

    void key(int code) {
        scene->noteKey(code, true, false, 0);
        frames();
    }

    // An Alert's button, found by its element id as a person would see it.
    jadefx::Button* button(const char* id) { return dynamic_cast<jadefx::Button*>(scene->getElementById(id)); }

    void fire(const char* id) {
        jadefx::Button* found = button(id);
        Expect(found != nullptr, "the Alert's button is showing");
        if (found != nullptr) {
            found->fire();
        }
        frames();
    }

    const ide::TerrainMaterialView* view(InstanceId entry) const {
        for (const ide::TerrainMaterialView& v : editor->view().materials) {
            if (v.id == entry) {
                return &v;
            }
        }
        return nullptr;
    }

    std::string counter() const { return editor->counter() != nullptr ? editor->counter()->getText() : std::string(); }
};

// The Rig with the tab open on its Terrain, wired to a real TerrainTextures
// instead of PaneRig's unwired (no-op) texture callbacks -- so TL-T3 can
// prove the header genuinely reflects a background build landing, not just
// that a callback exists. Kept separate from PaneRig: wiring these two
// callbacks there would add a "N MB textures" suffix to every other TE*
// test's exact rig.counter() text (TE1, TE2, TE4, TE5, TE11).
struct TexturePaneRig : Rig {
    engine_core::TerrainTextures textures;
    std::shared_ptr<ide::IdeTerrainEditor> editor;
    std::shared_ptr<jadefx::Scene> scene;
    double time = 0;

    TexturePaneRig() {
        ide::TerrainEditorHost host;
        host.texture_memory_bytes = [this](InstanceId terrain) { return textures.memory_bytes(terrain); };
        host.texture_revision = [this](InstanceId terrain) -> std::uint64_t {
            const std::shared_ptr<const engine_core::TerrainTextureSet> set = textures.published(terrain);
            return set ? set->revision : 0;
        };
        editor = jadefx::make<ide::IdeTerrainEditor>(game, terrain_id, std::move(host));
        editor->setPrefWidthRatio(1);
        editor->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(editor, kWidth, kHeight);
        frames(2);
    }

    void frames(int count = 1) {
        for (int i = 0; i < count; ++i) {
            scene->layout(kWidth, kHeight, time);
            time += 0.05;
        }
    }

    // As sandbox/terrain_textures_tests.cpp's settle(): runs update()+
    // wait_idle() a few times so every build this update() queued has landed
    // and been drained by a following update().
    void settle() {
        for (int i = 0; i < 6; ++i) {
            textures.update(game);
            textures.wait_idle();
        }
        textures.update(game);
    }

    std::string counter() const { return editor->counter() != nullptr ? editor->counter()->getText() : std::string(); }
};

bool HasClass(const jadefx::Node* node, const char* name) {
    if (node == nullptr) {
        return false;
    }
    const auto& items = node->getClassList().items();
    return std::find(items.begin(), items.end(), name) != items.end();
}

// The text of the first Label under node with style_class.
std::string LabelText(jadefx::Node* node, const char* style_class) {
    if (node == nullptr) {
        return {};
    }
    for (jadefx::Node* found : node->getElementsByClassName(style_class)) {
        if (const auto* label = dynamic_cast<const jadefx::Label*>(found)) {
            return label->getText();
        }
    }
    return {};
}

void TE1_cards_and_counter() {
    PaneRig rig;
    std::string error;
    const InstanceId rock = ide::add_terrain_material(rig.game, rig.terrain_id, rig.rock, error);
    const InstanceId grass = ide::add_terrain_material(rig.game, rig.terrain_id, rig.grass, error);
    rig.frames(2);
    const auto& materials = rig.editor->view().materials;
    Expect(materials.size() == 2 && materials[0].id == rock && materials[1].id == grass, "TE1 two cards, in Id order");
    jadefx::Node* first = rig.editor->cardNode(rock);
    jadefx::Node* second = rig.editor->cardNode(grass);
    Expect(first != nullptr && second != nullptr, "TE1 each TerrainMaterial has a card");
    Expect(first != nullptr && second != nullptr && first->getAbsoluteX() < second->getAbsoluteX(),
           "TE1 Id 1's card comes first");
    Expect(LabelText(first, "te-id") == "1" && LabelText(second, "te-id") == "2", "TE1 each card shows its Id");
    Expect(LabelText(first, "te-name") == "Rock", "TE1 and its Name");
    Expect(rig.counter() == "2 / 255 materials", "TE1 the counter reads 2 / 255 materials");
    Expect(rig.editor->title() == rig.game.name(rig.terrain_id), "TE1 the tab shows the Terrain's name");
    Expect(rig.editor->inUseBadge(rock) == nullptr, "TE1 nothing is in use, so no badge");
    Expect(rig.editor->unassignedRow() == nullptr && rig.editor->goneNote() == nullptr,
           "TE1 no unassigned row and no gone note");
}

void TE2_add_tile() {
    PaneRig rig;
    std::string error;
    ide::add_terrain_material(rig.game, rig.terrain_id, rig.rock, error);
    ide::add_terrain_material(rig.game, rig.terrain_id, rig.grass, error);
    rig.frames(2);
    rig.click(rig.editor->addTile());
    rig.frames(2);
    Expect(rig.adds == 1 && rig.editor->view().materials.size() == 3, "TE2 New Material adds one");
    Expect(rig.counter() == "3 / 255 materials", "TE2 the counter reads 3 / 255");
    const InstanceId made = rig.editor->view().materials.empty() ? 0 : rig.editor->view().materials.back().id;
    Expect(rig.editor->cardNode(made) != nullptr, "TE2 its card shows");
    Expect(rig.game.selection().get() == std::vector<InstanceId>{made}, "TE2 the new TerrainMaterial is selected");
    Expect(rig.editor->pickerRow(rig.rock) != nullptr, "TE2 its Material picker opens");
}

void TE3_set_material_and_rename() {
    PaneRig rig;
    std::string error;
    const InstanceId entry = ide::add_terrain_material(rig.game, rig.terrain_id, rig.rock, error);
    rig.frames(2);
    rig.click(rig.editor->materialSlot(entry));
    rig.frames();
    Expect(rig.editor->pickerRow(rig.grass) != nullptr, "TE3 a click on the slot lists the Materials");
    rig.click(rig.editor->pickerRow(rig.grass));
    rig.frames();
    Expect(rig.sets == 1 && rig.view(entry) != nullptr && rig.view(entry)->material == rig.grass,
           "TE3 picking Grass sets it");
    rig.click(rig.editor->materialSlot(entry));
    rig.frames();
    rig.click(rig.editor->pickerRow(0));
    rig.frames();
    Expect(rig.view(entry) != nullptr && rig.view(entry)->material == 0, "TE3 None clears it");

    rig.click_card(entry);
    rig.key(jadefx::Key::F2);
    jadefx::TextField* field = rig.editor->renameField(entry);
    Expect(field != nullptr && field->isVisible() && field->getText() == rig.game.name(entry),
           "TE3 F2 renames in place");
    if (field != nullptr) {
        field->setText("Stone");
    }
    rig.key(jadefx::Key::Enter);
    rig.frames();
    Expect(!rig.renames.empty() && rig.renames.back() == std::pair<InstanceId, std::string>{entry, "Stone"},
           "TE3 Enter keeps the new name");
    Expect(rig.view(entry) != nullptr && rig.view(entry)->name == "Stone", "TE3 and the card shows it");
}

void TE4_remove_unused_asks_nothing() {
    PaneRig rig;
    std::string error;
    const InstanceId entry = ide::add_terrain_material(rig.game, rig.terrain_id, rig.rock, error);
    rig.frames(2);
    rig.click(rig.editor->deleteButton(entry));
    Expect(rig.editor->removeAlert() == nullptr, "TE4 an unused one is removed without asking");
    Expect(rig.removed == std::vector<InstanceId>{entry} && !rig.remove_choices.empty() &&
               rig.remove_choices.back().kind == ide::RemoveChoice::Kind::KeepCells,
           "TE4 it is removed at once");
    rig.frames();
    Expect(rig.editor->cardNode(entry) == nullptr && rig.counter() == "0 / 255 materials", "TE4 its card goes");
}

void TE5_remove_used_asks() {
    PaneRig rig;
    std::string error;
    const InstanceId rock = ide::add_terrain_material(rig.game, rig.terrain_id, rig.rock, error);
    const InstanceId grass = ide::add_terrain_material(rig.game, rig.terrain_id, rig.grass, error);
    rig.fill_ball(0, 0, 0, 5, 1);
    rig.fill_ball(100, 0, 0, 5, 2);
    rig.frames(2);
    Expect(rig.editor->inUseBadge(rock) != nullptr && rig.editor->inUseBadge(grass) != nullptr,
           "TE5 voxels put both in use");

    // Cancel changes nothing.
    rig.click(rig.editor->deleteButton(rock));
    jadefx::Alert* alert = rig.editor->removeAlert();
    Expect(alert != nullptr, "TE5 removing one in use asks first");
    Expect(alert != nullptr && alert->getHeaderText() == "Rock is used by voxels", "TE5 the Alert names it");
    Expect(alert != nullptr &&
               alert->getContentText() ==
                   "Replace them with another material, or keep them:\nthey draw as the default material until a "
                   "new\nmaterial takes Id 1.",
           "TE5 and says what each answer does");
    // The Alert draws that text in one Label, which cuts a line too long for it
    // short with an ellipsis. Every line must fit, or the warning goes unseen.
    rig.frames();
    const jadefx::Label* content = nullptr;
    // A button sits in its row, in the button bar, in the Alert's panel.
    jadefx::Node* panel_node = rig.button("terrain-remove-keep");
    for (int up = 0; up < 3 && panel_node != nullptr; ++up) {
        panel_node = panel_node->getParent();
    }
    if (auto* panel = dynamic_cast<jadefx::Pane*>(panel_node)) {
        for (const std::shared_ptr<jadefx::Node>& child : panel->getChildren()) {
            auto* label = dynamic_cast<const jadefx::Label*>(child.get());
            if (label != nullptr && alert != nullptr && label->getText() == alert->getContentText()) {
                content = label;
            }
        }
    }
    Expect(content != nullptr && content->displayedText() == content->getText(),
           "TE5 and shows all of it, not cut short");
    Expect(rig.button("terrain-remove-replace") != nullptr && rig.button("terrain-remove-keep") != nullptr,
           "TE5 it offers Replace and Keep Cells");
    rig.fire("terrain-remove-cancel");
    Expect(rig.editor->removeAlert() == nullptr, "TE5 Cancel closes it");
    Expect(rig.removed.empty() && rig.view(rock) != nullptr && rig.cell_id(0, 0, 0) == 1, "TE5 and changes nothing");

    // Keep Cells removes only the TerrainMaterial.
    rig.click(rig.editor->deleteButton(rock));
    rig.fire("terrain-remove-keep");
    Expect(rig.removed == std::vector<InstanceId>{rock} && !rig.remove_choices.empty() &&
               rig.remove_choices.back().kind == ide::RemoveChoice::Kind::KeepCells,
           "TE5 Keep Cells removes it");
    rig.frames();
    Expect(rig.editor->cardNode(rock) == nullptr, "TE5 its card goes");
    Expect(rig.cell_id(0, 0, 0) == 1, "TE5 its voxels keep the Id");

    // Replace… picks from the others and Default.
    rig.click(rig.editor->deleteButton(grass));
    rig.fire("terrain-remove-replace");
    rig.frames();
    Expect(rig.editor->pickerRow(0) != nullptr, "TE5 Replace… offers Default");
    Expect(rig.editor->pickerRow(grass) == nullptr, "TE5 but not the one being removed");
    rig.click(rig.editor->pickerRow(0));
    rig.frames();
    Expect(rig.removed.size() == 2 && rig.removed.back() == grass &&
               rig.remove_choices.back().kind == ide::RemoveChoice::Kind::Replace &&
               rig.remove_choices.back().replace_with == 0,
           "TE5 picking Default replaces with Id 0, then removes");
    Expect(rig.cell_id(100, 0, 0) == 0, "TE5 its voxels are the default now");
    Expect(rig.editor->cardNode(grass) == nullptr, "TE5 and its card goes");
}

void TE6_script_changes_follow() {
    PaneRig rig;
    std::string error;
    const InstanceId entry = ide::add_terrain_material(rig.game, rig.terrain_id, rig.rock, error);
    rig.frames();
    Expect(rig.editor->cardNode(entry) != nullptr && rig.counter() == "1 / 255 materials",
           "TE6 a TerrainMaterial a script adds shows");
    rig.game.set_name(entry, "Cliff");
    rig.frames();
    Expect(rig.view(entry) != nullptr && rig.view(entry)->name == "Cliff", "TE6 a rename from a script shows");
    Expect(!ide::set_terrain_material(rig.game, entry, rig.grass), "TE6 a script points it at Grass");
    rig.frames();
    Expect(rig.view(entry) != nullptr && rig.view(entry)->material == rig.grass, "TE6 and the card follows");
    rig.game.destroy_tree(entry);
    rig.frames();
    Expect(rig.editor->cardNode(entry) == nullptr && rig.counter() == "0 / 255 materials",
           "TE6 one a script destroys goes");
}

void TE7_deleted_terrain() {
    PaneRig rig;
    Expect(rig.editor->goneNote() == nullptr, "TE7 a live Terrain shows no gone note");
    rig.game.destroy_tree(rig.terrain_id);
    rig.frames();
    Expect(rig.editor->goneNote() != nullptr, "TE7 a deleted Terrain shows the gone note");
    Expect(LabelText(rig.editor->goneNote(), "te-empty-title") == "This Terrain no longer exists",
           "TE7 which says so");
    rig.click(rig.editor->goneNote());
    rig.editor->addMaterial();
    rig.frames(2);
    Expect(rig.adds == 0, "TE7 and Add does nothing");
}

void TE8_one_undo_step_each() {
    Rig rig;
    std::string error;
    auto entries = [&rig] { return ide::read_terrain_materials(rig.game, rig.terrain_id).materials; };
    // Voxels on Id 1 first, so Remove is a Keep Cells on one in use.
    rig.fill_ball(0, 0, 0, 5, 1);
    // What the Studio's host runs on the simulation thread, each its own step.
    const InstanceId entry = ide::run_add(rig.game, rig.terrain_id, rig.rock, error);
    Expect(entry != 0 && error.empty(), "TE8 Add makes one");
    Expect(!ide::run_set(rig.game, entry, rig.grass), "TE8 Set points it at Grass");
    // Named after Rock, it follows its Material's name.
    Expect(rig.game.name(entry) == "Grass", "TE8 and renames it Grass");
    // Rename goes through IdeLayout::rename, which records it as "Rename".
    Expect(!rig.game.rename_error(entry, "Cliff"), "TE8 Cliff is a fine name");
    {
        ide::ScopedRecording step(rig.game, "Rename");
        rig.game.set_name(entry, "Cliff");
    }
    Expect(!ide::run_remove(rig.game, entry, {}), "TE8 Remove with Keep Cells");
    Expect(entries().empty() && rig.cell_id(0, 0, 0) == 1, "TE8 it is gone, and its voxels keep Id 1");

    rig.game.history().undo();
    std::vector<ide::TerrainMaterialView> back = entries();
    Expect(back.size() == 1 && back[0].id == entry && back[0].name == "Cliff" && back[0].material == rig.grass,
           "TE8 the first undo brings it back as it was before Remove");
    rig.game.history().undo();
    back = entries();
    Expect(back.size() == 1 && back[0].name == "Grass" && back[0].material == rig.grass,
           "TE8 the second undoes only the Rename");
    rig.game.history().undo();
    back = entries();
    Expect(back.size() == 1 && back[0].name == "Rock" && back[0].material == rig.rock,
           "TE8 the third undoes the Set, and the name that came with it");
    rig.game.history().undo();
    Expect(entries().empty(), "TE8 the fourth undoes the Add");
    Expect(rig.cell_id(0, 0, 0) == 1, "TE8 none of them touched the voxels");
}

void TE9_unassigned_row() {
    PaneRig rig;
    std::string error;
    const InstanceId rock = ide::add_terrain_material(rig.game, rig.terrain_id, rig.rock, error);
    rig.frames();
    Expect(rig.editor->unassignedRow() == nullptr, "TE9 no row while every used Id has a material");
    rig.fill_ball(0, 0, 0, 5, 3);
    rig.fill_ball(100, 0, 0, 5, 7);
    rig.frames();
    Expect(rig.editor->unassignedRow() != nullptr, "TE9 voxels on unassigned Ids show the row");
    Expect(LabelText(rig.editor->unassignedRow(), "te-unassigned-text") ==
               "Ids 3, 7 have voxels but no material: they draw as the default.",
           "TE9 naming the Ids");
    rig.click(rig.scene->getElementById("te-unassigned-replace"));
    rig.frames();
    Expect(rig.editor->pickerRow(0) != nullptr && rig.editor->pickerRow(rock) != nullptr,
           "TE9 Replace… offers this Terrain's materials and Default");
    rig.click(rig.editor->pickerRow(rock));
    rig.frames();
    Expect(rig.replaced == std::vector<std::pair<int, int>>{{3, 1}, {7, 1}}, "TE9 each listed Id becomes Rock's");
    Expect(rig.cell_id(0, 0, 0) == 1 && rig.cell_id(100, 0, 0) == 1, "TE9 in the voxels");
    Expect(rig.editor->unassignedRow() == nullptr, "TE9 and the row goes");
    Expect(rig.editor->inUseBadge(rock) != nullptr, "TE9 Rock is in use now");
}

void TE10_click_selects() {
    PaneRig rig;
    std::string error;
    const InstanceId rock = ide::add_terrain_material(rig.game, rig.terrain_id, rig.rock, error);
    const InstanceId grass = ide::add_terrain_material(rig.game, rig.terrain_id, rig.grass, error);
    rig.frames(2);
    rig.click_card(rock);
    Expect(rig.game.selection().get() == std::vector<InstanceId>{rock},
           "TE10 a click on a card selects its TerrainMaterial, for Properties");
    Expect(HasClass(rig.editor->cardNode(rock), "selected") && !HasClass(rig.editor->cardNode(grass), "selected"),
           "TE10 and marks its card");
    rig.click_card(grass);
    Expect(rig.game.selection().get() == std::vector<InstanceId>{grass}, "TE10 a click on another moves it");
    Expect(!HasClass(rig.editor->cardNode(rock), "selected") && HasClass(rig.editor->cardNode(grass), "selected"),
           "TE10 and the mark follows");
}

void TE11_add_tile_disabled_at_cap() {
    PaneRig rig;
    std::string error;
    // Straight through the data helper, not the host, so this does not count
    // toward rig.adds: only the tile's own click should (or should not).
    for (int i = 0; i < 255; ++i) {
        Expect(ide::add_terrain_material(rig.game, rig.terrain_id, 0, error) != 0, "TE11 filling to the cap");
    }
    rig.frames(2);
    Expect(rig.counter() == "255 / 255 materials", "TE11 the counter reads 255 / 255");
    Expect(rig.editor->addTile()->isDisabled(), "TE11 the tile is disabled at the cap, like the header button");
    rig.click(rig.editor->addTile());
    rig.frames(2);
    Expect(rig.adds == 0 && rig.editor->view().materials.size() == 255,
           "TE11 a click on the tile at the cap does nothing");
}

// TL-T3: the tab's header includes "MB textures" once a host wires
// texture_memory_bytes/texture_revision in (TE1..TE11's PaneRig never does,
// and keep showing their plain "N / M materials" counter), and the figure
// updates once a background build actually lands -- not merely once the
// callback exists.
void TL_T3_header_shows_and_updates_texture_memory() {
    TexturePaneRig rig;
    rig.frames();
    const std::string before = rig.counter();
    Expect(before.find("MB textures") != std::string::npos, "TL-T3 the header includes \"MB textures\"");
    // " 0 MB textures" (the leading space before the digit), not plain "0 MB
    // textures": the latter is also a substring of "10 MB textures", "20 MB
    // textures", and so on.
    Expect(before.find(" 0 MB textures") != std::string::npos,
           ("TL-T3 starts at 0 MB, nothing built yet (" + before + ")").c_str());

    rig.settle();   // the untextured default layer (layer 0) lands
    rig.frames();   // IdeTerrainEditor::refresh() picks up the new texture_revision

    const std::string after = rig.counter();
    Expect(after != before, ("TL-T3 the header's text changes once a build lands (" + after + ")").c_str());
    Expect(after.find("MB textures") != std::string::npos && after.find(" 0 MB textures") == std::string::npos,
           ("TL-T3 the figure is no longer 0 MB after the build (" + after + ")").c_str());
}

}  // namespace

int main() {
    data_helpers();
    TE1_cards_and_counter();
    TE2_add_tile();
    TE3_set_material_and_rename();
    TE4_remove_unused_asks_nothing();
    TE5_remove_used_asks();
    TE6_script_changes_follow();
    TE7_deleted_terrain();
    TE8_one_undo_step_each();
    TE9_unassigned_row();
    TE10_click_selects();
    TE11_add_tile_disabled_at_cap();
    TL_T3_header_shows_and_updates_texture_memory();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failed\n", gFailures);
        return 1;
    }
    std::printf("terrain editor tests passed\n");
    return 0;
}
