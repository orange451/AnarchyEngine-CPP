#pragma once

#include "SnapshotPump.hpp"

namespace engine_core {

// Stand-in for the GPU submit. perform() receives the front snapshot only.
// It must not touch a DataModel.
class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void perform(const VisualSnapshot& front, int color_batches) = 0;
    virtual void present() = 0;
};

}  // namespace engine_core
