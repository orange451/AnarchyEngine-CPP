#include "ide/IdeShaderEditor.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Game.hpp"
#include "LuaApi.hpp"
#include "ScriptRuntime.hpp"
#include "SelectionService.hpp"
#include "Shader.hpp"
#include "ShaderEffect.hpp"
#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The Shader Editor in a headless scene: its header, stages, modes, uniforms,
// and the Materials that use it.
namespace {

using engine_core::InstanceId;
using engine_core::ShaderKind;
using engine_core::ShaderStage;

int gFailures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        ++gFailures;
    }
}

constexpr double kWidth = 1200;
constexpr double kHeight = 720;

struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

// A place whose Shaders hold Wave (two uniforms, no vertex stage), and whose
// Materials hold Ocean and Lake, which use Wave, and Rock, which does not.
// The host writes straight to the game, standing in for the simulation thread.
struct Rig {
    SimRole role;
    engine_core::ScriptRuntime runtime;
    engine_core::Game game;
    InstanceId wave = 0;
    InstanceId ocean = 0;
    InstanceId lake = 0;
    InstanceId rock = 0;
    std::vector<std::pair<InstanceId, ShaderStage>> edited;
    std::vector<InstanceId> selected;
    std::vector<std::pair<std::string, bool>> modes;
    std::shared_ptr<ide::IdeShaderEditor> editor;
    std::shared_ptr<jadefx::Scene> scene;
    double time = 0;

    engine_core::Shader& shader() { return *dynamic_cast<engine_core::Shader*>(game.instance(wave)); }

    Rig() {
        engine_core::Shader& made = game.create<engine_core::Shader>();
        game.set_name(made.id(), "Wave");
        game.set_parent(made.id(), game.service("Shaders"));
        made.set_source("group Waves;\nuniform float Height : range(0, 4) = 1.5;\nuniform vec3 Deep : color = vec3(0.0, 0.1, 0.2);\n"
                        "void surface() {\n    ALBEDO = Deep;\n}\n");
        wave = made.id();
        ocean = material("Ocean", wave);
        lake = material("Lake", wave);
        rock = material("Rock", 0);
        open(wave);
    }

    // The editor on id, in a fresh scene.
    void open(InstanceId id) {
        ide::ShaderEditorHost host;
        host.set_kind = [this](InstanceId id, ShaderKind kind) {
            dynamic_cast<engine_core::Shader*>(game.instance(id))->set_kind(kind);
        };
        host.set_mode = [this](InstanceId id, const char* property, bool on) {
            auto* shader = dynamic_cast<engine_core::Shader*>(game.instance(id));
            const std::string name = property;
            modes.emplace_back(name, on);
            if (name == "Hdr") {
                shader->set_hdr(on);
            } else if (name == "Transparent") {
                shader->set_transparent(on);
            } else if (name == "TwoSided") {
                shader->set_two_sided(on);
            } else if (name == "CastsShadow") {
                shader->set_casts_shadow(on);
            }
        };
        host.set_margin = [this](InstanceId id, double units) {
            dynamic_cast<engine_core::Shader*>(game.instance(id))->set_displacement_margin(units);
        };
        host.edit_stage = [this](InstanceId id, ShaderStage stage) { edited.emplace_back(id, stage); };
        host.add_vertex_stage = [this](InstanceId id) {
            dynamic_cast<engine_core::Shader*>(game.instance(id))->set_vertex_source(engine_core::kShaderVertexStarterSource);
            edited.emplace_back(id, ShaderStage::Vertex);
        };
        host.select = [this](InstanceId id) { selected.push_back(id); };
        editor = jadefx::make<ide::IdeShaderEditor>(game, id, std::move(host));
        editor->setPrefWidthRatio(1);
        editor->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(editor, kWidth, kHeight);
        frames(2);
    }

    InstanceId material(const char* name, InstanceId shader) {
        engine_core::Material& made = game.create<engine_core::Material>();
        game.set_name(made.id(), name);
        game.set_parent(made.id(), game.service("Materials"));
        if (shader != 0) {
            engine_core::LuaSlot ref;
            ref.kind = engine_core::LuaSlot::Kind::Instance;
            ref.id = shader;
            made.set_reference(engine_core::Material::kShaderReference, ref);
        }
        return made.id();
    }

    void frames(int count = 1) {
        for (int i = 0; i < count; ++i) {
            scene->layout(kWidth, kHeight, time);
            time += 0.05;
        }
    }

