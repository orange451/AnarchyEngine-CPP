#include "IdeShaderEditor.hpp"

#include "IdeIcons.hpp"
#include "LockWaits.hpp"

#include "AssetInstances.hpp"
#include "DataModelLock.hpp"
#include "Shader.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace ide {
namespace {

constexpr const char* kEditorRules = R"CSS(
.shader-editor {
    background-color: var(--background-color);
}
.se-header {
    background-color: var(--surface-color);
    border-width: 0 0 1px 0;
    border-style: solid;
    border-color: var(--border-color);
    padding: 12px 20px;
    spacing: 12px;
}
.se-badge {
    background-color: var(--selection-color);
    border-radius: 9px;
}
.se-title {
    color: var(--ide-text-color);
    font-size: 17px;
    font-weight: bold;
}
.se-state {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.se-state-error {
    color: var(--ide-problems-error-color);
    font-size: 12px;
}
.se-body {
    padding: 16px;
    spacing: 16px;
}
.se-column {
    spacing: 12px;
}
.se-card {
    background-color: var(--surface-color);
    border-width: 1px;
    border-style: solid;
    border-color: var(--border-color);
    border-radius: 12px;
    padding: 12px;
    spacing: 8px;
}
.se-card-title {
    color: var(--ide-muted-text-color);
    font-size: 11px;
    font-weight: bold;
}
.se-row {
    spacing: 8px;
}
.se-name {
    color: var(--ide-text-color);
    font-weight: bold;
}
.se-mono {
    color: var(--ide-text-color);
    font-family: monospace;
}
.se-muted {
    color: var(--ide-muted-text-color);
    font-size: 12px;
}
.se-group {
    color: var(--ide-muted-text-color);
    font-size: 11px;
    font-weight: bold;
}
.se-primary {
    background-color: var(--accent-color);
    border-width: 0;
    border-radius: 7px;
    color: var(--surface-color);
    font-weight: bold;
    padding: 5px 12px;
}
.se-user {
    border-radius: 6px;
    padding: 4px 6px;
    spacing: 6px;
}
.se-user:hover {
    background-color: var(--selection-color);
}
.se-preview {
    background-color: #0c1a24;
    border-radius: 12px;
}
.se-empty-title {
    color: var(--ide-text-color);
    font-size: 15px;
    font-weight: bold;
}
)CSS";

std::shared_ptr<jadefx::Label> text_label(const std::string& text, const char* style_class) {
    auto label = jadefx::make<jadefx::Label>(text);
    label->getClassList().add(style_class);
    label->setMouseTransparent(true);
    return label;
}

std::shared_ptr<jadefx::Pane> spacer() {
    auto pane = jadefx::make<jadefx::Pane>();
    pane->setStyle("width: 100%;");
    pane->setMouseTransparent(true);
    return pane;
}

std::shared_ptr<jadefx::StackPane> icon_box(const std::string& file, double size, double box) {
    auto pane = jadefx::make<jadefx::StackPane>();
    pane->getClassList().add("se-badge");
    pane->setAlignment(jadefx::Pos::Center);
    pane->setPrefSize(box, box);
    pane->setMinSize(box, box);
    pane->setMaxSize(box, box);
    pane->setMouseTransparent(true);
    if (std::shared_ptr<jadefx::ImageView> icon = icon_file(file)) {
        icon->setPrefSize(size, size);
        icon->setMinSize(size, size);
        icon->setMaxSize(size, size);
        pane->getChildren().add(std::move(icon));
    }
    return pane;
}

std::shared_ptr<jadefx::VBox> card(const char* title) {
    auto box = jadefx::make<jadefx::VBox>();
    box->getClassList().add("se-card");
    box->setStyle("width: 100%;");
    box->getChildren().add(text_label(title, "se-card-title"));
    return box;
}

int line_count(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    const int newlines = static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    return text.back() == '\n' ? newlines : newlines + 1;
}

// A stage's error, "<line>: <message>", as "✕ line N".
std::string error_line(const std::string& error) {
    const std::size_t colon = error.find(':');
    return "\xE2\x9C\x95 line " + error.substr(0, colon);
}

const char* type_word(engine_core::ShaderParamType type) {
    switch (type) {
    case engine_core::ShaderParamType::Float:
        return "float";
    case engine_core::ShaderParamType::Int:
        return "int";
    case engine_core::ShaderParamType::Bool:
        return "bool";
    case engine_core::ShaderParamType::Vec2:
        return "vec2";
    case engine_core::ShaderParamType::Vec3:
        return "vec3";
    case engine_core::ShaderParamType::Color:
        return "color";
    case engine_core::ShaderParamType::Sampler:
        return "texture";
    }
    return "";
}

std::string number_text(double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%g", value);
    return text;
}

// A uniform's default as the pane shows it.
std::string default_text(const engine_core::ShaderParam& param) {
    const engine_core::LuaSlot& value = param.fallback;
    switch (param.type) {
    case engine_core::ShaderParamType::Float:
    case engine_core::ShaderParamType::Int:
        return number_text(value.number);
    case engine_core::ShaderParamType::Bool:
        return value.flag ? "true" : "false";
    case engine_core::ShaderParamType::Vec2:
        return number_text(value.vec.x) + ", " + number_text(value.vec.y);
    case engine_core::ShaderParamType::Vec3:
        return number_text(value.vec.x) + ", " + number_text(value.vec.y) + ", " + number_text(value.vec.z);
    case engine_core::ShaderParamType::Color:
        return number_text(value.color.r) + ", " + number_text(value.color.g) + ", " + number_text(value.color.b);
    case engine_core::ShaderParamType::Sampler:
        return param.black ? "black" : "white";
    }
    return "";
}

}  // namespace

