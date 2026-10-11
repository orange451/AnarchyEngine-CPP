#include "aanim.hpp"

#include <cmath>
#include <cstring>
#include <map>

namespace anarchy::aanim {
namespace {

// Header field offsets.
constexpr std::size_t kVersionAt = 4;
constexpr std::size_t kFlagsAt = 8;
constexpr std::size_t kKeyframeCountAt = 12;
constexpr std::size_t kPoseCountAt = 16;
constexpr std::size_t kNameBlobAt = 20;
constexpr std::size_t kClipNameAt = 24;

std::string Num(std::uint64_t value) { return std::to_string(value); }

template <typename T>
T Load(ByteSpan bytes, std::uint64_t at) {
    T value;
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

template <typename T>
void Store(std::vector<std::byte>& out, std::uint64_t at, T value) {
    std::memcpy(out.data() + at, &value, sizeof(T));
}

bool Finite(const float* values, int count) {
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) {
            return false;
        }
    }
    return true;
}

// The name at offset in the blob: a u16 length, then its bytes.
std::string NameAt(ByteSpan bytes, std::uint64_t blob, std::uint64_t blob_size, std::uint32_t offset,
                   std::uint64_t record_at, const char* what) {
    if (std::uint64_t{offset} + 2 > blob_size) {
        throw AnimError(record_at, std::string(what) + "'s name runs past the name blob");
    }
    const auto length = Load<std::uint16_t>(bytes, blob + offset);
    if (std::uint64_t{offset} + 2 + length > blob_size) {
        throw AnimError(record_at, std::string(what) + "'s name runs past the name blob");
    }
    std::string name(reinterpret_cast<const char*>(bytes.data() + blob + offset + 2), length);
    if (name.find('\0') != std::string::npos) {
        throw AnimError(blob + offset, std::string(what) + "'s name holds a NUL");
    }
    return name;
}

// What a pose or keyframe may hold, for read and write alike. Empty when it may.
std::string PoseProblem(const Pose& pose) {
    if (!Finite(pose.position, 3) || !Finite(pose.rotation, 4) || !Finite(pose.scale, 3) ||
        !std::isfinite(pose.weight)) {
        return "has a value that is not finite";
    }
    if (pose.weight < 0.f || pose.weight > 1.f) {
        return "has weight " + std::to_string(pose.weight) + ", outside 0 to 1";
    }
    const float length = std::sqrt(pose.rotation[0] * pose.rotation[0] + pose.rotation[1] * pose.rotation[1] +
                                   pose.rotation[2] * pose.rotation[2] + pose.rotation[3] * pose.rotation[3]);
    if (!(length > 1e-6f)) {
        return "has a zero rotation";
    }
    if (static_cast<std::uint8_t>(pose.style) >= kEasingStyleCount) {
        return "has easing style " + Num(static_cast<std::uint8_t>(pose.style)) + ", which is not one";
    }
    if (static_cast<std::uint8_t>(pose.direction) >= kEasingDirectionCount) {
        return "has easing direction " + Num(static_cast<std::uint8_t>(pose.direction)) + ", which is not one";
    }
    return std::string();
}

}  // namespace

AnimError::AnimError(std::uint64_t at, std::string why)
    : std::runtime_error("AANIM at byte " + std::to_string(at) + ": " + why), byte_offset(at), reason(std::move(why)) {}

std::size_t name_blob_size_of(const std::vector<std::byte>& bytes) {
    if (bytes.size() < kHeaderSize) {
        return 0;
    }
    return Load<std::uint32_t>(ByteSpan(bytes.data(), bytes.size()), kNameBlobAt);
}