    void click(jadefx::Node* node) {
        Expect(node != nullptr, "the node to click is on screen");
        if (node == nullptr) {
            return;
        }
        const double x = node->getAbsoluteX() + node->getWidth() * 0.5;
        const double y = node->getAbsoluteY() + node->getHeight() * 0.5;
        scene->noteMove(x, y);
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
        frames();
    }
};

std::string text_of(jadefx::Node* node) {
    if (auto* label = dynamic_cast<jadefx::Labeled*>(node)) {
        return label->getText();
    }
    return std::string();
}

// Every label's text under node, in order.
std::vector<std::string> texts_under(jadefx::Node* node) {
    std::vector<std::string> out;
    if (auto* label = dynamic_cast<jadefx::Labeled*>(node)) {
        out.push_back(label->getText());
    }
    if (auto* pane = dynamic_cast<jadefx::Pane*>(node)) {
        for (const auto& child : pane->getChildren().items()) {
            for (std::string& text : texts_under(child.get())) {
                out.push_back(std::move(text));
            }
        }
    }
    return out;
}

void header_and_state() {
    Rig rig;
    Expect(rig.editor->title() == "Wave", "the tab is the Shader's name");
    Expect(text_of(rig.editor->titleLabel()) == "Wave", "the header names the Shader");
    Expect(rig.editor->kindBox() != nullptr && rig.editor->kindBox()->getValue() == "Surface", "Kind shows Surface");
    Expect(text_of(rig.editor->stateLabel()) == "Built", "a Shader with no error reads Built (" +
                                                             text_of(rig.editor->stateLabel()) + ")");
    rig.shader().set_source("uniform float A;\n");
    rig.frames(2);
    const std::string state = text_of(rig.editor->stateLabel());
    Expect(state.find("fragment 1") != std::string::npos, "an error shows in the header (" + state + ")");
    Expect(text_of(rig.editor->stageState(ShaderStage::Fragment)).find("line 1") != std::string::npos,
           "and on the Fragment row (" + text_of(rig.editor->stageState(ShaderStage::Fragment)) + ")");
    // Kind from the dropdown goes through the host.
    // Down on the closed dropdown picks the next kind, as a person would.
    rig.editor->kindBox()->requestFocus();
    rig.scene->noteKey(jadefx::Key::Down, true, false, 0);
    rig.frames(2);
    Expect(rig.shader().kind() == ShaderKind::Post, "picking Post sets Kind");
    Expect(rig.editor->stageButton(ShaderStage::Vertex) == nullptr || !rig.editor->stageButton(ShaderStage::Vertex)->isVisible(),
           "a post Shader shows no vertex stage");
}

void stages() {
    Rig rig;
    jadefx::Node* vertex = rig.editor->stageButton(ShaderStage::Vertex);
    Expect(text_of(rig.editor->stageState(ShaderStage::Vertex)) == "none", "no vertex stage reads none");
    Expect(text_of(vertex) == "Add vertex stage", "and offers to add one (" + text_of(vertex) + ")");
    rig.click(vertex);
    rig.frames(2);
    Expect(rig.shader().has_vertex_stage(), "Add vertex stage fills in the starter");
    Expect(!rig.edited.empty() && rig.edited.back().second == ShaderStage::Vertex, "and opens the vertex text");
    const std::string built = text_of(rig.editor->stageState(ShaderStage::Vertex));
    Expect(built.find("built") != std::string::npos && built.find("lines") != std::string::npos,
           "the Vertex row then reads built and its lines (" + built + ")");
    Expect(text_of(rig.editor->stageButton(ShaderStage::Vertex)) == "Edit", "its button edits it (" +
                                                                                text_of(rig.editor->stageButton(ShaderStage::Vertex)) + ")");
    Expect(text_of(rig.editor->stageButton(ShaderStage::Fragment)) == "Edit", "and so does the Fragment row's (" +
                                                                                  text_of(rig.editor->stageButton(ShaderStage::Fragment)) + ")");
    rig.edited.clear();
    rig.click(rig.editor->stageButton(ShaderStage::Fragment));
    Expect(rig.edited.size() == 1 && rig.edited[0].first == rig.wave && rig.edited[0].second == ShaderStage::Fragment,
           "Edit fragment opens the fragment text");
}