IdeShaderEditor::IdeShaderEditor(engine_core::DataModel& world, engine_core::InstanceId shader, ShaderEditorHost host)
    : IdePane("Shader", true), world_(world), shader_(shader), host_(std::move(host)) {
    setIconFile("Shader.png");
    getClassList().add("shader-editor");
    setStylesheet(kEditorRules);
    setMinSize(320, 240);
    watch_ = world_.watch_changes(edited_.setter());
    world_.set_watched(watch_, {shader_});

    // The header: the Shader, its Kind, and whether it built.
    auto header = jadefx::make<jadefx::HBox>();
    header->getClassList().add("se-header");
    header->setAlignment(jadefx::Pos::CenterLeft);
    header->setStyle("width: 100%;");
    header->getChildren().add(icon_box("Shader.png", 22, 38));
    auto heading = jadefx::make<jadefx::VBox>();
    heading->setSpacing(1);
    title_ = text_label("", "se-title");
    state_ = text_label("", "se-state");
    heading->getChildren().add(title_);
    heading->getChildren().add(state_);
    header->getChildren().add(heading);
    kind_ = jadefx::make<jadefx::ComboBox>();
    kind_->getItems().add("Surface");
    kind_->getItems().add("Post");
    kind_->setOnAction([this](jadefx::ActionEvent&) {
        if (showing_kind_ || !host_.set_kind) {
            return;
        }
        host_.set_kind(shader_, kind_->getSelectionIndex() == 1 ? engine_core::ShaderKind::Post
                                                                 : engine_core::ShaderKind::Surface);
    });
    jadefx::Tooltip::install(kind_.get(), jadefx::make<jadefx::Tooltip>("What it draws: a Material's surface, or the finished frame"));
    header->getChildren().add(kind_);
    header->getChildren().add(spacer());

    // Left: the stages, then the render modes and the margin.
    auto left = jadefx::make<jadefx::VBox>();
    left->getClassList().add("se-column");
    left->setPrefWidth(250);
    left->setMinSize(220, 0);
    auto stages = card("STAGES");
    vertex_ = make_stage_row(engine_core::ShaderStage::Vertex);
    fragment_ = make_stage_row(engine_core::ShaderStage::Fragment);
    stages->getChildren().add(vertex_.root);
    stages->getChildren().add(fragment_.root);
    left->getChildren().add(stages);
    auto modes = card("RENDER MODES");
    const auto toggle = [this, &modes](const char* text, const char* property, std::shared_ptr<jadefx::CheckBox>& out) {
        out = jadefx::make<jadefx::CheckBox>(text);
        out->setOnAction([this, property, box = out.get()](jadefx::ActionEvent&) {
            if (host_.set_mode) {
                host_.set_mode(shader_, property, box->isSelected());
            }
        });
        modes->getChildren().add(out);
    };
    toggle("Transparent", "Transparent", transparent_);
    toggle("Two-sided", "TwoSided", two_sided_);
    toggle("Casts shadow", "CastsShadow", casts_shadow_);
    auto margin_row = jadefx::make<jadefx::HBox>();
    margin_row->getClassList().add("se-row");
    margin_row->setAlignment(jadefx::Pos::CenterLeft);
    margin_row->getChildren().add(text_label("Displacement margin", "se-muted"));
    margin_ = jadefx::make<jadefx::TextField>();
    margin_->setPrefWidth(64);
    margin_->setOnAction([this](jadefx::ActionEvent&) { commit_margin(); });
    margin_->setOnFocusChanged([this](bool focused) {
        if (!focused) {
            commit_margin();
        }
    });
    jadefx::Tooltip::install(margin_.get(),
                             jadefx::make<jadefx::Tooltip>("How far the vertex stage may move a vertex, in units"));
    margin_row->getChildren().add(margin_);
    modes->getChildren().add(margin_row);
    modes_card_ = modes;
    left->getChildren().add(modes);

    // Middle: the preview.
    preview_ = jadefx::make<jadefx::StackPane>();
    preview_->getClassList().add("se-preview");
    preview_->setStyle("width: 100%; height: 100%;");
    preview_->setMinSize(200, 200);

    // Right: the uniforms and the Materials using the Shader.
    auto right = jadefx::make<jadefx::VBox>();
    right->getClassList().add("se-column");
    right->setPrefWidth(250);
    right->setMinSize(220, 0);
    auto uniforms = card("UNIFORMS");
    uniforms_ = jadefx::make<jadefx::VBox>();
    uniforms_->setSpacing(4);
    uniforms->getChildren().add(uniforms_);
    right->getChildren().add(uniforms);
    auto users = card("USED BY");
    users_ = jadefx::make<jadefx::VBox>();
    users_->setSpacing(2);
    users->getChildren().add(users_);
    right->getChildren().add(users);

    auto body = jadefx::make<jadefx::HBox>();
    body->getClassList().add("se-body");
    Fill(*body);
    body->getChildren().add(left);
    body->getChildren().add(preview_);
    body->getChildren().add(right);
    body_ = body;

    auto gone = jadefx::make<jadefx::VBox>();
    gone->setAlignment(jadefx::Pos::Center);
    Fill(*gone);
    gone->getChildren().add(text_label("This Shader no longer exists", "se-empty-title"));
    gone->getChildren().add(text_label("It was deleted, or the place it was in was closed.", "se-muted"));
    gone->setVisible(false);
    gone_ = gone;

    auto canvas = jadefx::make<jadefx::StackPane>();
    canvas->setAlignment(jadefx::Pos::TopLeft);
    Fill(*canvas);
    canvas->getChildren().add(body);
    canvas->getChildren().add(gone);

    auto root = jadefx::make<jadefx::BorderPane>();
    Fill(*root);
    root->setTop(header);
    root->setCenter(canvas);
    getChildren().add(root);
}

