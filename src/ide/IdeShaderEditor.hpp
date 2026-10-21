#pragma once

#include "ChangeFlag.hpp"
#include "IdePane.hpp"
#include "MaterialBall.hpp"

#include "DataModel.hpp"
#include "ShaderSource.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jadefx {
class CheckBox;
class ComboBox;
class ImageView;
class Label;
class Menu;
class TextField;
class VBox;
}  // namespace jadefx

namespace ide {

// What the Shader Editor asks of the studio. Each write is one undo step.
struct ShaderEditorHost {
    std::function<void(engine_core::InstanceId shader, engine_core::ShaderKind kind)> set_kind;
    // property is "Transparent", "TwoSided", or "CastsShadow".
    std::function<void(engine_core::InstanceId shader, const char* property, bool on)> set_mode;
    std::function<void(engine_core::InstanceId shader, double units)> set_margin;
    // Opens a stage's text in the shader text editor.
    std::function<void(engine_core::InstanceId shader, engine_core::ShaderStage stage)> edit_stage;
    // Fills the vertex stage with its starter text, then opens it.
    std::function<void(engine_core::InstanceId shader)> add_vertex_stage;
    // Selects a Material the Shader is used by.
    std::function<void(engine_core::InstanceId material)> select;
};

// One Shader's home, docked beside the scene view as the Prefab editor is.
// Double-clicking a Shader, or Edit, opens it; each stage's text opens from it.
//
// The header names the Shader and holds its Kind and whether it built: Built,
// or the first error of either stage. On the left, the stages: a Vertex row
// and a Fragment row, each with its state (built and how many lines, or the
// line its error is on) and a button that opens it in the text editor. A
// surface Shader with no vertex text offers Add vertex stage; a post Shader
// has no vertex row. Under them, the render modes as toggles and the
// displacement margin. In the middle, a preview of the Shader drawn on a
// mesh. On the right, the uniforms it declares with their defaults, by group,
// and the Materials that use it: clicking one selects it, so Properties shows
// its uniform rows; under a post Shader, which no Material can use, each is
// marked as drawing plain. A deleted Shader leaves a note in the page.
class IdeShaderEditor : public IdePane {
public:
    IdeShaderEditor(engine_core::DataModel& world, engine_core::InstanceId shader, ShaderEditorHost host = {});
    ~IdeShaderEditor() override;

    engine_core::InstanceId shader() const { return shader_; }

    // For tests: the widgets a person would read or click.
    jadefx::Label* titleLabel() const { return title_.get(); }
    jadefx::ComboBox* kindBox() const { return kind_.get(); }
    jadefx::Label* stateLabel() const { return state_.get(); }
    jadefx::Label* stageState(engine_core::ShaderStage stage) const;
    jadefx::Node* stageButton(engine_core::ShaderStage stage) const;
    // property is "Transparent", "TwoSided", "CastsShadow", or "Hdr".
    jadefx::CheckBox* modeToggle(const std::string& property) const;
    jadefx::TextField* marginField() const { return margin_.get(); }
    std::vector<std::string> uniformNames() const;
    std::vector<std::string> usedByNames() const;
    jadefx::Node* usedByRow(engine_core::InstanceId material) const;
    jadefx::Node* goneState() const { return gone_.get(); }
    // Where the preview draws; dragging across it turns the mesh.
    jadefx::Node* previewNode() const { return preview_.get(); }
    PreviewMesh previewMesh() const { return preview_mesh_; }
    jadefx::Node* meshButton(PreviewMesh mesh) const;
    // The preview's yaw and pitch, radians.
    std::pair<float, float> rotation() const { return {yaw_, pitch_}; }
    // Whether the preview draws at now: renderContent asks, draws, then says
    // so with previewDrawn. Public for tests, which have no GL to draw with.
    bool previewDue(std::chrono::steady_clock::time_point now) const;
    void previewDrawn(std::chrono::steady_clock::time_point now);

protected:
    void layoutChildren() override;
    // Draws the preview: GL is current while the pane paints.
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void sceneChanged(jadefx::Scene* previous) override;

