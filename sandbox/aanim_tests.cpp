// AANIM: the keyframe clip format. Round trips, and the reader's refusals.

#include "aanim.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace aanim = anarchy::aanim;

aanim::Pose pose(const char* bone, float x, aanim::EasingStyle style, float weight = 1.f) {
    aanim::Pose out;
    out.bone = bone;
    out.position[0] = x;
    out.rotation[0] = 0.f, out.rotation[1] = 0.f, out.rotation[2] = 0.70710678f, out.rotation[3] = 0.70710678f;
    out.scale[1] = 2.f;
    out.style = style;
    out.direction = aanim::EasingDirection::InOut;
    out.weight = weight;
    return out;
}

aanim::Data sample() {
    aanim::Data data;
    data.name = "Walk";
    data.looped = true;
    aanim::Keyframe first;
    first.time = 0.f;
    first.name = "Start";
    first.poses = {pose("Hips", 1.f, aanim::EasingStyle::Linear), pose("Head", 2.f, aanim::EasingStyle::Bounce, 0.5f)};
    aanim::Keyframe second;
    second.time = 1.25f;
    second.name = "";
    second.poses = {pose("Hips", 3.f, aanim::EasingStyle::Constant)};
    data.keyframes = {first, second};
    return data;
}

aanim::Data read_back(const std::vector<std::byte>& bytes) {
    return aanim::read(aanim::ByteSpan(bytes.data(), bytes.size()));
}

// The reason a refusal gives, or empty when it reads.
std::string refusal(const std::vector<std::byte>& bytes) {
    try {
        read_back(bytes);
    } catch (const aanim::AnimError& error) {
        return error.reason;
    }
    return std::string();
}

// The CRC again after editing bytes in place, so a test reaches the check it means.
void reseal(std::vector<std::byte>& bytes) {
    const std::uint32_t crc =
        anarchy::amesh::crc32(anarchy::amesh::ByteSpan(bytes.data(), bytes.size() - 4));
    std::memcpy(bytes.data() + bytes.size() - 4, &crc, 4);
}

}  // namespace

TEST_CASE("AN1 a clip round-trips field for field", "[aanim]") {
    const aanim::Data data = sample();
    const aanim::Data back = read_back(aanim::write(data));
    REQUIRE(back.name == "Walk");
    REQUIRE(back.looped);
    REQUIRE(back.keyframes.size() == 2);
    REQUIRE(back.keyframes[0].name == "Start");
    REQUIRE(back.keyframes[1].name.empty());
    REQUIRE(back.keyframes[1].time == 1.25f);
    REQUIRE(back.keyframes[0].poses.size() == 2);
    const aanim::Pose& head = back.keyframes[0].poses[1];
    REQUIRE(head.bone == "Head");
    REQUIRE(head.position[0] == 2.f);
    REQUIRE(head.rotation[2] == 0.70710678f);
    REQUIRE(head.scale[1] == 2.f);
    REQUIRE(head.style == aanim::EasingStyle::Bounce);
    REQUIRE(head.direction == aanim::EasingDirection::InOut);
    REQUIRE(head.weight == 0.5f);
    REQUIRE(back.keyframes[1].poses[0].style == aanim::EasingStyle::Constant);
}

TEST_CASE("AN2 an empty clip round-trips", "[aanim]") {
    aanim::Data data;
    data.name = "Nothing";
    const aanim::Data back = read_back(aanim::write(data));
    REQUIRE(back.name == "Nothing");
    REQUIRE_FALSE(back.looped);
    REQUIRE(back.keyframes.empty());
}

TEST_CASE("AN3 the writer refuses what a reader would", "[aanim]") {
    aanim::Data unsorted = sample();
    std::swap(unsorted.keyframes[0], unsorted.keyframes[1]);
    REQUIRE_THROWS_AS(aanim::write(unsorted), aanim::AnimError);
    aanim::Data heavy = sample();
    heavy.keyframes[0].poses[0].weight = 1.5f;
    REQUIRE_THROWS_AS(aanim::write(heavy), aanim::AnimError);
    aanim::Data endless = sample();
    endless.keyframes[0].time = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(aanim::write(endless), aanim::AnimError);
}

TEST_CASE("AN4 the reader refuses a bad magic, version, CRC, time, weight, easing, or length", "[aanim]") {
    const std::vector<std::byte> good = aanim::write(sample());
    REQUIRE(refusal(good).empty());

    std::vector<std::byte> bytes = good;
    bytes[0] = std::byte{'X'};
    reseal(bytes);
    REQUIRE(refusal(bytes).find("AANM") != std::string::npos);

    bytes = good;
    bytes[4] = std::byte{2};  // major version, low byte
    reseal(bytes);
    REQUIRE(refusal(bytes).find("version") != std::string::npos);

    bytes = good;
    bytes[bytes.size() / 2] ^= std::byte{0x40};
    REQUIRE(refusal(bytes).find("CRC") != std::string::npos);

    bytes = good;
    bytes.resize(bytes.size() - 10);
    REQUIRE_FALSE(refusal(bytes).empty());

    // Field by field, at the offsets the format gives (aanim.hpp).
    const std::size_t keyframes = aanim::kHeaderSize + aanim::name_blob_size_of(good);
    const std::size_t poses = keyframes + 2 * aanim::kKeyframeSize;

    bytes = good;
    const float early = -1.f;
    std::memcpy(bytes.data() + keyframes + aanim::kKeyframeSize, &early, 4);
    reseal(bytes);
    REQUIRE(refusal(bytes).find("time") != std::string::npos);

    bytes = good;
    const float heavy = 1.5f;
    std::memcpy(bytes.data() + poses + aanim::kPoseWeightOffset, &heavy, 4);
    reseal(bytes);
    REQUIRE(refusal(bytes).find("weight") != std::string::npos);

    bytes = good;
    bytes[poses + aanim::kPoseStyleOffset] = std::byte{12};
    reseal(bytes);
    REQUIRE(refusal(bytes).find("easing") != std::string::npos);
}
