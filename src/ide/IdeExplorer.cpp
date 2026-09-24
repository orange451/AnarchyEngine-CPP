#include "IdeExplorer.hpp"

namespace ide {
namespace {

std::shared_ptr<jadefx::TreeItem> Branch(const char* name) {
    auto branch = jadefx::make<jadefx::TreeItem>(name);
    branch->setExpanded(true);
    branch->getChildren().add(jadefx::make<jadefx::TreeItem>("Child"));
    branch->getChildren().add(jadefx::make<jadefx::TreeItem>("Child"));
    return branch;
}

}  // namespace

IdeExplorer::IdeExplorer(std::string name) : IdePane(std::move(name), true) {
    setPrefWidth(240);
    setMinSize(150, 80);

    auto root = jadefx::make<jadefx::TreeItem>("root");
    root->setExpanded(true);
    root->getChildren().add(Branch("Folder"));
    root->getChildren().add(Branch("Model"));
    root->getChildren().add(Branch("Script"));

    auto tree = jadefx::make<jadefx::TreeView>(root);
    tree->setShowRoot(false);
    tree->setFixedCellSize(24);
    Fill(*tree);
    getChildren().add(tree);
}

}  // namespace ide
