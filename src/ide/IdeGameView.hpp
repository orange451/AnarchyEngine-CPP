#pragma once

#include "IdePane.hpp"

namespace ide {

// The viewport. The runner's picture will land here later. This page stays open.
class IdeGameView : public IdePane {
public:
    IdeGameView();
};

}  // namespace ide
