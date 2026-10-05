#include "InstanceBuffer.hpp"

#include "amesh.hpp"
#include "gl.hpp"

#include <cstddef>

namespace runner {

void InstanceBuffer::upload(const InstanceData* data, int count) {
    if (count <= 0) {
        return;
    }
    if (buffer_ == 0) {
        glGenBuffers(1, &buffer_);
    }
    glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(InstanceData) * static_cast<std::size_t>(count)),
                 data, RT_GL_STREAM_DRAW);
}

void InstanceBuffer::attach(int first) const {
    glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    const auto stride = static_cast<GLsizei>(sizeof(InstanceData));
    const std::size_t base = sizeof(InstanceData) * static_cast<std::size_t>(first);
    const auto slot = [&](unsigned location, GLint size, std::size_t offset) {
        glEnableVertexAttribArray(location);
        glVertexAttribPointer(location, size, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(base + offset));
        glVertexAttribDivisor(location, 1);
    };
    for (unsigned column = 0; column < 4; ++column) {
        slot(anarchy::amesh::kAttribInstanceModel + column, 4, offsetof(InstanceData, model) + column * 16);
    }
    for (unsigned column = 0; column < 3; ++column) {
        slot(anarchy::amesh::kAttribInstanceNormal + column, 3, offsetof(InstanceData, normal) + column * 12);
    }
    slot(anarchy::amesh::kAttribInstanceTint, 3, offsetof(InstanceData, tint));
}

void InstanceBuffer::destroy() {
    if (buffer_ != 0) {
        glDeleteBuffers(1, &buffer_);
        buffer_ = 0;
    }
}

}  // namespace runner
