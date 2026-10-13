#include "ide/BillboardBaker.hpp"
#include "ide/IdePrefabEditor.hpp"
#include "ide/PrefabLods.hpp"
#include "ide/PrefabModels.hpp"
#include "ChangeHistoryService.hpp"
#include "SelectionService.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "LuaApi.hpp"
#include "ScriptRuntime.hpp"
#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The Prefab editor in a headless scene, and the model logic under it.
namespace {

using engine_core::InstanceId;
using engine_core::LodKind;
using ide::ModelPart;

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

constexpr double kWidth = 960;
constexpr double kHeight = 640;

// Marks this thread as the simulation thread while it lives, as asset setters need.
struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

// A place whose Meshes hold Rock and, in a Folder Props, Lid; whose Materials
// hold Wood and Metal; whose Textures hold Bark; and whose Prefabs hold Crate,
// with no Models yet. The host writes straight to the game, standing in for
// the simulation thread.
struct Rig {
    SimRole role;
    engine_core::ScriptRuntime runtime;
    engine_core::Game game;
    InstanceId rock = 0;
    InstanceId lid = 0;
    InstanceId wood = 0;
    InstanceId metal = 0;
    InstanceId bark = 0;
    InstanceId crate = 0;
    std::vector<std::string> notices;
    std::vector<std::pair<InstanceId, std::string>> renames;
    std::vector<InstanceId> removed;
    int adds = 0;
    int sets = 0;
    std::shared_ptr<ide::IdePrefabEditor> editor;
    std::shared_ptr<jadefx::Scene> scene;
    double time = 0;

    Rig() {
        const InstanceId meshes = game.service("Meshes");
        rock = make<engine_core::Mesh>("Rock", meshes);
        lid = make<engine_core::Mesh>("Lid", make<engine_core::Folder>("Props", meshes));
        wood = make<engine_core::Material>("Wood", game.service("Materials"));
        metal = make<engine_core::Material>("Metal", game.service("Materials"));
        bark = make<engine_core::Texture>("Bark", game.service("Textures"));
        crate = make<engine_core::Prefab>("Crate", game.service("Prefabs"));

        ide::PrefabEditorHost host;
        host.add_model = [this](InstanceId prefab, InstanceId mesh, InstanceId material,
                                std::shared_ptr<ide::InsertResult> result) {
            ++adds;
            std::string error;
            result->id = ide::add_model(game, prefab, mesh, material, error);
            result->error = error;
            result->done = true;
        };
        host.set_part = [this](InstanceId model, ModelPart part, InstanceId target) {
            ++sets;
            if (std::optional<std::string> error = ide::set_model_part(game, model, part, target)) {
                notices.push_back(*error);
            }
        };
        host.rename = [this](InstanceId id, std::string name) {
            game.set_name(id, name);
            renames.emplace_back(id, std::move(name));
        };
        host.remove = [this](const std::vector<InstanceId>& ids) {
            for (InstanceId id : ids) {
                removed.push_back(id);
                game.destroy_tree(id);
            }
        };
        host.notice = [this](std::string text) { notices.push_back(std::move(text)); };
        host.add_lod = [this](InstanceId prefab, std::shared_ptr<ide::InsertResult> result) {
            std::string error;
            result->id = ide::run_add_lod(game, prefab, error);
            result->error = error;
            result->done = true;
        };
        const auto said = [this](std::optional<std::string> error) {
            if (error) {
                notices.push_back(*error);
            }
        };
        host.set_lod_distance = [this, said](InstanceId layer, double distance) {
            said(ide::run_set_lod_distance(game, layer, distance));
        };
        host.set_lod_kind = [this, said](InstanceId layer, LodKind kind) {
            said(ide::run_set_lod_kind(game, layer, kind));
        };
        host.set_lod_material = [this, said](InstanceId layer, InstanceId material) {
            said(ide::run_set_lod_material(game, layer, material));
        };
        host.set_lod_size = [this, said](InstanceId layer, float width, float height) {
            said(ide::run_set_lod_size(game, layer, width, height));
        };
        host.set_lod_card = [this, said](InstanceId layer, ide::CardFiles files) {
            said(ide::run_set_lod_card(game, layer, files));
        };
        editor = jadefx::make<ide::IdePrefabEditor>(game, crate, std::move(host));
        editor->setPrefWidthRatio(1);
        editor->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(editor, kWidth, kHeight);
        frames(2);
    }

    template <typename T>
    InstanceId make(const char* name, InstanceId parent) {
        T& object = game.create<T>();
        game.set_name(object.id(), name);
        game.set_parent(object.id(), parent);
        return object.id();
    }