IdeShaderEditor::~IdeShaderEditor() { world_.unwatch_changes(watch_); }

IdeShaderEditor::StageRow IdeShaderEditor::make_stage_row(engine_core::ShaderStage stage) {
    const bool vertex = stage == engine_core::ShaderStage::Vertex;
    StageRow row;
    auto box = jadefx::make<jadefx::VBox>();
    box->setSpacing(4);
    auto top = jadefx::make<jadefx::HBox>();
    top->getClassList().add("se-row");
    top->setAlignment(jadefx::Pos::CenterLeft);
    top->setStyle("width: 100%;");
    top->getChildren().add(text_label(vertex ? "Vertex" : "Fragment", "se-name"));
    top->getChildren().add(spacer());
    row.state = text_label("", "se-state");
    top->getChildren().add(row.state);
    box->getChildren().add(top);
    auto button = jadefx::make<jadefx::Button>(vertex ? "Edit vertex.glsl" : "Edit fragment.glsl");
    button->getClassList().add("se-primary");
    button->setOnAction([this, stage](jadefx::ActionEvent&) {
        if (stage == engine_core::ShaderStage::Vertex && !view_.has_vertex) {
            if (host_.add_vertex_stage) {
                host_.add_vertex_stage(shader_);
            }
            return;
        }
        if (host_.edit_stage) {
            host_.edit_stage(shader_, stage);
        }
    });
    jadefx::Tooltip::install(button.get(), jadefx::make<jadefx::Tooltip>(
                                               vertex ? "Moves each vertex every frame: waves, wind, wobble"
                                                      : "Colors each pixel: albedo, normal, roughness, glow"));
    box->getChildren().add(button);
    row.button = button;
    row.root = box;
    return row;
}

jadefx::Label* IdeShaderEditor::stageState(engine_core::ShaderStage stage) const {
    return (stage == engine_core::ShaderStage::Vertex ? vertex_ : fragment_).state.get();
}

