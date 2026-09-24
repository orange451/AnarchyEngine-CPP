#include "IdePane.hpp"

namespace ide {

IdePane::IdePane(std::string name, bool closable) : name_(std::move(name)), closable_(closable) {
    setAlignment(jadefx::Pos::TopLeft);
    getClassList().add("ide-pane");
}

void Fill(jadefx::Node& node) { node.setStyle("width: 100%; height: 100%;"); }

}  // namespace ide
