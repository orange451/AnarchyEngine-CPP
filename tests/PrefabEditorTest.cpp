#include "ide/IdePrefabEditor.hpp"
#include "ide/PrefabModels.hpp"
#include "SelectionService.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "LuaApi.hpp"
#include "ScriptRuntime.hpp"
#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The Prefab editor in a headless scene, and the model logic under it.
namespace {

using engine_core::InstanceId;
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
// hold Wood and Metal; and whose Prefabs hold Crate, with no Models yet. The
// host writes straight to the game, standing in for the simulation thread.
struct Rig {
    SimRole role;
    engine_core::ScriptRuntime runtime;
    engine_core::Game game;
    InstanceId rock = 0;
    InstanceId lid = 0;
    InstanceId wood = 0;
    InstanceId metal = 0;
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

}  // namespace

int main() {
    model_logic();
    guided_add();
    picker_keys_and_none();
    clear_select_delete_rename();
    drops();
    deleted_prefab();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failed\n", gFailures);
        return 1;
    }
    std::printf("prefab editor tests passed\n");
    return 0;
}