jadefx::Node* IdeShaderEditor::stageButton(engine_core::ShaderStage stage) const {
    const StageRow& row = stage == engine_core::ShaderStage::Vertex ? vertex_ : fragment_;
    return row.root && row.root->isVisible() ? row.button.get() : nullptr;
}

jadefx::CheckBox* IdeShaderEditor::modeToggle(const std::string& property) const {
    if (property == "Transparent") {
        return transparent_.get();
    }
    if (property == "TwoSided") {
        return two_sided_.get();
    }
    if (property == "CastsShadow") {
        return casts_shadow_.get();
    }
    return nullptr;
}

std::vector<std::string> IdeShaderEditor::uniformNames() const { return uniform_names_; }

std::vector<std::string> IdeShaderEditor::usedByNames() const {
    std::vector<std::string> out;
    for (const auto& [id, name] : view_.users) {
        out.push_back(name);
    }
    return out;
}

jadefx::Node* IdeShaderEditor::usedByRow(engine_core::InstanceId material) const {
    for (const auto& [id, row] : user_rows_) {
        if (id == material) {
            return row.get();
        }
    }
    return nullptr;
}

void IdeShaderEditor::layoutChildren() {
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kFrameLockWait);
        if (lock.owns()) {
            refresh();
        }
    }
    IdePane::layoutChildren();
    // The Shader and its users change on their own, so the editor checks them again next frame.
    markLayoutDirty(LayoutDirt::Arrange);
}

void IdeShaderEditor::refresh() {
    const std::uint64_t tree = world_.tree_revision();
    const std::uint64_t authored = world_.authored_revision();
    // Taken before the read, so a change made while reading reads again next frame.
    const bool edited = edited_.take();
    if (tree == seen_tree_ && authored == seen_authored_ && !edited) {
        return;
    }
    seen_tree_ = tree;
    seen_authored_ = authored;
    View next = read();
    const bool same = next.alive == view_.alive && next.name == view_.name && next.kind == view_.kind &&
                      next.error == view_.error && next.vertex_error == view_.vertex_error &&
                      next.fragment_error == view_.fragment_error && next.has_vertex == view_.has_vertex &&
                      next.vertex_lines == view_.vertex_lines && next.fragment_lines == view_.fragment_lines &&
                      next.transparent == view_.transparent && next.two_sided == view_.two_sided &&
                      next.casts_shadow == view_.casts_shadow && next.margin == view_.margin &&
                      next.users == view_.users && next.params.size() == view_.params.size() &&
                      std::equal(next.params.begin(), next.params.end(), view_.params.begin(),
                                 [](const engine_core::ShaderParam& a, const engine_core::ShaderParam& b) {
                                     return a.name == b.name && a.type == b.type && a.group == b.group &&
                                            default_text(a) == default_text(b);
                                 });
    if (same && !title_->getText().empty()) {
        return;
    }
    view_ = std::move(next);
    show();
}

IdeShaderEditor::View IdeShaderEditor::read() const {
    View out;
    const auto* shader = dynamic_cast<const engine_core::Shader*>(world_.instance(shader_));
    if (shader == nullptr) {
        return out;
    }
    out.alive = true;
    out.name = world_.name(shader_);
    out.kind = shader->kind();
    out.error = shader->error();
    out.vertex_error = shader->vertex_error();
    out.fragment_error = shader->fragment_error();
    out.has_vertex = shader->has_vertex_stage();
    out.vertex_lines = line_count(shader->vertex_source());
    out.fragment_lines = line_count(shader->source());
    out.transparent = shader->transparent();
    out.two_sided = shader->two_sided();
    out.casts_shadow = shader->casts_shadow();
    out.margin = shader->displacement_margin();
    out.params = shader->parse()->params;
    // Every Material using it, wherever it is under Materials.
    std::vector<engine_core::InstanceId> pending{world_.service("Materials")};
    while (!pending.empty()) {
        const engine_core::InstanceId at = pending.back();
        pending.pop_back();
        for (engine_core::InstanceId child = world_.first_child(at); child != 0; child = world_.next_sibling(child)) {
            if (const auto* material = dynamic_cast<const engine_core::Material*>(world_.instance(child))) {
                if (material->shader(world_) == shader) {
                    out.users.emplace_back(child, world_.name(child));
                }
            }
            pending.push_back(child);
        }
    }
    std::sort(out.users.begin(), out.users.end(),
              [](const auto& a, const auto& b) { return a.second < b.second || (a.second == b.second && a.first < b.first); });
    return out;
}