    // What the pane last read of the Shader.
    struct View {
        bool alive = false;
        std::string name;
        engine_core::ShaderKind kind = engine_core::ShaderKind::Surface;
        std::string error;
        std::string vertex_error;
        std::string fragment_error;
        bool has_vertex = false;
        int vertex_lines = 0;
        int fragment_lines = 0;
        bool transparent = false;
        bool two_sided = false;
        bool casts_shadow = true;
        bool hdr = false;
        double margin = 1.0;
        std::vector<engine_core::ShaderParam> params;
        // What uses it, by name: a Material naming it, or a ShaderEffect.
        struct User {
            engine_core::InstanceId id = 0;
            std::string name;
            bool effect = false;
            bool operator==(const User& other) const {
                return id == other.id && name == other.name && effect == other.effect;
            }
        };
        std::vector<User> users;
    };
    const View& view() const { return view_; }
    engine_core::DataModel& world() const { return world_; }

private:
    struct StageRow {
        std::shared_ptr<jadefx::Node> root;
        std::shared_ptr<jadefx::Label> state;
        std::shared_ptr<jadefx::Node> button;
    };

    // Reads the Shader again when the tree, an authored edit, or the Shader moved.
    void refresh();
    View read() const;
    void show();
    void show_stage(StageRow& row, engine_core::ShaderStage stage);
    void show_uniforms();
    void show_users();
    StageRow make_stage_row(engine_core::ShaderStage stage);
    void commit_margin();
    void pick_mesh(PreviewMesh mesh);
    void show_mesh_menu();
    // Turns the preview by a drag of dx, dy points.
    void turn(double dx, double dy);

    engine_core::DataModel& world_;
    engine_core::InstanceId shader_;
    ShaderEditorHost host_;
    std::uint64_t watch_ = 0;
    ChangeFlag edited_;
    std::uint64_t seen_tree_ = ~0ull;
    std::uint64_t seen_authored_ = ~0ull;
    View view_;
    bool showing_kind_ = false;

    std::shared_ptr<jadefx::Label> title_;
    std::shared_ptr<jadefx::ComboBox> kind_;
    std::shared_ptr<jadefx::Label> state_;
    StageRow vertex_;
    StageRow fragment_;
    std::shared_ptr<jadefx::Node> modes_card_;
    std::shared_ptr<jadefx::CheckBox> transparent_;
    std::shared_ptr<jadefx::CheckBox> two_sided_;
    std::shared_ptr<jadefx::CheckBox> casts_shadow_;
    std::shared_ptr<jadefx::CheckBox> hdr_;
    std::shared_ptr<jadefx::Node> margin_row_;
    std::shared_ptr<jadefx::TextField> margin_;
    std::shared_ptr<jadefx::VBox> uniforms_;
    std::shared_ptr<jadefx::VBox> users_;
    std::vector<std::pair<engine_core::InstanceId, std::shared_ptr<jadefx::Node>>> user_rows_;
    std::vector<std::string> uniform_names_;
    std::shared_ptr<jadefx::Node> body_;
    std::shared_ptr<jadefx::Node> gone_;

protected:
    std::shared_ptr<jadefx::StackPane> preview_;

private:
    std::shared_ptr<jadefx::ImageView> preview_image_;
    std::unique_ptr<MaterialBall> ball_;
    std::shared_ptr<const engine_core::SurfaceBinding> preview_binding_;
    PreviewMesh preview_mesh_ = PreviewMesh::Sphere;
    std::string preview_path_;
    float yaw_ = 0.6f;
    float pitch_ = -0.35f;
    std::chrono::steady_clock::time_point drawn_{};
    // Something the preview shows changed since it last drew.
    bool preview_stale_ = true;
    std::vector<std::pair<PreviewMesh, std::shared_ptr<jadefx::Node>>> mesh_buttons_;
    // The place's Meshes with a file: name and Path, for Mesh...
    std::vector<std::pair<std::string, std::string>> meshes_;
    std::shared_ptr<jadefx::Menu> mesh_menu_;
};

}  // namespace ide