void modes_and_margin() {
    Rig rig;
    jadefx::CheckBox* two_sided = rig.editor->modeToggle("TwoSided");
    jadefx::CheckBox* shadow = rig.editor->modeToggle("CastsShadow");
    Expect(two_sided != nullptr && !two_sided->isSelected(), "Two-sided starts off");
    Expect(shadow != nullptr && shadow->isSelected(), "Casts shadow starts on");
    rig.click(two_sided);
    rig.frames(2);
    Expect(rig.shader().two_sided(), "the toggle sets TwoSided");
    rig.shader().set_casts_shadow(false);
    rig.frames(2);
    Expect(shadow != nullptr && !shadow->isSelected(), "the toggle follows the property");
    jadefx::TextField* margin = rig.editor->marginField();
    Expect(margin != nullptr && margin->getText() == "1", "the margin shows 1");
    if (margin != nullptr) {
        margin->requestFocus();
        margin->setText("3.5");
        rig.scene->noteKey(jadefx::Key::Enter, true, false, 0);
        rig.frames(2);
        Expect(rig.shader().displacement_margin() == 3.5, "typing a margin sets it");
    }
}

void uniforms_and_used_by() {
    Rig rig;
    const std::vector<std::string> rows = rig.editor->uniformNames();
    Expect(rows == std::vector<std::string>{"Height", "Deep"}, "the uniforms are listed in order");
    rig.shader().set_vertex_source("uniform float Speed = 2.0;\nvoid vertex() {}\n");
    rig.frames(2);
    Expect(rig.editor->uniformNames() == std::vector<std::string>{"Height", "Deep", "Speed"},
           "a vertex-stage uniform joins the list");
    const std::vector<std::string> users = rig.editor->usedByNames();
    Expect(users == std::vector<std::string>{"Lake", "Ocean"}, "Used by lists the Materials that use it");
    jadefx::Node* lake = rig.editor->usedByRow(rig.lake);
    rig.click(lake);
    Expect(rig.selected == std::vector<InstanceId>{rig.lake}, "clicking one selects it");
    // A Material that stops using it drops off.
    rig.game.instance(rig.ocean);
    dynamic_cast<engine_core::Material*>(rig.game.instance(rig.ocean))
        ->set_reference(engine_core::Material::kShaderReference, engine_core::LuaSlot{});
    rig.frames(2);
    Expect(rig.editor->usedByNames() == std::vector<std::string>{"Lake"}, "and one that stops using it drops off");
    // A post Shader: the Material still names it but draws plain, so it stays, marked.
    rig.shader().set_kind(ShaderKind::Post);
    rig.frames(2);
    Expect(rig.editor->usedByNames() == std::vector<std::string>{"Lake"}, "a post Shader still lists its Materials");
    const std::vector<std::string> marked = texts_under(rig.editor->usedByRow(rig.lake));
    Expect(std::find(marked.begin(), marked.end(), "Can't use post") != marked.end(),
           "marked as unable to use a post shader");
    rig.shader().set_kind(ShaderKind::Surface);
    rig.frames(2);
    const std::vector<std::string> plain = texts_under(rig.editor->usedByRow(rig.lake));
    Expect(std::find(plain.begin(), plain.end(), "Material") != plain.end(), "and back to a surface, plain again");
}

// The preview draws when what it shows changes, and over and over only for a
// Shader that reads TIME: a still one costs nothing while the pane is open.
void preview_redraws() {
    Rig rig;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    Expect(rig.editor->previewDue(start), "the preview draws when the pane opens");
    rig.editor->previewDrawn(start);
    Expect(!rig.editor->previewDue(start + std::chrono::seconds(1)), "a still Shader is not drawn again");
    // A change elsewhere in the place changes nothing it shows.
    rig.game.set_name(rig.rock, "Stone");
    rig.frames(2);
    Expect(!rig.editor->previewDue(start + std::chrono::seconds(1)), "a rename elsewhere draws nothing");
    rig.click(rig.editor->meshButton(ide::PreviewMesh::Cube));
    Expect(rig.editor->previewDue(start + std::chrono::seconds(1)), "picking a mesh draws it");
    rig.editor->previewDrawn(start + std::chrono::seconds(1));
    rig.shader().set_source("void surface() {\n    EMISSIVE = vec3(sin(TIME));\n}\n");
    rig.frames(2);
    const Clock::time_point edited = start + std::chrono::seconds(2);
    Expect(rig.editor->previewDue(edited), "a new source draws it");
    rig.editor->previewDrawn(edited);
    Expect(!rig.editor->previewDue(edited + std::chrono::milliseconds(10)) &&
               rig.editor->previewDue(edited + std::chrono::milliseconds(60)),
           "a Shader that reads TIME draws again every 50 ms");
}