Data read(ByteSpan bytes) {
    if (bytes.size() < kHeaderSize + 4) {
        throw AnimError(0, "the file is too short to be AANIM");
    }
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        throw AnimError(0, "the file does not start with AANM");
    }
    const auto major = Load<std::uint16_t>(bytes, kVersionAt);
    if (major != kVersionMajor) {
        throw AnimError(kVersionAt, "version " + Num(major) + " is not one this reader knows");
    }
    if (bytes.size() > kMaxFileSize) {
        throw AnimError(0, "the file is larger than 256 MiB");
    }
    const std::uint32_t stored = Load<std::uint32_t>(bytes, bytes.size() - 4);
    if (anarchy::amesh::crc32(bytes.first(bytes.size() - 4)) != stored) {
        throw AnimError(bytes.size() - 4, "the CRC does not match the file");
    }
    const auto flags = Load<std::uint16_t>(bytes, kFlagsAt);
    const auto keyframe_count = Load<std::uint32_t>(bytes, kKeyframeCountAt);
    const auto pose_count = Load<std::uint32_t>(bytes, kPoseCountAt);
    const auto blob_size = Load<std::uint32_t>(bytes, kNameBlobAt);
    const std::uint64_t blob = kHeaderSize;
    const std::uint64_t keyframes = blob + blob_size;
    const std::uint64_t poses = keyframes + std::uint64_t{keyframe_count} * kKeyframeSize;
    const std::uint64_t end = poses + std::uint64_t{pose_count} * kPoseSize;
    if (end + 4 != bytes.size()) {
        throw AnimError(kKeyframeCountAt, "the counts do not add up to the file's size");
    }

    Data data;
    data.looped = (flags & FLAG_LOOPED) != 0;
    data.name = NameAt(bytes, blob, blob_size, Load<std::uint32_t>(bytes, kClipNameAt), kClipNameAt, "the clip");
    data.keyframes.resize(keyframe_count);
    std::uint32_t next_pose = 0;
    float last_time = 0.f;
    for (std::uint32_t k = 0; k < keyframe_count; ++k) {
        const std::uint64_t at = keyframes + std::uint64_t{k} * kKeyframeSize;
        Keyframe& keyframe = data.keyframes[k];
        keyframe.time = Load<float>(bytes, at);
        const std::string label = "keyframe " + Num(k);
        if (!std::isfinite(keyframe.time) || keyframe.time < 0.f || (k > 0 && keyframe.time < last_time)) {
            throw AnimError(at, label + "'s time is not finite, below 0, or before the one ahead of it");
        }
        last_time = keyframe.time;
        keyframe.name = NameAt(bytes, blob, blob_size, Load<std::uint32_t>(bytes, at + 4), at + 4, label.c_str());
        const auto first = Load<std::uint32_t>(bytes, at + 8);
        const auto count = Load<std::uint32_t>(bytes, at + 12);
        if (first != next_pose || std::uint64_t{first} + count > pose_count) {
            throw AnimError(at + 8, label + "'s poses do not follow the keyframe before it");
        }
        next_pose = first + count;
        keyframe.poses.resize(count);
        for (std::uint32_t p = 0; p < count; ++p) {
            const std::uint64_t pose_at = poses + std::uint64_t{first + p} * kPoseSize;
            Pose& pose = keyframe.poses[p];
            pose.bone = NameAt(bytes, blob, blob_size, Load<std::uint32_t>(bytes, pose_at), pose_at,
                               ("pose " + Num(first + p)).c_str());
            std::memcpy(pose.position, bytes.data() + pose_at + 4, sizeof(pose.position));
            std::memcpy(pose.rotation, bytes.data() + pose_at + 16, sizeof(pose.rotation));
            std::memcpy(pose.scale, bytes.data() + pose_at + 32, sizeof(pose.scale));
            pose.style = static_cast<EasingStyle>(Load<std::uint8_t>(bytes, pose_at + kPoseStyleOffset));
            pose.direction = static_cast<EasingDirection>(Load<std::uint8_t>(bytes, pose_at + kPoseStyleOffset + 1));
            pose.weight = Load<float>(bytes, pose_at + kPoseWeightOffset);
            const std::string problem = PoseProblem(pose);
            if (!problem.empty()) {
                throw AnimError(pose_at, "pose " + Num(first + p) + " " + problem);
            }
            // Made unit length when it is not, leaving one that is exactly as written.
            const float length = std::sqrt(pose.rotation[0] * pose.rotation[0] + pose.rotation[1] * pose.rotation[1] +
                                           pose.rotation[2] * pose.rotation[2] + pose.rotation[3] * pose.rotation[3]);
            if (std::fabs(length - 1.f) > 1e-4f) {
                for (float& component : pose.rotation) {
                    component /= length;
                }
            }
        }
    }
    if (next_pose != pose_count) {
        throw AnimError(kPoseCountAt, "poses " + Num(next_pose) + " on belong to no keyframe");
    }
    return data;
}

