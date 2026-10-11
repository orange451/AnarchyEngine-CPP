#pragma once

// AANIM: an animation clip as keyframes. Little-endian, ending in a CRC32 of
// everything before it.
//
//     AAHeader    header                                  32 bytes
//     u8          names[name_blob_size]                   u16 length + UTF-8 bytes, each name once
//     AAKeyframe  keyframes[keyframe_count]               16 bytes each, sorted by time
//     AAPose      poses[pose_count]                       52 bytes each, keyframe by keyframe
//     u32         crc32 of bytes [0, size - 4)            zlib CRC-32 (amesh::crc32)
//
// A pose is a change from its bone's rest local transform:
// animated_local = rest_local * T(position) * R(rotation) * S(scale), with
// rotation a unit quaternion (x, y, z, w). Its easing shapes the way from it
// to the same bone's pose in the next keyframe, and its weight (0 to 1) is
// how much of the bone the clip moves, so a weight of 0 masks the bone out.
// Easing values are Roblox's Enum.EasingStyle and Enum.EasingDirection.

#include "amesh.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace anarchy::aanim {

using ByteSpan = anarchy::amesh::ByteSpan;

inline constexpr char kMagic[4] = {'A', 'A', 'N', 'M'};
inline constexpr std::uint16_t kVersionMajor = 1;
inline constexpr std::uint16_t kVersionMinor = 0;
inline constexpr std::uint16_t FLAG_LOOPED = 1u << 0;
inline constexpr std::size_t kHeaderSize = 32;
inline constexpr std::size_t kKeyframeSize = 16;
inline constexpr std::size_t kPoseSize = 52;
// Where a pose record keeps its easing style and its weight.
inline constexpr std::size_t kPoseStyleOffset = 44;
inline constexpr std::size_t kPoseWeightOffset = 48;
inline constexpr std::size_t kMaxFileSize = std::size_t{256} << 20;

enum class EasingStyle : std::uint8_t {
    Linear = 0,
    Constant = 1,
    Sine = 2,
    Quad = 3,
    Cubic = 4,
    Quart = 5,
    Quint = 6,
    Exponential = 7,
    Circular = 8,
    Back = 9,
    Elastic = 10,
    Bounce = 11,
};
inline constexpr std::uint8_t kEasingStyleCount = 12;

enum class EasingDirection : std::uint8_t { In = 0, Out = 1, InOut = 2 };
inline constexpr std::uint8_t kEasingDirectionCount = 3;

struct Pose {
    std::string bone;
    float position[3] = {0.f, 0.f, 0.f};
    float rotation[4] = {0.f, 0.f, 0.f, 1.f};
    float scale[3] = {1.f, 1.f, 1.f};
    EasingStyle style = EasingStyle::Linear;
    EasingDirection direction = EasingDirection::In;
    float weight = 1.f;
};

struct Keyframe {
    // Seconds from the start of the clip, 0 or more.
    float time = 0.f;
    std::string name;
    std::vector<Pose> poses;
};

struct Data {
    std::string name;
    bool looped = false;
    // Sorted by time; two may share one.
    std::vector<Keyframe> keyframes;
};

// A malformed file, or Data that cannot be written. byte_offset is where in
// the file (or the file write() was building) the problem is.
class AnimError : public std::runtime_error {
public:
    AnimError(std::uint64_t byte_offset, std::string reason);

    std::uint64_t byte_offset;
    std::string reason;
};

// Throws AnimError.
Data read(ByteSpan bytes);
// Throws AnimError when data breaks a rule a reader would refuse.
std::vector<std::byte> write(const Data& data);

// The header's name_blob_size, for tests that edit a file by hand. 0 when the
// bytes are too short to have a header.
std::size_t name_blob_size_of(const std::vector<std::byte>& bytes);

}  // namespace anarchy::aanim
