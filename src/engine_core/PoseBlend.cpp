#include "PoseBlend.hpp"

namespace engine_core {

void PoseBlend::begin_capture() {
    entries_.clear();
    index_.clear();
    capturing_ = true;
}

void PoseBlend::record(InstanceId id, Kind kind, const Matrix4& before, const Matrix4& after) {
    if (!capturing_) {
        return;
    }
    const auto found = index_.find(id);
    if (found != index_.end()) {
        entries_[found->second].after = after;
        return;
    }
    index_.emplace(id, entries_.size());
    entries_.push_back(Entry{id, kind, before, after});
}

void PoseBlend::clear() {
    entries_.clear();
    index_.clear();
    armed_ = false;
    capturing_ = false;
}

Matrix4 PoseBlend::blended(const Entry& entry, const Matrix4& current, double alpha) {
    if (alpha >= 1.0) {
        return current;
    }
    const Matrix4 drawn = matrix4_lerp(entry.before, entry.after, alpha < 0.0 ? 0.0 : alpha);
    return matrix4_multiply(matrix4_multiply(drawn, matrix4_inverse(entry.after)), current);
}

}  // namespace engine_core
