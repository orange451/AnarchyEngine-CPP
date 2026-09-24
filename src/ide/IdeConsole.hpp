#pragma once

#include "IdePane.hpp"

namespace ide {

// Output log with a command line under it. Nothing is executed yet.
class IdeConsole : public IdePane {
public:
    IdeConsole();
};

}  // namespace ide
