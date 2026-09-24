#pragma once

#include "jadefx/jadefx.hpp"

#include <string>

namespace ide {

// One docked page. The dock shows `name` on the tab. A page that is not closable
// keeps its tab for the life of the shell.
class IdePane : public jadefx::StackPane {
public:
    IdePane(std::string name, bool closable);

    const std::string& name() const { return name_; }
    bool closable() const { return closable_; }

    virtual void onOpen() {}
    virtual void onClose() {}

private:
    std::string name_;
    bool closable_;
};

// Stretch a child across the page the dock gives this pane.
void Fill(jadefx::Node& node);

}  // namespace ide
