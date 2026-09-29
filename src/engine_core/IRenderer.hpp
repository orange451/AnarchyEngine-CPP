#pragma once

namespace engine_core {

// Forward declared, so the renderer interface does not compile the world model.
struct VisualSnapshot;

// Stand-in for the GPU submit. perform() receives the front snapshot only.
// It must not touch a DataModel.
class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual void perform(const VisualSnapshot& front) = 0;
    virtual void present() = 0;
};

}  // namespace engine_core
