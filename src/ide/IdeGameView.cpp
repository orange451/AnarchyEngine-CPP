#include "IdeGameView.hpp"

namespace ide {

IdeGameView::IdeGameView() : IdePane("Scene View", false) {
    setMinSize(64, 64);
    getClassList().add("ide-viewport");
    setBackground(jadefx::Color::rgb8(30, 30, 30));
}

}  // namespace ide
