#pragma once

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace ide {

// A line of cells at their own widths, except one that takes what is left, so
// a long name ends in an ellipsis instead of pushing the rest out.
class StretchRow : public jadefx::HBox {
public:
    explicit StretchRow(std::size_t stretch) : stretch_(stretch) {
        setAlignment(jadefx::Pos::CenterLeft);
        setSpacing(5);
    }

protected:
    double preferredContentWidth(double innerAvailable) const override {
        const double wanted = HBox::preferredContentWidth(innerAvailable);
        return innerAvailable > 0 ? std::min(wanted, innerAvailable) : wanted;
    }

    void layoutChildren() override {
        const std::vector<std::shared_ptr<jadefx::Node>>& children = getChildren().items();
        const double gap = getSpacing();
        const double height = contentHeight();
        std::vector<double> widths(children.size(), 0);
        double used = 0;
        for (std::size_t index = 0; index < children.size(); ++index) {
            if (index != stretch_ && children[index]) {
                widths[index] = children[index]->measuredWidth(contentWidth());
                used += widths[index];
            }
            if (index > 0) {
                used += gap;
            }
        }
        if (stretch_ < children.size()) {
            widths[stretch_] = std::max(0.0, contentWidth() - used);
        }
        double x = contentLeft();
        for (std::size_t index = 0; index < children.size(); ++index) {
            if (jadefx::Node* child = children[index].get()) {
                const double child_height = std::min(child->measuredHeight(widths[index], height), height);
                child->performLayout(x, contentTop() + (height - child_height) * 0.5, widths[index], child_height);
            }
            x += widths[index] + gap;
        }
    }

private:
    std::size_t stretch_;
};

}  // namespace ide