    void frames(int count = 1) {
        for (int i = 0; i < count; ++i) {
            scene->layout(kWidth, kHeight, time);
            time += 0.05;
        }
    }

    // Presses and releases the middle of node. JadeFX counts presses on one
    // spot within 0.4 s of real time as one run, so a move away comes first.
    void click(jadefx::Node* node, int button = 0) {
        Expect(node != nullptr, "the node to click is on screen");
        if (node == nullptr) {
            return;
        }
        const double x = node->getAbsoluteX() + node->getWidth() * 0.5;
        const double y = node->getAbsoluteY() + node->getHeight() * 0.5;
        scene->noteMove(x, y);
        scene->noteButton(button, true, x, y);
        scene->noteButton(button, false, x, y);
        frames();
    }

    void key(int code) {
        scene->noteKey(code, true, false, 0);
        frames();
    }

    const ide::ModelView* view(InstanceId model) const {
        for (const ide::ModelView& v : editor->models()) {
            if (v.id == model) {
                return &v;
            }
        }
        return nullptr;
    }

    InstanceId last_model() const { return editor->models().empty() ? 0 : editor->models().back().id; }

    const ide::LodView* lod(InstanceId layer) const {
        for (const ide::LodView& v : editor->lods()) {
            if (v.id == layer) {
                return &v;
            }
        }
        return nullptr;
    }

    // Types text into field and presses Enter, as a person would.
    void type(jadefx::TextField* field, const std::string& text) {
        Expect(field != nullptr, "the field to type in is on screen");
        if (field == nullptr) {
            return;
        }
        field->requestFocus();
        field->setText(text);
        key(jadefx::Key::Enter);
        frames();
    }

