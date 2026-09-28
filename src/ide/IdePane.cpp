#include "IdePane.hpp"

namespace ide {

IdePane::IdePane(std::string name, bool closable) : name_(std::move(name)), closable_(closable) {
    setAlignment(jadefx::Pos::TopLeft);
    getClassList().add("ide-pane");
}

void IdePane::setTitle(std::string title) {
    if (title == name_) {
        title.clear();
    }
    if (title == title_) {
        return;
    }
    title_ = std::move(title);
    if (on_title_) {
        on_title_(this->title());
    }
}

void Fill(jadefx::Node& node) { node.setStyle("width: 100%; height: 100%;"); }

}  // namespace ide
