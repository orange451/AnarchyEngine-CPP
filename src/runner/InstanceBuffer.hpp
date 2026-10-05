#pragma once

#include "DrawBatches.hpp"

namespace runner {

// One frame's InstanceData in a GL buffer, and the vertex slots that read it.
// Every call needs the GL context it was made in. destroy before that context goes.
class InstanceBuffer {
public:
    // Replaces the buffer's contents with count rows. Each upload gets new
    // storage (glBufferData, GL_STREAM_DRAW), so draws still reading the last
    // upload never stall the CPU. count 0 keeps what is there.
    void upload(const InstanceData* data, int count);
    // With a mesh's vertex array bound: enables slots 7 to 14 at divisor 1,
    // reading from row first on. GL 3.3 has no base instance, so each run attaches.
    void attach(int first) const;
    void destroy();

private:
    unsigned buffer_ = 0;
};

}  // namespace runner