    // Presses and releases near the top edge of node, away from what is inside it.
    void click_edge(jadefx::Node* node) {
        Expect(node != nullptr, "the node to click is on screen");
        if (node == nullptr) {
            return;
        }
        const double x = node->getAbsoluteX() + node->getWidth() * 0.5;
        const double y = node->getAbsoluteY() + 4;
        scene->noteMove(x, y);
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
        frames();
    }
};

void model_logic() {
    Rig rig;
    engine_core::Game& game = rig.game;
    std::string error;
    const InstanceId a = ide::add_model(game, rig.crate, 0, 0, error);
    const InstanceId b = ide::add_model(game, rig.crate, 0, 0, error);
    const InstanceId c = ide::add_model(game, rig.crate, rig.rock, rig.wood, error);
    Expect(game.name(a) == "Model" && game.name(b) == "Model 2", "new Models are Model, then Model 2");
    Expect(game.name(c) == "Rock", "a Model made with a Mesh is named after it");
    Expect(ide::add_model(game, rig.rock, 0, 0, error) == 0 && !error.empty(), "only a Prefab takes a Model");

    std::vector<ide::ModelView> models = ide::read_models(game, rig.crate);
    Expect(models.size() == 3 && models[2].mesh.id == rig.rock && models[2].material.id == rig.wood,
           "read_models reads each Model's Mesh and Material");
    Expect(models.size() == 3 && models[2].mesh.where == "Meshes", "a part's folder starts at its category");
    Expect(models.size() == 3 && models[0].status() == ide::ModelStatus::NeedsBoth &&
               models[2].status() == ide::ModelStatus::Ready,
           "status says what a Model still needs");

    Expect(!ide::set_model_part(game, a, ModelPart::Mesh, rig.lid), "a Mesh goes in the Mesh slot");
    Expect(game.name(a) == "Lid", "a Model still named Model takes its new Mesh's name");
    Expect(ide::read_models(game, rig.crate)[0].mesh.where == "Meshes/Props", "a nested Mesh shows its folder");
    Expect(!ide::set_model_part(game, a, ModelPart::Mesh, rig.rock) && game.name(a) == "Rock 2",
           "a Model named after its old Mesh follows the new one, numbered past a sibling");
    game.set_name(b, "Handle");
    Expect(!ide::set_model_part(game, b, ModelPart::Mesh, rig.rock) && game.name(b) == "Handle",
           "a Model named by hand keeps its name");
    Expect(ide::set_model_part(game, b, ModelPart::Mesh, rig.wood).has_value(), "a Material is refused as a Mesh");
    Expect(!ide::set_model_part(game, b, ModelPart::Mesh, 0) && ide::read_models(game, rig.crate)[1].mesh.id == 0,
           "0 clears a part");

    game.destroy_tree(rig.wood);
    models = ide::read_models(game, rig.crate);
    Expect(models[2].material.missing && models[2].status() == ide::ModelStatus::Missing,
           "a part whose asset was deleted is missing");

    const std::vector<ide::AssetChoice> meshes = ide::part_choices(game, ModelPart::Mesh);
    Expect(meshes.size() == 2 && meshes[0].name == "Lid" && meshes[0].where == "Meshes/Props" &&
               meshes[1].name == "Rock" && meshes[1].where == "Meshes",
           "choices list every Mesh by name, with its folder");
    Expect(ide::filter_choices(meshes, "PROPS").size() == 1, "a search matches a folder, ignoring case");
    Expect(ide::filter_choices(meshes, "").size() == 2, "an empty search keeps every choice");
    std::vector<ide::AssetChoice> textures(1);
    textures[0].name = "Diffuse";
    textures[0].where = "Textures";
    textures[0].path = "Textures/Grass/Diffuse.png";
    Expect(ide::filter_choices(textures, "grass").size() == 1, "a search matches a file asset's Path");

    const ide::DraggedParts parts = ide::dragged_parts(game, {rig.metal, rig.crate, rig.lid, rig.rock});
    Expect(parts.mesh == rig.lid && parts.material == rig.metal, "a drag's first Mesh and first Material count");
    Expect(!ide::dragged_parts(game, {rig.crate}).any(), "a drag without either holds no parts");
}

// Add Model, then a pick in each picker, makes a whole Model.
void guided_add() {
    Rig rig;
    Expect(rig.editor->emptyState() != nullptr, "a Prefab with no Models shows the empty state");
    rig.click(rig.editor->addButton());
    rig.frames(2);
    const InstanceId model = rig.last_model();
    Expect(rig.adds == 1 && model != 0, "Add Model adds a Model");
    Expect(rig.editor->emptyState() == nullptr && rig.editor->cardNode(model) != nullptr, "its card shows");
    Expect(rig.game.selection().get() == std::vector<InstanceId>{model}, "the new Model is selected");
    Expect(rig.editor->pickerOpen() && rig.editor->pickerRow(rig.rock) != nullptr,
           "its Mesh picker opens, listing the Meshes");
    Expect(rig.editor->pickerRow(0) == nullptr, "an empty slot's picker has no None");

    rig.click(rig.editor->pickerRow(rig.rock));
    rig.frames(2);
    const ide::ModelView* view = rig.view(model);
    Expect(view != nullptr && view->mesh.id == rig.rock, "picking a Mesh sets it");
    Expect(view != nullptr && view->name == "Rock", "the Model takes the Mesh's name");
    Expect(rig.editor->pickerOpen() && rig.editor->pickerRow(rig.wood) != nullptr,
           "the Material picker opens next");

    rig.click(rig.editor->pickerRow(rig.wood));
    rig.frames(2);
    view = rig.view(model);
    Expect(view != nullptr && view->status() == ide::ModelStatus::Ready, "two picks make a ready Model");
    Expect(!rig.editor->pickerOpen(), "the flow ends there");
}

void picker_keys_and_none() {
    Rig rig;
    std::string error;
    const InstanceId model = ide::add_model(rig.game, rig.crate, rig.rock, 0, error);
    rig.frames(2);
    rig.click(rig.editor->slotNode(model, ModelPart::Mesh));
    Expect(rig.editor->pickerOpen(), "a click on a slot opens its picker");
    Expect(rig.editor->pickerRow(0) != nullptr, "a filled slot's picker offers None");

    rig.editor->pickerField()->setText("li");
    rig.frames();
    Expect(rig.editor->pickerRow(rig.rock) == nullptr && rig.editor->pickerRow(rig.lid) != nullptr,
           "typing filters the list");
    rig.key(jadefx::Key::Enter);
    rig.frames();
    Expect(rig.view(model) != nullptr && rig.view(model)->mesh.id == rig.lid, "Enter picks the highlighted row");
    Expect(!rig.editor->pickerOpen(), "a pick closes the picker");

    rig.click(rig.editor->slotNode(model, ModelPart::Mesh));
    rig.key(jadefx::Key::Escape);
    Expect(!rig.editor->pickerOpen(), "Escape closes the picker");

    rig.click(rig.editor->slotNode(model, ModelPart::Mesh));
    rig.click(rig.editor->pickerRow(0));
    rig.frames();
    Expect(rig.view(model) != nullptr && rig.view(model)->mesh.id == 0, "None clears the slot");
}

void clear_select_delete_rename() {
    Rig rig;
    std::string error;
    const InstanceId first = ide::add_model(rig.game, rig.crate, rig.rock, rig.wood, error);
    const InstanceId second = ide::add_model(rig.game, rig.crate, rig.lid, 0, error);
    rig.frames(2);

    const int before = rig.sets;
    rig.click(rig.editor->slotClearNode(first, ModelPart::Material));
    rig.frames();
    Expect(rig.sets == before + 1 && rig.view(first)->material.id == 0, "× on a slot clears it");
    Expect(!rig.editor->pickerOpen(), "and opens no picker");
    Expect(rig.editor->slotClearNode(first, ModelPart::Material) == nullptr, "an empty slot has no ×");

    // Away from the slots: the card's top edge.
    jadefx::Node* card = rig.editor->cardNode(second);
    Expect(card != nullptr, "the second card shows");
    if (card != nullptr) {
        const double x = card->getAbsoluteX() + card->getWidth() * 0.5;
        const double y = card->getAbsoluteY() + 6;
        rig.scene->noteMove(x, y);
        rig.scene->noteButton(0, true, x, y);
        rig.scene->noteButton(0, false, x, y);
        rig.frames();
    }
    Expect(rig.game.selection().get() == std::vector<InstanceId>{second}, "a click on a card selects its Model");

    rig.key(jadefx::Key::F2);
    jadefx::TextField* field = rig.editor->renameField(second);
    Expect(field != nullptr && field->isVisible() && field->getText() == "Lid", "F2 renames it in place");
    if (field != nullptr) {
        field->setText("Top");
    }
    rig.key(jadefx::Key::Enter);
    Expect(!rig.renames.empty() && rig.renames.back() == std::pair<InstanceId, std::string>{second, "Top"},
           "Enter keeps the new name");

    rig.key(jadefx::Key::Delete);
    rig.frames();
    Expect(rig.removed == std::vector<InstanceId>{second}, "Delete removes the selected Model");
    Expect(rig.editor->models().size() == 1 && rig.editor->cardNode(second) == nullptr, "its card goes");
}

void drops() {
    Rig rig;
    // Empty space: a Model holding the Mesh, and its Material picker opens.
    Expect(rig.editor->dropOnto({rig.lid}), "a Mesh dropped on empty space is taken");
    rig.frames(3);
    const InstanceId made = rig.last_model();
    Expect(made != 0 && rig.view(made)->mesh.id == rig.lid && rig.view(made)->name == "Lid",
           "it makes a Model holding the Mesh, named after it");
    Expect(rig.editor->pickerOpen() && rig.editor->pickerRow(rig.wood) != nullptr,
           "and asks for the Material it still needs");
    rig.editor->closePicker();

    // Onto a card: the slot that fits.
    Expect(rig.editor->dropOnto({rig.metal}, made), "a Material dropped on a card is taken");
    Expect(rig.view(made) != nullptr, "the Model is still shown");
    rig.frames();
    Expect(rig.view(made)->material.id == rig.metal, "it fills the Material slot");

    // Onto the wrong slot: refused, with a notice.
    const ModelPart mesh = ModelPart::Mesh;
    rig.notices.clear();
    Expect(!rig.editor->dropOnto({rig.wood}, made, &mesh), "a Material dropped on the Mesh slot is refused");
    Expect(!rig.notices.empty(), "with a notice that says why");
    Expect(!rig.editor->dropOnto({rig.crate}), "a drop with no Mesh or Material makes nothing");
    Expect(rig.adds == 1, "only the first drop made a Model");
}

void deleted_prefab() {
    Rig rig;
    rig.frames();
    Expect(rig.editor->title() == "Crate", "the tab shows the Prefab's name");
    rig.game.set_name(rig.crate, "Box");
    rig.frames();
    Expect(rig.editor->title() == "Box", "and follows a rename");
    rig.game.destroy_tree(rig.crate);
    rig.frames();
    Expect(rig.editor->emptyState() == nullptr && rig.editor->models().empty(),
           "a deleted Prefab shows no Models and no empty state");
    rig.click(rig.editor->addButton());
    Expect(rig.adds == 0, "and Add Model does nothing");
}

// The LOD logic: adding, reading in draw order, and each write one undo step.
void lod_logic() {
    Rig rig;
    engine_core::Game& game = rig.game;
    std::string error;
    const InstanceId near_layer = ide::run_add_lod(game, rig.crate, error);
    const InstanceId far_layer = ide::run_add_lod(game, rig.crate, error);
    Expect(game.name(near_layer) == "LOD" && game.name(far_layer) == "LOD 2", "new LODs are LOD, then LOD 2");
    std::vector<ide::LodView> lods = ide::read_lods(game, rig.crate);
    Expect(lods.size() == 2 && lods[0].distance == 100.0 && lods[1].distance == 200.0,
           "the first LOD is at 100 units, the next twice the farthest");
    Expect(lods.size() == 2 && lods[0].until == std::optional<double>(200.0) && !lods[1].until,
           "each runs until the next; the last runs on");
    Expect(ide::run_add_lod(game, rig.rock, error) == 0 && !error.empty(), "only a Prefab takes a LOD");

    Expect(!ide::run_set_lod_distance(game, near_layer, 300.0), "a distance is set");
    lods = ide::read_lods(game, rig.crate);
    Expect(lods.size() == 2 && lods[0].id == far_layer && lods[1].id == near_layer, "they read nearest first");
    Expect(!ide::run_set_lod_distance(game, far_layer, 300.0), "two may share a distance");
    lods = ide::read_lods(game, rig.crate);
    Expect(lods.size() == 2 && lods[0].covered && !lods[1].covered,
           "of two at one distance, the earlier one is covered");
    Expect(ide::run_set_lod_distance(game, far_layer, -5.0).has_value(), "a negative distance is refused");

    Expect(!ide::run_set_lod_kind(game, near_layer, LodKind::Billboard), "a kind is set");
    Expect(ide::run_set_lod_material(game, near_layer, rig.bark).has_value(), "a Texture is refused as a Material");
    Expect(!ide::run_set_lod_material(game, near_layer, rig.wood), "a Material is set");
    Expect(!ide::run_set_lod_size(game, near_layer, 3.f, 9.f), "a size is set");
    Expect(ide::run_set_lod_size(game, near_layer, 0.f, 9.f).has_value(), "a zero width is refused");
    // The LOD a list holds with id.
    const auto find = [](const std::vector<ide::LodView>& list, InstanceId id) {
        const auto found = std::find_if(list.begin(), list.end(), [id](const ide::LodView& v) { return v.id == id; });
        return found != list.end() ? *found : ide::LodView{};
    };
    const ide::LodView billboard = find(ide::read_lods(game, rig.crate), near_layer);
    Expect(billboard.kind == LodKind::Billboard && billboard.material.id == rig.wood &&
               billboard.material.where == "Materials" && billboard.width == 3.f && billboard.height == 9.f,
           "read_lods reads a billboard's Material and size");

    // Each write is its own step.
    game.history().undo();
    const ide::LodView sized = find(ide::read_lods(game, rig.crate), near_layer);
    Expect(sized.width == 4.f && sized.material.id == rig.wood, "undo takes back the size alone");
    game.history().undo();
    Expect(find(ide::read_lods(game, rig.crate), near_layer).material.id == 0, "then the Material");
    game.history().undo();
    Expect(find(ide::read_lods(game, rig.crate), near_layer).kind == LodKind::Model, "then the kind");
    game.history().redo();
    Expect(find(ide::read_lods(game, rig.crate), near_layer).kind == LodKind::Billboard, "and redo puts it back");

    // A Model-kind layer holds Models, as the Prefab does.
    const InstanceId low = ide::add_model(game, far_layer, rig.lid, rig.wood, error);
    Expect(low != 0 && game.parent(low) == far_layer, "a LOD takes a Model");
    const ide::LodView holder = find(ide::read_lods(game, rig.crate), far_layer);
    Expect(holder.models.size() == 1 && holder.models[0].name == "Lid", "read_lods reads a layer's Models");
    Expect(ide::read_models(game, rig.crate).empty(), "the Prefab's own Models are only its own");
    Expect(ide::distance_text(250.0) == "250" && ide::distance_text(62.5) == "62.5", "distances read plainly");
}

// The LOD section: Add LOD, the distance typed in place, the kind, a
// billboard's Material and size, a layer's own Models, and Delete.
void lod_section() {
    Rig rig;
    rig.click(rig.editor->addLodButton());
    rig.frames(3);
    Expect(rig.editor->lods().size() == 1, "Add LOD adds a LOD");
    const InstanceId layer = rig.editor->lods().empty() ? 0 : rig.editor->lods()[0].id;
    Expect(rig.editor->lodCard(layer) != nullptr, "its card shows");
    Expect(rig.editor->emptyState() == nullptr, "a Prefab with a LOD and no Models is not empty");
    Expect(rig.game.selection().get() == std::vector<InstanceId>{layer}, "the new LOD is selected");
    jadefx::TextField* distance = rig.editor->lodDistanceField(layer);
    Expect(distance != nullptr && distance->getText() == "100", "its distance shows in place");

    rig.type(distance, "250");
    Expect(rig.lod(layer) != nullptr && rig.lod(layer)->distance == 250.0, "a distance typed and entered is set");
    rig.type(distance, "-3");
    Expect(rig.lod(layer)->distance == 250.0 && distance->getText() == "250",
           "a negative distance is refused, and the field shows the distance again");

    rig.click(rig.editor->lodKindButton(layer, LodKind::Billboard));
    rig.frames();
    Expect(rig.lod(layer)->kind == LodKind::Billboard, "Billboard makes it a billboard");
    Expect(rig.editor->lodMaterialSlot(layer) != nullptr && rig.editor->lodNewModelTile(layer) == nullptr,
           "a billboard shows a Material slot, not Models");
    jadefx::TextField* width = rig.editor->lodSizeField(layer, 0);
    Expect(width != nullptr && width->getText() == "4", "its width shows");
    rig.type(width, "6");
    Expect(rig.lod(layer)->width == 6.f && rig.lod(layer)->height == 8.f, "a width typed is set, the height kept");
    Expect(rig.editor->dropMaterial({rig.wood}, layer), "a Material dropped on it is taken");
    rig.frames();
    Expect(rig.lod(layer)->material.id == rig.wood, "it fills the Material slot");
    rig.notices.clear();
    Expect(!rig.editor->dropMaterial({rig.rock}, layer) && !rig.notices.empty(),
           "a Mesh dropped on it is refused, with a notice");

    rig.click(rig.editor->lodKindButton(layer, LodKind::None));
    rig.frames();
    Expect(rig.lod(layer)->kind == LodKind::None && rig.editor->lodMaterialSlot(layer) == nullptr,
           "None draws nothing, and shows no slot");

    rig.click(rig.editor->lodKindButton(layer, LodKind::Model));
    rig.frames();
    Expect(rig.editor->lodNewModelTile(layer) != nullptr, "a Model LOD shows a New Model tile");
    rig.click(rig.editor->lodNewModelTile(layer));
    rig.frames(3);
    const InstanceId swap = rig.lod(layer) != nullptr && !rig.lod(layer)->models.empty()
                                ? rig.lod(layer)->models[0].id
                                : 0;
    Expect(swap != 0 && rig.game.parent(swap) == layer, "its tile adds a Model to the LOD");
    Expect(rig.editor->cardNode(swap) != nullptr && rig.editor->models().empty(),
           "the Model's card shows in the LOD, not up close");
    // The picker starts with the last Mesh search; an empty one lists them all.
    if (jadefx::TextField* search = rig.editor->pickerField()) {
        search->setText("");
        rig.frames();
    }
    Expect(rig.editor->pickerOpen() && rig.editor->pickerRow(rig.rock) != nullptr,
           "and its Mesh picker opens");
    rig.click(rig.editor->pickerRow(rig.rock));
    rig.frames(2);
    rig.editor->closePicker();
    Expect(rig.lod(layer)->models[0].mesh.id == rig.rock, "a pick fills the LOD's Model");

    rig.click_edge(rig.editor->lodCard(layer));
    Expect(rig.game.selection().get() == std::vector<InstanceId>{layer}, "a click on a LOD's card selects it");
    rig.key(jadefx::Key::Delete);
    rig.frames();
    Expect(rig.removed == std::vector<InstanceId>{layer} && rig.editor->lods().empty() &&
               rig.editor->lodCard(layer) == nullptr,
           "Delete removes the selected LOD");
}

// The Texture material's slot holds, or null.
const engine_core::Texture* slot_texture(const engine_core::Game& game, const engine_core::Material& material,
                                         std::size_t slot) {
    const engine_core::LuaSlot reference = material.reference(slot);
    return reference.kind == engine_core::LuaSlot::Kind::Instance
               ? dynamic_cast<const engine_core::Texture*>(game.instance(reference.id))
               : nullptr;
}

// A baked card: a Material in Materials/Billboards whose maps are Textures in
// Textures/Billboards, numbers where a map was not saved, and the card's
// size, in one undo step; baked again, the same Material and Textures.
void lod_card_bake() {
    using engine_core::Material;
    Rig rig;
    engine_core::Game& game = rig.game;
    std::string error;
    const InstanceId layer = ide::run_add_lod(game, rig.crate, error);
    Expect(!ide::run_set_lod_kind(game, layer, LodKind::Billboard), "a billboard to bake for");
    ide::CardFiles files;
    files.color = "textures/billboards/Crate.color.00000001.png";
    files.normal = "textures/billboards/Crate.normal.00000001.png";
    files.roughness = 0.8f;
    files.metalness = 0.f;
    files.reflectivity = 0.25f;
    files.width = 2.f;
    files.height = 3.f;
    Expect(!ide::run_set_lod_card(game, layer, files), "a card is set");
    ide::LodView card = ide::read_lods(game, rig.crate)[0];
    Expect(card.material.id != 0 && card.material.where == "Materials/Billboards" && card.material.name == "Crate Card",
           "its Material is new, in Materials/Billboards, named after the Prefab");
    const auto* material = dynamic_cast<const Material*>(game.instance(card.material.id));
    const engine_core::Texture* color =
        material != nullptr ? slot_texture(game, *material, Material::kDiffuseTextureReference) : nullptr;
    const engine_core::Texture* normal =
        material != nullptr ? slot_texture(game, *material, Material::kNormalTextureReference) : nullptr;
    Expect(color != nullptr && color->path() == files.color && game.name(color->id()) == "Crate Card Color" &&
               game.name(game.parent(color->id())) == "Billboards",
           "its color map is a Texture in Textures/Billboards with the card's Path");
    Expect(normal != nullptr && normal->path() == files.normal, "and its normal map another");
    Expect(material != nullptr && slot_texture(game, *material, Material::kRoughnessTextureReference) == nullptr &&
               std::abs(material->roughness() - 0.8) < 1e-6 && material->metalness() == 0.0 &&
               std::abs(material->reflectivity() - 0.25) < 1e-6,
           "a map not saved leaves its slot empty, and the Material's number holds it");
    Expect(card.width == 2.f && card.height == 3.f, "the layer takes the card's size");
    game.history().undo();
    card = ide::read_lods(game, rig.crate)[0];
    Expect(card.material.id == 0 && card.width == 4.f &&
               game.find_first_child(game.service("Textures"), "Billboards") == 0 &&
               game.find_first_child(game.service("Materials"), "Billboards") == 0,
           "one undo takes back the Material, its Textures, their folders, and the size");
    game.history().redo();
    const InstanceId first = ide::read_lods(game, rig.crate)[0].material.id;
    files.color = "textures/billboards/Crate.color.00000002.png";
    files.normal.clear();
    files.roughness_map = "textures/billboards/Crate.roughness.00000002.png";
    files.roughness = 1.f;
    files.width = 2.5f;
    Expect(!ide::run_set_lod_card(game, layer, files), "baked again");
    card = ide::read_lods(game, rig.crate)[0];
    material = dynamic_cast<const Material*>(game.instance(card.material.id));
    const engine_core::Texture* recolored =
        material != nullptr ? slot_texture(game, *material, Material::kDiffuseTextureReference) : nullptr;
    Expect(card.material.id == first && recolored != nullptr && recolored->id() == color->id() &&
               recolored->path() == files.color,
           "baked again, the same Material and color Texture take the new card");
    Expect(material != nullptr && slot_texture(game, *material, Material::kNormalTextureReference) == nullptr &&
               slot_texture(game, *material, Material::kRoughnessTextureReference) != nullptr &&
               material->roughness() == 1.0,
           "a map no longer saved leaves its slot; a new one takes its own");

    // The button: a Billboard's card has it, and a click waits for the next paint to draw.
    rig.frames(2);
    Expect(rig.editor->lodBakeButton(layer) != nullptr, "a billboard's card has Bake card");
    rig.click(rig.editor->lodBakeButton(layer));
    Expect(rig.editor->baking() == layer, "Bake card waits to draw at the next paint");
}

// The card's frame, its maps shrunk with each one's values carried into the clear pixels, and its files.
void card_pixels() {
    const ide::CardFrame frame = ide::card_frame({-1.f, -0.5f, -3.f}, {2.f, 6.f, 1.f});
    Expect(frame.width == 6.f && frame.height == 6.f, "a card is twice the widest reach wide, from the origin up");
    // Six by two: an opaque red block facing out, a block with one opaque blue
    // pixel facing right, and a clear block.
    runner::GBufferPixels render;
    render.width = 6;
    render.height = 2;
    render.albedo.assign(6 * 2 * 4, 0);
    render.normal.assign(6 * 2 * 3, 0.f);
    render.material.assign(6 * 2 * 4, 0);
    render.emissive.assign(6 * 2 * 3, 0.f);
    const auto put = [&](int x, int y, unsigned char r, unsigned char b, float nx, float nz, unsigned char rough) {
        const std::size_t at = static_cast<std::size_t>(y) * 6 + static_cast<std::size_t>(x);
        render.albedo[at * 4] = r;
        render.albedo[at * 4 + 2] = b;
        render.albedo[at * 4 + 3] = 255;
        render.normal[at * 3] = nx;
        render.normal[at * 3 + 2] = nz;
        render.material[at * 4 + 1] = rough;
        render.material[at * 4 + 2] = 128;
    };
    for (int y = 0; y < 2; ++y) {
        put(0, y, 200, 0, 0.f, 1.f, 100);
        put(1, y, 200, 0, 0.f, 1.f, 100);
    }
    put(3, 1, 0, 200, 1.f, 0.f, 200);
    const ide::CardMaps small = ide::shrink_card(render);
    Expect(small.color.width == 3 && small.color.height == 1 && small.normal.width == 3, "shrunk to half");
    if (small.color.rgba.size() == 12 && small.normal.rgba.size() == 12 && small.roughness.rgba.size() == 12) {
        Expect(small.color.rgba[0] == 200 && small.color.rgba[3] == 255, "a drawn block keeps its color, whole");
        Expect(small.color.rgba[6] == 200 && small.color.rgba[7] == 64,
               "a part-drawn block keeps the drawn color, a quarter there");
        Expect(small.color.rgba[11] == 0 && small.color.rgba[10] == 200,
               "a clear block carries its nearest drawn color, and stays clear");
        Expect(small.normal.rgba[0] == 128 && small.normal.rgba[2] == 255 && small.normal.rgba[4] == 255 &&
                   small.normal.rgba[6] == 128,
               "each normal shrinks to the drawn ones' direction, x right and z out");
        Expect(small.roughness.rgba[0] == 100 && small.roughness.rgba[4] == 200 && small.roughness.rgba[8] == 200,
               "roughness shrinks the same way, carried into the clear block");
        Expect(std::abs(small.reflectivity - 128.f / 255.f) < 1e-3f, "reflectivity is what was drawn, averaged");
    }
    const std::string named = ide::card_file("Oak Tree/1", "color", "bytes");
    Expect(named.rfind("textures/billboards/Oak_Tree_1.color.", 0) == 0 && named.size() > 4 &&
               named.compare(named.size() - 4, 4, ".png") == 0,
           "a card's map is a file in textures/billboards, named after its Prefab and map");
    Expect(ide::card_file("Oak", "color", "bytes") == ide::card_file("Oak", "color", "bytes") &&
               ide::card_file("Oak", "color", "bytes") != ide::card_file("Oak", "color", "other bytes"),
           "the same map is the same file; a different one, another");
}

}  // namespace