std::vector<std::byte> write(const Data& data) {
    // Every name once, in the blob.
    std::vector<std::byte> blob;
    std::map<std::string, std::uint32_t> offsets;
    const auto name = [&](const std::string& text) -> std::uint32_t {
        const auto found = offsets.find(text);
        if (found != offsets.end()) {
            return found->second;
        }
        if (text.size() > 0xFFFF) {
            throw AnimError(0, "a name is longer than 65535 bytes");
        }
        const auto offset = static_cast<std::uint32_t>(blob.size());
        const auto length = static_cast<std::uint16_t>(text.size());
        blob.resize(blob.size() + 2 + text.size());
        std::memcpy(blob.data() + offset, &length, 2);
        std::memcpy(blob.data() + offset + 2, text.data(), text.size());
        offsets.emplace(text, offset);
        return offset;
    };
    const std::uint32_t clip_name = name(data.name);
    std::size_t pose_total = 0;
    for (std::size_t k = 0; k < data.keyframes.size(); ++k) {
        const Keyframe& keyframe = data.keyframes[k];
        if (!std::isfinite(keyframe.time) || keyframe.time < 0.f ||
            (k > 0 && keyframe.time < data.keyframes[k - 1].time)) {
            throw AnimError(0, "keyframe " + Num(k) + "'s time is not finite, below 0, or before the one ahead of it");
        }
        for (std::size_t p = 0; p < keyframe.poses.size(); ++p) {
            const std::string problem = PoseProblem(keyframe.poses[p]);
            if (!problem.empty()) {
                throw AnimError(0, "keyframe " + Num(k) + "'s pose " + Num(p) + " " + problem);
            }
        }
        pose_total += keyframe.poses.size();
    }
    std::vector<std::uint32_t> keyframe_names(data.keyframes.size());
    std::vector<std::uint32_t> pose_names;
    pose_names.reserve(pose_total);
    for (std::size_t k = 0; k < data.keyframes.size(); ++k) {
        keyframe_names[k] = name(data.keyframes[k].name);
        for (const Pose& pose : data.keyframes[k].poses) {
            pose_names.push_back(name(pose.bone));
        }
    }

    const std::size_t keyframes = kHeaderSize + blob.size();
    const std::size_t poses = keyframes + data.keyframes.size() * kKeyframeSize;
    const std::size_t size = poses + pose_total * kPoseSize + 4;
    if (size > kMaxFileSize) {
        throw AnimError(0, "the clip is larger than 256 MiB");
    }
    std::vector<std::byte> out(size, std::byte{0});
    std::memcpy(out.data(), kMagic, sizeof(kMagic));
    Store(out, kVersionAt, kVersionMajor);
    Store(out, kVersionAt + 2, kVersionMinor);
    Store(out, kFlagsAt, static_cast<std::uint16_t>(data.looped ? FLAG_LOOPED : 0));
    Store(out, kKeyframeCountAt, static_cast<std::uint32_t>(data.keyframes.size()));
    Store(out, kPoseCountAt, static_cast<std::uint32_t>(pose_total));
    Store(out, kNameBlobAt, static_cast<std::uint32_t>(blob.size()));
    Store(out, kClipNameAt, clip_name);
    if (!blob.empty()) {
        std::memcpy(out.data() + kHeaderSize, blob.data(), blob.size());
    }
    std::uint32_t first = 0;
    std::size_t written = 0;
    for (std::size_t k = 0; k < data.keyframes.size(); ++k) {
        const Keyframe& keyframe = data.keyframes[k];
        const std::size_t at = keyframes + k * kKeyframeSize;
        Store(out, at, keyframe.time);
        Store(out, at + 4, keyframe_names[k]);
        Store(out, at + 8, first);
        Store(out, at + 12, static_cast<std::uint32_t>(keyframe.poses.size()));
        for (const Pose& pose : keyframe.poses) {
            const std::size_t pose_at = poses + written * kPoseSize;
            Store(out, pose_at, pose_names[written]);
            std::memcpy(out.data() + pose_at + 4, pose.position, sizeof(pose.position));
            std::memcpy(out.data() + pose_at + 16, pose.rotation, sizeof(pose.rotation));
            std::memcpy(out.data() + pose_at + 32, pose.scale, sizeof(pose.scale));
            Store(out, pose_at + kPoseStyleOffset, static_cast<std::uint8_t>(pose.style));
            Store(out, pose_at + kPoseStyleOffset + 1, static_cast<std::uint8_t>(pose.direction));
            Store(out, pose_at + kPoseWeightOffset, pose.weight);
            ++written;
        }
        first += static_cast<std::uint32_t>(keyframe.poses.size());
    }
    Store(out, size - 4, anarchy::amesh::crc32(ByteSpan(out.data(), size - 4)));
    return out;
}

}  // namespace anarchy::aanim
