#pragma once

#include "jadefx/scene/Node.hpp"

#include <string>

namespace ide {

inline bool has_class(const jadefx::Node& node, const char* name) {
    for (const std::string& item : node.getClassList().items()) {
        if (item == name) {
            return true;
        }
    }
    return false;
}

// Adds or removes a style class. Does nothing when the node already matches.
inline void set_class(jadefx::Node& node, const char* name, bool on) {
    if (has_class(node, name) == on) {
        return;
    }
    if (on) {
        node.getClassList().add(name);
    } else {
        node.getClassList().removeIf([name](const std::string& item) { return item == name; });
    }
}

}  // namespace ide