// A long Model or asset name ends in an ellipsis inside its card.
void long_names_fit() {
    Rig rig;
    const std::string long_name = "Meshessmproptrafficbarrel011MtlWithAVeryLongNameIndeed";
    rig.game.set_name(rig.rock, long_name);
    std::string error;
    const InstanceId model = ide::add_model(rig.game, rig.crate, rig.rock, rig.wood, error);
    rig.game.set_name(model, long_name);
    rig.frames(3);
    jadefx::Node* card = rig.editor->cardNode(model);
    Expect(card != nullptr, "the long-named Model's card shows");
    if (card == nullptr) {
        return;
    }
    const double right = card->getAbsoluteX() + card->getWidth();
    int long_labels = 0;
    std::vector<jadefx::Node*> pending = {card};
    while (!pending.empty()) {
        jadefx::Node* node = pending.back();
        pending.pop_back();
        if (auto* label = dynamic_cast<jadefx::Label*>(node); label != nullptr && label->getText() == long_name) {
            ++long_labels;
            Expect(label->getAbsoluteX() + label->getWidth() <= right + 0.5, "a long name stays inside its card");
            Expect(label->displayedText() != long_name, "a long name is cut short with an ellipsis");
        }
        if (auto* parent = dynamic_cast<jadefx::Pane*>(node)) {
            for (const auto& child : parent->getChildren().items()) {
                pending.push_back(child.get());
            }
        }
    }
    Expect(long_labels == 2, "the card shows the long Model and Mesh names");
}

int main() {
    model_logic();
    guided_add();
    picker_keys_and_none();
    clear_select_delete_rename();
    drops();
    deleted_prefab();
    long_names_fit();
    lod_logic();
    lod_section();
    lod_card_bake();
    card_pixels();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failed\n", gFailures);
        return 1;
    }
    std::printf("prefab editor tests passed\n");
    return 0;
}
