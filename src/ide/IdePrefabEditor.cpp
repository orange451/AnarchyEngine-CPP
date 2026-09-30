#include "IdePrefabEditor.hpp"
#include "LockWaits.hpp"

#include "AssetInstances.hpp"
#include "DataModelLock.hpp"

#include <utility>

namespace ide {

namespace {

constexpr const char* kPrefabEditorRules = R"CSS(
.prefab-editor {
    background-color: var(--ide-panel-color);
}
.prefab-editor-body {
    padding: 16px;
}
.prefab-editor-heading {
    color: var(--ide-text-color);
    font-size: 16px;
}
.prefab-editor-detail {
    color: var(--ide-search-status-color);
    font-size: 12px;
}
)CSS";

std::shared_ptr<jadefx::Label> label(const char* style_class) {
    auto made = jadefx::make<jadefx::Label>("");
    made->getClassList().add(style_class);
    made->setMouseTransparent(true);
    return made;
}

}  // namespace

IdePrefabEditor::IdePrefabEditor(engine_core::DataModel& world, engine_core::InstanceId prefab)
    : IdePane("Prefab", true), world_(world), prefab_(prefab) {
    setIconFile("ModelAlt.png");
    getClassList().add("prefab-editor");
    setStylesheet(kPrefabEditorRules);
    setMinSize(200, 140);

    heading_ = label("prefab-editor-heading");
    detail_ = label("prefab-editor-detail");
    auto body = jadefx::make<jadefx::VBox>();
    body->getClassList().add("prefab-editor-body");
    body->setSpacing(6);
    Fill(*body);
    body->getChildren().add(heading_);
    body->getChildren().add(detail_);
    getChildren().add(body);
    refresh();
}

void IdePrefabEditor::layoutChildren() {
    refresh();
    IdePane::layoutChildren();
}

void IdePrefabEditor::refresh() {
    const std::uint64_t tree = world_.tree_revision();
    if (tree == seen_tree_) {
        return;
    }
    std::string name;
    bool alive = false;
    {
        engine_core::DataModelLock lock(world_, engine_core::DataModelLock::Read, kFrameLockWait);
        if (!lock.owns()) {
            // Busy this frame; the next one reads it.
            return;
        }
        const engine_core::DataModel* object = world_.instance(prefab_);
        alive = dynamic_cast<const engine_core::Prefab*>(object) != nullptr;
        if (alive) {
            name = world_.name(prefab_);
        }
    }
    seen_tree_ = tree;
    if (alive) {
        setTitle(name);
        heading_->setText("Editing Prefab \"" + name + "\"");
        detail_->setText("The Prefab editor is a placeholder for now.");
    } else {
        heading_->setText("This Prefab no longer exists.");
        detail_->setText("It was deleted, or the place it was in was closed.");
    }
}

}  // namespace ide
