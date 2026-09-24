#pragma once

#include "IdePane.hpp"

#include <string>

namespace ide {

// A project tree. The rows are a static stand-in until this is bound to a DataModel.
class IdeExplorer : public IdePane {
public:
    explicit IdeExplorer(std::string name);
};

}  // namespace ide
