#include "IdeTreeTest.hpp"

namespace ide {

IdeTreeTest::IdeTreeTest() : IdePane("I'm a tree", true) {
    setPrefWidth(240);
    setMinSize(150, 80);

    auto root = jadefx::make<jadefx::TreeItem>("root");
    root->setExpanded(true);
    for (int i = 1; i < 8; ++i) {
        auto item = jadefx::make<jadefx::TreeItem>("Message" + std::to_string(i));
        item->setExpanded(true);
        for (int j = 1; j < 4; ++j) {
            item->getChildren().add(jadefx::make<jadefx::TreeItem>("Message" + std::to_string(j)));
        }
        root->getChildren().add(std::move(item));
    }

    auto tree = jadefx::make<jadefx::TreeView>(root);
    tree->setShowRoot(false);
    tree->setFixedCellSize(24);
    Fill(*tree);
    getChildren().add(tree);
}

}  // namespace ide