void IdeShaderEditor::show() {
    body_->setVisible(view_.alive);
    gone_->setVisible(!view_.alive);
    if (!view_.alive) {
        title_->setText("Shader");
        state_->setText("Deleted");
        return;
    }
    setTitle(view_.name);
    title_->setText(view_.name);
    const bool broken = !view_.error.empty();
    state_->setText(broken ? view_.error : std::string("Built"));
    state_->getClassList().removeIf([](const std::string& name) { return name == "se-state" || name == "se-state-error"; });
    state_->getClassList().add(broken ? "se-state-error" : "se-state");
    showing_kind_ = true;
    kind_->select(view_.kind == engine_core::ShaderKind::Post ? 1 : 0);
    showing_kind_ = false;
    const bool surface = view_.kind == engine_core::ShaderKind::Surface;
    vertex_.root->setVisible(surface);
    modes_card_->setVisible(surface);
    show_stage(vertex_, engine_core::ShaderStage::Vertex);
    show_stage(fragment_, engine_core::ShaderStage::Fragment);
    transparent_->setSelected(view_.transparent);
    two_sided_->setSelected(view_.two_sided);
    casts_shadow_->setSelected(view_.casts_shadow);
    if (!margin_->isFocused()) {
        margin_->setText(number_text(view_.margin));
    }
    show_uniforms();
    show_users();
}

void IdeShaderEditor::show_stage(StageRow& row, engine_core::ShaderStage stage) {
    const bool vertex = stage == engine_core::ShaderStage::Vertex;
    auto* button = dynamic_cast<jadefx::Button*>(row.button.get());
    if (vertex && !view_.has_vertex) {
        row.state->setText("none");
        button->setText("Add vertex stage");
        return;
    }
    button->setText(vertex ? "Edit vertex.glsl" : "Edit fragment.glsl");
    const std::string& error = vertex ? view_.vertex_error : view_.fragment_error;
    const int lines = vertex ? view_.vertex_lines : view_.fragment_lines;
    row.state->setText(error.empty() ? "built \xC2\xB7 " + std::to_string(lines) + " lines" : error_line(error));
    jadefx::Tooltip::install(row.state.get(), error.empty() ? nullptr : jadefx::make<jadefx::Tooltip>(error));
}

void IdeShaderEditor::show_uniforms() {
    uniforms_->getChildren().clear();
    uniform_names_.clear();
    std::string group = "\x01";
    for (const engine_core::ShaderParam& param : view_.params) {
        if (param.hidden) {
            continue;
        }
        if (param.group != group) {
            group = param.group;
            if (!group.empty()) {
                uniforms_->getChildren().add(text_label(group, "se-group"));
            }
        }
        auto row = jadefx::make<jadefx::HBox>();
        row->getClassList().add("se-row");
        row->setStyle("width: 100%;");
        row->getChildren().add(text_label(param.name, "se-mono"));
        row->getChildren().add(text_label(type_word(param.type), "se-muted"));
        row->getChildren().add(spacer());
        row->getChildren().add(text_label(default_text(param), "se-muted"));
        uniforms_->getChildren().add(row);
        uniform_names_.push_back(param.name);
    }
    if (uniform_names_.empty()) {
        uniforms_->getChildren().add(text_label("None yet: declare a uniform in either stage", "se-muted"));
    }
}

void IdeShaderEditor::show_users() {
    users_->getChildren().clear();
    user_rows_.clear();
    for (const auto& [id, name] : view_.users) {
        auto row = jadefx::make<jadefx::HBox>();
        row->getClassList().add("se-user");
        row->setAlignment(jadefx::Pos::CenterLeft);
        row->setStyle("width: 100%;");
        row->getChildren().add(text_label(name, "se-name"));
        row->getChildren().add(spacer());
        row->getChildren().add(text_label("Material", "se-muted"));
        row->setCursor(jadefx::Cursor::Pointer);
        const engine_core::InstanceId material = id;
        row->setOnMouseClicked([this, material](const jadefx::MouseEvent& event) {
            if (event.button == 0 && host_.select) {
                host_.select(material);
            }
        });
        users_->getChildren().add(row);
        user_rows_.emplace_back(id, row);
    }
    if (view_.users.empty()) {
        users_->getChildren().add(text_label("No Material uses it yet", "se-muted"));
    }
}

void IdeShaderEditor::commit_margin() {
    const std::string text = margin_->getText();
    char* end = nullptr;
    const double units = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(units) || units < 0.0) {
        margin_->setText(number_text(view_.margin));
        return;
    }
    if (units != view_.margin && host_.set_margin) {
        host_.set_margin(shader_, units);
    }
}

}  // namespace ide