void preview_controls() {
    Rig rig;
    Expect(rig.editor->previewMesh() == ide::PreviewMesh::Sphere, "the preview starts on the sphere");
    rig.click(rig.editor->meshButton(ide::PreviewMesh::Cube));
    Expect(rig.editor->previewMesh() == ide::PreviewMesh::Cube, "the mesh bar's Cube picks the cube");
    jadefx::Node* preview = rig.editor->previewNode();
    Expect(preview != nullptr, "the preview is on screen");
    if (preview == nullptr) {
        return;
    }
    const auto before = rig.editor->rotation();
    const double x = preview->getAbsoluteX() + preview->getWidth() * 0.5;
    const double y = preview->getAbsoluteY() + preview->getHeight() * 0.5;
    rig.scene->noteMove(x, y);
    rig.scene->noteButton(0, true, x, y);
    for (int step = 1; step <= 6; ++step) {
        rig.scene->noteMove(x + 20 * step, y + 5 * step);
        rig.frames();
    }
    rig.scene->noteButton(0, false, x + 120, y + 30);
    rig.frames();
    const auto after = rig.editor->rotation();
    Expect(after.first != before.first && after.second != before.second, "a drag across the preview turns it");
}

// A post Shader: an Hdr toggle in place of the surface modes, the ShaderEffects
// that use it under Used by, and a preview that draws.
void post_shader_editor() {
    Rig rig;
    engine_core::Shader& fade = rig.game.create<engine_core::Shader>();
    rig.game.set_name(fade.id(), "Fade");
    rig.game.set_parent(fade.id(), rig.game.service("Shaders"));
    fade.set_kind(ShaderKind::Post);
    fade.set_source("uniform float Strength : range(0, 1) = 0.5;\nvoid post() {\n    COLOR.rgb *= 1.0 - Strength;\n}\n");
    engine_core::ShaderEffect& vignette = rig.game.create<engine_core::ShaderEffect>();
    rig.game.set_name(vignette.id(), "Vignette");
    rig.game.set_parent(vignette.id(), rig.game.scene_service("Lighting"));
    engine_core::LuaSlot ref;
    ref.kind = engine_core::LuaSlot::Kind::Instance;
    ref.id = fade.id();
    vignette.set_reference(engine_core::ShaderEffect::kShaderReference, ref);
    Expect(rig.editor->modeToggle("Hdr") == nullptr || !rig.editor->modeToggle("Hdr")->isVisible(),
           "a surface Shader shows no Hdr toggle");
    rig.open(fade.id());
    Expect(rig.editor->uniformNames() == std::vector<std::string>{"Strength"}, "a post Shader lists its uniforms");
    Expect(rig.editor->usedByNames() == std::vector<std::string>{"Vignette"}, "Used by lists the ShaderEffects that use it");
    const std::vector<std::string> tagged = texts_under(rig.editor->usedByRow(vignette.id()));
    Expect(std::find(tagged.begin(), tagged.end(), "ShaderEffect") != tagged.end(), "tagged as a ShaderEffect");
    rig.click(rig.editor->usedByRow(vignette.id()));
    Expect(rig.selected == std::vector<InstanceId>{vignette.id()}, "clicking one selects it");
    jadefx::CheckBox* hdr = rig.editor->modeToggle("Hdr");
    Expect(hdr != nullptr && hdr->isVisible(), "a post Shader shows the Hdr toggle");
    Expect(rig.editor->modeToggle("Transparent") == nullptr || !rig.editor->modeToggle("Transparent")->isVisible(),
           "and none of the surface toggles");
    if (hdr != nullptr) {
        Expect(!hdr->isSelected(), "Hdr starts off");
        const auto drawn = std::chrono::steady_clock::now();
        rig.editor->previewDrawn(drawn);
        rig.click(hdr);
        rig.frames(2);
        Expect(rig.modes == std::vector<std::pair<std::string, bool>>{{"Hdr", true}}, "clicking it sets Hdr");
        Expect(hdr->isSelected(), "and the toggle follows the Shader");
        Expect(rig.editor->previewDue(drawn), "and the preview draws again, in the other stage");
    }
    Expect(rig.editor->previewDue(std::chrono::steady_clock::now()), "the preview draws a post Shader too");
}

void deleted() {
    Rig rig;
    rig.game.destroy(rig.wave);
    rig.frames(2);
    Expect(rig.editor->goneState() != nullptr && rig.editor->goneState()->isVisible(), "a deleted Shader says so");
}

}  // namespace

int main() {
    header_and_state();
    stages();
    modes_and_margin();
    uniforms_and_used_by();
    post_shader_editor();
    preview_controls();
    preview_redraws();
    deleted();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failed\n", gFailures);
        return 1;
    }
    std::printf("shader editor pane tests passed\n");
    return 0;
}
