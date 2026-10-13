# Network Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A library, `anarchy_network`, that moves bytes between a server and its clients over UDP: packets that each learn whether they arrived, a reliable ordered message channel, an unreliable payload channel, flow control, a handshake that refuses another game's client, and a network conditioner, with `project.json` gaining the `gameId` and `version` the handshake checks.

**Architecture:** A static library under `src/network/` with no dependency on the engine. Bottom up: `Bits` (bit packing and quantization), `Sequence` (wrap-aware sequence math), `Packet` (the 13-byte header and the encode/decode hook), `PacketRing` (sent and received rings, ack bitfield, RTT, loss), `Socket` (an `ISocket` interface with a real `UdpSocket` and an in-process `LinkHub`), `Conditioner` (latency, jitter, loss, duplication, cap), `ReliableChannel`, `FlowControl`, `Connection` (one peer's state: packets in and out), `GameIdentity` (id, version, resources hash), and `Endpoint` (server or client: handshake, peers, pumping). The engine's `Project` reads and writes `gameId` and `version`; it does not link the library.

**Tech Stack:** C++17, Winsock 2 / BSD sockets, zstd's bundled XXH64, CMake, Catch2 v3 sandbox tests, git.

**Spec:** `docs/superpowers/specs/2026-10-12-network-transport-design.md` (and the roadmap `docs/superpowers/specs/2026-10-12-networking-roadmap-design.md`)

## Global Constraints

- Engine distances are "units"; never write "studs". Names are Anarchy's own, never Roblox's.
- `src/network/` depends only on the standard library, the platform socket API, and zstd's `common/xxhash.h`. It never includes anything from `src/engine_core`.
- Namespace `network`. Headers `#pragma once`. Comments in the engine's voice: a sentence that says what and why, above the thing.
- Wire constants, copied from the spec: `kProtocolId = 0xA11C`, `kMaxPacketBytes = 1200`, `kHeaderBytes = 13`, `kMaxPayloadBytes = 1187`, `kFragmentBytes = 1000`, `kWindowSize = 256`, `kRingSize = 256`, `kMaxRefsPerPacket = 64`, `kMaxBlockBytes = 16 MB`, keep-alive every 0.1 s, connect request every 0.1 s, handshake and silence timeout 5 s, `Disconnect` sent 10 times, loss window 2 s, loss timeout `max(0.1, 2 × RTT)`, RTT smoothing weight 0.1, resend after `RTT × 1.25`, flow control halves above 10% loss or 250 ms RTT and recovers 10% a second.
- No threads in the library. Time is a `double now` in seconds that every pump call takes; nothing reads a clock.
- No heap allocation on the steady-state packet path; only reliable blocks (over 1,000 bytes) and the conditioner's queue allocate.
- Build and test (Git Bash, repo root, main checkout with `build/` configured; a worktree needs the setup in memory `worktree-build-setup`):
  - Build: `cmake --build build --config Release --target sandbox > build/net.log 2>&1; grep -E " error |error C|LNK" build/net.log`
  - Run: `./build/Release/sandbox.exe "[network]"`
  - Never run a background test while building: a running `sandbox.exe` locks the link.
- Test names: `N<n> sentence`, tag `[network]` plus an area tag (`[bits]`, `[ring]`, `[socket]`, `[conditioner]`, `[reliable]`, `[flow]`, `[connection]`, `[identity]`, `[endpoint]`, `[bench]`); project tests `PJ<n> sentence`, tags `[project][network]`.
- Commits: one or more per task. Message style `Network: what changed, as a sentence`, ending with:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  ```
- Files are LF. Python is not on PATH; edit with the Edit tool.

## Review Focus

1. **A second client behind the same address** (two players on one NAT, or a client that restarts on the same port): a `ConnectRequest` from an address that already has a peer but with a different client salt. It must neither hijack the existing peer nor crash; the request is ignored while the old peer lives, so the old one times out and the new one gets in on its next request (N60).
2. **Packets from a session that ended** (a client reconnects while old datagrams are still in flight): a packet whose session id does not match the live peer's is dropped and counted, never applied (N61).
3. **Time that jumps** (a debugger pause, a laptop sleeping): an ack arriving with `now` earlier than the packet's send time must not make RTT negative, and a jump forward of a minute times every peer out cleanly rather than looping (N44, N62).
4. **Someone else's datagrams on the port** (a 1,500-byte datagram from another program): the socket drops it without failing, and a well-formed packet with the wrong protocol id is counted and dropped (N22, N23, N63).
5. **The empty and the oversized** (a zero-length reliable message, a message over 16 MB, a payload over the packet's space): the empty message round-trips; the oversized ones are refused with `false` and nothing is queued (N30, N31, N51).

---

### Task 1: Library skeleton and bit serialization

**Files:**
- Create: `src/network/Bits.hpp`, `src/network/Bits.cpp`, `src/network/Sequence.hpp`, `src/network/README.md`, `sandbox/network_bits_tests.cpp`
- Modify: `CMakeLists.txt` (a new target after `profiler`, about line 586; the sandbox source list, about line 1107; `target_link_libraries(sandbox ...)`, about line 1112)

**Interfaces:**
- Produces:
  ```cpp
  namespace network {
  int bits_required(std::uint64_t range);           // bits that hold 0..range; 0 for range 0
  class BitWriter {
  public:
      BitWriter(std::uint8_t* buffer, std::size_t capacity_bytes);
      void bits(std::uint32_t value, int count);     // 1..32 bits, low bits of value
      void boolean(bool value);
      void integer(std::int64_t value, std::int64_t min, std::int64_t max);
      void float_bounded(float value, float min, float max, float resolution);
      void quaternion_smallest_three(const float q[4], int bits_per_component);
      void align();                                  // to the next byte
      void bytes(const std::uint8_t* data, std::size_t count);   // aligns first
      bool overflowed() const;                       // a write did not fit; nothing after it was written
      std::size_t bits_written() const;
      std::size_t bytes_written() const;             // rounded up; the buffer is complete up to here
      std::size_t bits_left() const;
  };
  class BitReader {
  public:
      BitReader(const std::uint8_t* buffer, std::size_t length_bytes);
      std::uint32_t bits(int count);
      bool boolean();
      std::int64_t integer(std::int64_t min, std::int64_t max);
      float float_bounded(float min, float max, float resolution);
      void quaternion_smallest_three(float q[4], int bits_per_component);
      void align();
      void bytes(std::uint8_t* out, std::size_t count);
      bool failed() const;                           // a read went past the end; its result was 0
      std::size_t bits_read() const;
      std::size_t byte_offset() const;               // bits_read / 8, meaningful after align()
      std::size_t bytes_left() const;                // after align(): whole bytes not yet read
  };
  bool sequence_greater_than(std::uint16_t a, std::uint16_t b);
  std::int16_t sequence_diff(std::uint16_t a, std::uint16_t b);  // a - b, wrap-aware
  }
  ```

- [ ] **Step 1: The CMake target**

In `CMakeLists.txt`, after `target_link_libraries(profiler PUBLIC Threads::Threads)` (about line 586), insert:

```cmake
# anarchy_network moves bytes between a server and its clients over UDP
# (docs/superpowers/specs/2026-10-12-network-transport-design.md). It knows
# nothing about instances or physics and never includes engine_core. The
# resources hash in the handshake is zstd's bundled XXH64.
add_library(anarchy_network STATIC
    src/network/Bits.cpp
)
target_include_directories(anarchy_network PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/network")
target_compile_features(anarchy_network PUBLIC cxx_std_17)
target_link_libraries(anarchy_network PRIVATE zstd)
if(WIN32)
    target_link_libraries(anarchy_network PUBLIC ws2_32)
endif()
```

Add `sandbox/network_bits_tests.cpp` to the `add_executable(sandbox ...)` list, after `sandbox/animator_tests.cpp`, and change the sandbox link line to:

```cmake
target_link_libraries(sandbox PRIVATE studio_core anarchy_network Catch2::Catch2WithMain)
```

- [ ] **Step 2: Write the failing tests**

`sandbox/network_bits_tests.cpp`:

```cpp
// Bit packing and quantization for packets
// (docs/superpowers/specs/2026-10-12-network-transport-design.md, section 8).

#include "Bits.hpp"
#include "Sequence.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>

using Catch::Approx;
using network::BitReader;
using network::BitWriter;

TEST_CASE("N1 bits round-trip across byte boundaries and the buffer is complete up to bytes_written", "[network][bits]") {
    std::uint8_t buffer[16] = {};
    BitWriter writer(buffer, sizeof(buffer));
    writer.bits(5, 3);
    writer.bits(0x1FFFF, 17);
    writer.bits(0xFFFFFFFFu, 32);
    writer.bits(2, 2);
    REQUIRE(writer.bits_written() == 54);
    REQUIRE(writer.bytes_written() == 7);
    REQUIRE_FALSE(writer.overflowed());
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE(reader.bits(3) == 5);
    REQUIRE(reader.bits(17) == 0x1FFFF);
    REQUIRE(reader.bits(32) == 0xFFFFFFFFu);
    REQUIRE(reader.bits(2) == 2);
    REQUIRE_FALSE(reader.failed());
}

TEST_CASE("N2 a write that does not fit sets overflowed and writes nothing", "[network][bits]") {
    std::uint8_t buffer[2] = {};
    BitWriter writer(buffer, sizeof(buffer));
    writer.bits(0xABCD, 16);
    writer.bits(1, 1);
    REQUIRE(writer.overflowed());
    REQUIRE(writer.bits_written() == 16);
    REQUIRE(buffer[0] == 0xCD);
    REQUIRE(buffer[1] == 0xAB);
}

TEST_CASE("N3 a read past the end fails and returns 0", "[network][bits]") {
    std::uint8_t buffer[1] = {0xFF};
    BitReader reader(buffer, 1);
    REQUIRE(reader.bits(8) == 0xFF);
    REQUIRE(reader.bits(1) == 0);
    REQUIRE(reader.failed());
}

TEST_CASE("N4 integer uses the fewest bits for its range", "[network][bits]") {
    REQUIRE(network::bits_required(0) == 0);
    REQUIRE(network::bits_required(1) == 1);
    REQUIRE(network::bits_required(255) == 8);
    REQUIRE(network::bits_required(256) == 9);
    std::uint8_t buffer[8] = {};
    BitWriter writer(buffer, sizeof(buffer));
    writer.integer(-3, -10, 10);   // 21 values: 5 bits
    writer.integer(7, 7, 7);       // one value: 0 bits
    writer.integer(99, 0, 50);     // clamped to 50
    REQUIRE(writer.bits_written() == 5 + 0 + 6);
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE(reader.integer(-10, 10) == -3);
    REQUIRE(reader.integer(7, 7) == 7);
    REQUIRE(reader.integer(0, 50) == 50);
}

TEST_CASE("N5 float_bounded quantizes to the resolution and clamps the edges", "[network][bits]") {
    std::uint8_t buffer[16] = {};
    BitWriter writer(buffer, sizeof(buffer));
    writer.float_bounded(1.23456f, -256.0f, 256.0f, 1.0f / 4096.0f);
    writer.float_bounded(-300.0f, -256.0f, 256.0f, 1.0f / 4096.0f);
    writer.float_bounded(256.0f, -256.0f, 256.0f, 1.0f / 4096.0f);
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE(reader.float_bounded(-256.0f, 256.0f, 1.0f / 4096.0f) == Approx(1.23456f).margin(1.0f / 4096.0f));
    REQUIRE(reader.float_bounded(-256.0f, 256.0f, 1.0f / 4096.0f) == Approx(-256.0f).margin(1.0f / 4096.0f));
    REQUIRE(reader.float_bounded(-256.0f, 256.0f, 1.0f / 4096.0f) == Approx(256.0f).margin(1.0f / 4096.0f));
}

TEST_CASE("N6 smallest-three keeps a unit quaternion within its precision, any largest component", "[network][bits]") {
    const float inputs[4][4] = {
        {0.9f, 0.1f, -0.2f, 0.3f},
        {0.1f, -0.95f, 0.2f, 0.1f},
        {-0.3f, 0.2f, 0.85f, -0.3f},
        {0.2f, 0.2f, 0.2f, -0.9f},
    };
    for (const float* raw : inputs) {
        float q[4];
        const float length = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2] + raw[3] * raw[3]);
        for (int i = 0; i < 4; ++i) q[i] = raw[i] / length;
        std::uint8_t buffer[8] = {};
        BitWriter writer(buffer, sizeof(buffer));
        writer.quaternion_smallest_three(q, 15);
        REQUIRE(writer.bits_written() == 2 + 3 * 15);
        float out[4];
        BitReader reader(buffer, writer.bytes_written());
        reader.quaternion_smallest_three(out, 15);
        // The same rotation: q and -q are equal, so compare the absolute dot product.
        const float dot = std::fabs(q[0] * out[0] + q[1] * out[1] + q[2] * out[2] + q[3] * out[3]);
        REQUIRE(dot == Approx(1.0f).margin(1e-4f));
    }
}

TEST_CASE("N7 bytes align first and read back whole", "[network][bits]") {
    std::uint8_t buffer[16] = {};
    BitWriter writer(buffer, sizeof(buffer));
    writer.bits(1, 3);
    const std::uint8_t data[4] = {1, 2, 3, 4};
    writer.bytes(data, 4);
    REQUIRE(writer.bits_written() == 8 + 32);
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE(reader.bits(3) == 1);
    reader.align();
    REQUIRE(reader.byte_offset() == 1);
    REQUIRE(reader.bytes_left() == 4);
    std::uint8_t out[4];
    reader.bytes(out, 4);
    REQUIRE(std::memcmp(out, data, 4) == 0);
    REQUIRE(reader.bytes_left() == 0);
}

TEST_CASE("N8 sequence comparison wraps", "[network][bits]") {
    REQUIRE(network::sequence_greater_than(1, 0));
    REQUIRE(network::sequence_greater_than(0, 65535));
    REQUIRE_FALSE(network::sequence_greater_than(65535, 0));
    REQUIRE(network::sequence_greater_than(32768, 0));
    REQUIRE_FALSE(network::sequence_greater_than(32769, 0));
    REQUIRE(network::sequence_diff(0, 65535) == 1);
    REQUIRE(network::sequence_diff(65535, 0) == -1);
    REQUIRE(network::sequence_diff(10, 3) == 7);
}
```

- [ ] **Step 3: Build to see the failure**

Run: the build command. Expected: errors that `Bits.hpp` cannot be opened.

- [ ] **Step 4: Write `Sequence.hpp`**

```cpp
#pragma once

#include <cstdint>

namespace network {

// 16-bit sequence numbers wrap: 0 comes after 65535. a is later than b when
// their distance the short way round points forward.
inline bool sequence_greater_than(std::uint16_t a, std::uint16_t b) {
    return ((a > b) && (a - b <= 32768)) || ((a < b) && (b - a > 32768));
}

// a - b, taken the short way round.
inline std::int16_t sequence_diff(std::uint16_t a, std::uint16_t b) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b));
}

}  // namespace network
```

- [ ] **Step 5: Write `Bits.hpp`**

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace network {

// The bits that hold every value from 0 to range; 0 when range is 0.
int bits_required(std::uint64_t range);

// Packs values into a byte buffer, low bits first. A write that does not fit
// sets overflowed and is not written; the buffer is complete through
// bytes_written() after every write, so no flush is needed.
class BitWriter {
public:
    BitWriter(std::uint8_t* buffer, std::size_t capacity_bytes);

    void bits(std::uint32_t value, int count);
    void boolean(bool value) { bits(value ? 1u : 0u, 1); }
    void integer(std::int64_t value, std::int64_t min, std::int64_t max);
    void float_bounded(float value, float min, float max, float resolution);
    // Fiedler's smallest three: the index of the largest component in 2 bits,
    // then the other three, each in bits_per_component over ±0.707107.
    void quaternion_smallest_three(const float q[4], int bits_per_component);
    void align();
    void bytes(const std::uint8_t* data, std::size_t count);

    bool overflowed() const { return overflowed_; }
    std::size_t bits_written() const { return bits_written_; }
    std::size_t bytes_written() const { return (bits_written_ + 7) / 8; }
    std::size_t bits_left() const { return capacity_bits_ - bits_written_; }

private:
    std::uint8_t* buffer_;
    std::size_t capacity_bits_;
    std::size_t bits_written_ = 0;
    std::size_t byte_index_ = 0;
    std::uint64_t scratch_ = 0;
    int scratch_bits_ = 0;
    bool overflowed_ = false;
};

// Reads what BitWriter wrote. A read past the end sets failed and returns 0;
// callers check failed() once at the end rather than after every read.
class BitReader {
public:
    BitReader(const std::uint8_t* buffer, std::size_t length_bytes);

    std::uint32_t bits(int count);
    bool boolean() { return bits(1) != 0; }
    std::int64_t integer(std::int64_t min, std::int64_t max);
    float float_bounded(float min, float max, float resolution);
    void quaternion_smallest_three(float q[4], int bits_per_component);
    void align();
    void bytes(std::uint8_t* out, std::size_t count);

    bool failed() const { return failed_; }
    std::size_t bits_read() const { return bits_read_; }
    std::size_t byte_offset() const { return bits_read_ / 8; }
    std::size_t bytes_left() const { return (total_bits_ - bits_read_) / 8; }

private:
    const std::uint8_t* buffer_;
    std::size_t total_bits_;
    std::size_t bits_read_ = 0;
    std::size_t byte_index_ = 0;
    std::uint64_t scratch_ = 0;
    int scratch_bits_ = 0;
    bool failed_ = false;
};

}  // namespace network
```

- [ ] **Step 6: Write `Bits.cpp`**

```cpp
#include "Bits.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace network {

int bits_required(std::uint64_t range) {
    int count = 0;
    while (range > 0) {
        ++count;
        range >>= 1;
    }
    return count;
}

BitWriter::BitWriter(std::uint8_t* buffer, std::size_t capacity_bytes)
    : buffer_(buffer), capacity_bits_(capacity_bytes * 8) {}

void BitWriter::bits(std::uint32_t value, int count) {
    if (count <= 0) return;
    if (bits_written_ + static_cast<std::size_t>(count) > capacity_bits_) {
        overflowed_ = true;
        return;
    }
    if (count < 32) value &= (1u << count) - 1u;
    scratch_ |= static_cast<std::uint64_t>(value) << scratch_bits_;
    scratch_bits_ += count;
    bits_written_ += static_cast<std::size_t>(count);
    while (scratch_bits_ >= 8) {
        buffer_[byte_index_++] = static_cast<std::uint8_t>(scratch_ & 0xFF);
        scratch_ >>= 8;
        scratch_bits_ -= 8;
    }
    // The partial byte is written too, so the buffer is complete at any time.
    if (scratch_bits_ > 0) buffer_[byte_index_] = static_cast<std::uint8_t>(scratch_ & 0xFF);
}

void BitWriter::integer(std::int64_t value, std::int64_t min, std::int64_t max) {
    const std::uint64_t range = static_cast<std::uint64_t>(max - min);
    const int count = bits_required(range);
    if (count == 0) return;
    const std::int64_t clamped = std::min(std::max(value, min), max);
    bits(static_cast<std::uint32_t>(clamped - min), count);
}

void BitWriter::float_bounded(float value, float min, float max, float resolution) {
    const float clamped = std::min(std::max(value, min), max);
    const std::uint32_t steps = static_cast<std::uint32_t>(std::ceil((max - min) / resolution));
    std::uint32_t q = static_cast<std::uint32_t>(std::lround((clamped - min) / resolution));
    if (q > steps) q = steps;
    integer(q, 0, steps);
}

void BitWriter::quaternion_smallest_three(const float q[4], int bits_per_component) {
    int largest = 0;
    for (int i = 1; i < 4; ++i) {
        if (std::fabs(q[i]) > std::fabs(q[largest])) largest = i;
    }
    // q and -q are one rotation; flip so the dropped component is positive.
    const float sign = q[largest] < 0.0f ? -1.0f : 1.0f;
    const float kBound = 0.707107f;
    const std::uint32_t range = (1u << bits_per_component) - 1u;
    bits(static_cast<std::uint32_t>(largest), 2);
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        const float c = std::min(std::max(q[i] * sign, -kBound), kBound);
        const std::uint32_t v = static_cast<std::uint32_t>(std::lround((c + kBound) / (2.0f * kBound) * range));
        bits(std::min(v, range), bits_per_component);
    }
}

void BitWriter::align() {
    const int pad = static_cast<int>((8 - bits_written_ % 8) % 8);
    if (pad > 0) bits(0, pad);
}

void BitWriter::bytes(const std::uint8_t* data, std::size_t count) {
    align();
    if (overflowed_ || bits_written_ + count * 8 > capacity_bits_) {
        overflowed_ = true;
        return;
    }
    std::memcpy(buffer_ + byte_index_, data, count);
    byte_index_ += count;
    bits_written_ += count * 8;
}

BitReader::BitReader(const std::uint8_t* buffer, std::size_t length_bytes)
    : buffer_(buffer), total_bits_(length_bytes * 8) {}

std::uint32_t BitReader::bits(int count) {
    if (count <= 0) return 0;
    if (bits_read_ + static_cast<std::size_t>(count) > total_bits_) {
        failed_ = true;
        return 0;
    }
    while (scratch_bits_ < count) {
        scratch_ |= static_cast<std::uint64_t>(buffer_[byte_index_++]) << scratch_bits_;
        scratch_bits_ += 8;
    }
    const std::uint64_t mask = count == 32 ? 0xFFFFFFFFull : ((1ull << count) - 1ull);
    const std::uint32_t value = static_cast<std::uint32_t>(scratch_ & mask);
    scratch_ >>= count;
    scratch_bits_ -= count;
    bits_read_ += static_cast<std::size_t>(count);
    return value;
}

std::int64_t BitReader::integer(std::int64_t min, std::int64_t max) {
    const int count = bits_required(static_cast<std::uint64_t>(max - min));
    if (count == 0) return min;
    return min + static_cast<std::int64_t>(bits(count));
}

float BitReader::float_bounded(float min, float max, float resolution) {
    const std::uint32_t steps = static_cast<std::uint32_t>(std::ceil((max - min) / resolution));
    const std::int64_t q = integer(0, steps);
    return std::min(max, min + static_cast<float>(q) * resolution);
}

void BitReader::quaternion_smallest_three(float q[4], int bits_per_component) {
    const int largest = static_cast<int>(bits(2));
    const float kBound = 0.707107f;
    const std::uint32_t range = (1u << bits_per_component) - 1u;
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        const float c = static_cast<float>(bits(bits_per_component)) / static_cast<float>(range) * (2.0f * kBound) - kBound;
        q[i] = c;
        sum += c * c;
    }
    q[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
}

void BitReader::align() {
    const int pad = static_cast<int>((8 - bits_read_ % 8) % 8);
    if (pad > 0) bits(pad);
}

void BitReader::bytes(std::uint8_t* out, std::size_t count) {
    align();
    for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<std::uint8_t>(bits(8));
}

}  // namespace network
```

- [ ] **Step 7: Write `src/network/README.md`**

```markdown
# network

`anarchy_network` moves bytes between a server and its clients over UDP and knows nothing about instances or physics. Spec: `docs/superpowers/specs/2026-10-12-network-transport-design.md`; roadmap: `docs/superpowers/specs/2026-10-12-networking-roadmap-design.md`.

Bottom up: `Bits` (bit packing, quantization), `Sequence` (wrap-aware 16-bit sequence math), `Packet` (the header and the encode/decode hook), `PacketRing` (sent and received rings, acks, RTT, loss), `Socket` (`ISocket`, `UdpSocket`, `LinkHub` for tests), `Conditioner` (latency, jitter, loss, duplication, cap), `ReliableChannel`, `FlowControl`, `Connection` (one peer), `GameIdentity` (game id, version, resources hash), `Endpoint` (server or client).

No threads: every pump call takes `double now` in seconds. No heap use on the packet path; reliable blocks and the conditioner's queue are the exceptions.

Tests: `sandbox/network_*_tests.cpp`, tag `[network]`.
```

- [ ] **Step 8: Build and run**

Run: build, then `./build/Release/sandbox.exe "[network]"`. Expected: N1 to N8 pass.

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt src/network sandbox/network_bits_tests.cpp
git commit -m "Network: the anarchy_network library with bit serialization and sequence math

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Packet header and the packet ring

**Files:**
- Create: `src/network/Packet.hpp`, `src/network/Packet.cpp`, `src/network/PacketRing.hpp`, `src/network/PacketRing.cpp`, `sandbox/network_ring_tests.cpp`
- Modify: `CMakeLists.txt` (add the two .cpp to `anarchy_network`; add the test file to sandbox)

**Interfaces:**
- Consumes: `BitWriter`, `BitReader`, `sequence_greater_than`, `sequence_diff` (Task 1).
- Produces:
  ```cpp
  namespace network {
  constexpr std::uint16_t kProtocolId = 0xA11C;
  constexpr std::size_t kMaxPacketBytes = 1200;
  constexpr std::size_t kHeaderBytes = 13;
  constexpr std::size_t kMaxPayloadBytes = kMaxPacketBytes - kHeaderBytes;
  enum class PacketType : std::uint8_t { ConnectRequest, ConnectAccept, ConnectDeny, Disconnect, KeepAlive, Payload };
  struct PacketHeader { std::uint16_t protocol; PacketType type; std::uint16_t session; std::uint16_t sequence; std::uint16_t ack; std::uint32_t ack_bits; };
  void write_header(BitWriter&, const PacketHeader&);
  bool read_header(BitReader&, PacketHeader&);     // false: truncated or wrong protocol
  std::size_t encode_packet(std::uint8_t* bytes, std::size_t length, std::size_t capacity);  // identity today
  std::size_t decode_packet(std::uint8_t* bytes, std::size_t length);                        // identity today; 0 rejects

  constexpr std::size_t kRingSize = 256;
  constexpr int kMaxRefsPerPacket = 64;
  constexpr std::uint16_t kWholeMessage = 0xFFFF;
  struct MessageRef { std::uint16_t id; std::uint16_t fragment; };
  struct SentPacket { bool valid; bool acked; bool has_payload; std::uint16_t sequence; std::uint16_t bytes; std::uint32_t tag; double time; std::uint8_t ref_count; MessageRef refs[kMaxRefsPerPacket]; };
  class PacketRing {
  public:
      SentPacket& record_sent(std::uint16_t sequence, double now);
      enum class Receive { Accepted, Replay, Stale };
      Receive note_received(std::uint16_t sequence);
      bool any_received() const;
      std::uint16_t ack() const;          // 0xFFFF until any_received()
      std::uint32_t ack_bits() const;
      template <class F> void process_acks(std::uint16_t ack, std::uint32_t ack_bits, double now, F&& on_acked);  // on_acked(SentPacket&, double rtt_sample)
      float loss(double now, double timeout) const;   // lost / sent over the last 2 s, for packets older than timeout
  };
  }
  ```

- [ ] **Step 1: Write the failing tests**

`sandbox/network_ring_tests.cpp`:

```cpp
// The packet header, and the rings that turn piggybacked acks into per-packet
// delivery (spec sections 1 and 4).

#include "Bits.hpp"
#include "Packet.hpp"
#include "PacketRing.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using Catch::Approx;
using network::BitReader;
using network::BitWriter;
using network::PacketHeader;
using network::PacketRing;
using network::PacketType;
using network::SentPacket;

TEST_CASE("N10 the header is 13 bytes and round-trips", "[network][ring]") {
    std::uint8_t buffer[network::kMaxPacketBytes];
    BitWriter writer(buffer, sizeof(buffer));
    PacketHeader in{network::kProtocolId, PacketType::Payload, 0xBEEF, 1234, 1230, 0x80000001u};
    network::write_header(writer, in);
    REQUIRE(writer.bytes_written() == network::kHeaderBytes);
    PacketHeader out{};
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE(network::read_header(reader, out));
    REQUIRE(out.type == PacketType::Payload);
    REQUIRE(out.session == 0xBEEF);
    REQUIRE(out.sequence == 1234);
    REQUIRE(out.ack == 1230);
    REQUIRE(out.ack_bits == 0x80000001u);
}

TEST_CASE("N11 a wrong protocol id or a truncated header is refused", "[network][ring]") {
    std::uint8_t buffer[network::kMaxPacketBytes];
    BitWriter writer(buffer, sizeof(buffer));
    network::write_header(writer, PacketHeader{0x1234, PacketType::Payload, 0, 0, 0, 0});
    PacketHeader out{};
    BitReader wrong(buffer, writer.bytes_written());
    REQUIRE_FALSE(network::read_header(wrong, out));
    BitReader truncated(buffer, 5);
    REQUIRE_FALSE(network::read_header(truncated, out));
}

TEST_CASE("N12 encode and decode are the identity today", "[network][ring]") {
    std::uint8_t buffer[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(network::encode_packet(buffer, 8, sizeof(buffer)) == 8);
    REQUIRE(network::decode_packet(buffer, 8) == 8);
    REQUIRE(buffer[0] == 1);
    REQUIRE(buffer[7] == 8);
}

TEST_CASE("N13 received sequences make the ack and the bitfield", "[network][ring]") {
    PacketRing ring;
    REQUIRE_FALSE(ring.any_received());
    REQUIRE(ring.ack() == 0xFFFF);
    REQUIRE(ring.ack_bits() == 0);
    REQUIRE(ring.note_received(100) == PacketRing::Receive::Accepted);
    REQUIRE(ring.note_received(98) == PacketRing::Receive::Accepted);
    REQUIRE(ring.note_received(103) == PacketRing::Receive::Accepted);
    REQUIRE(ring.ack() == 103);
    // bit n: sequence ack - n - 1. 102 no, 101 no, 100 yes (bit 2), 99 no, 98 yes (bit 4).
    REQUIRE(ring.ack_bits() == ((1u << 2) | (1u << 4)));
    REQUIRE(ring.note_received(100) == PacketRing::Receive::Replay);
    REQUIRE(ring.note_received(103) == PacketRing::Receive::Replay);
}

TEST_CASE("N14 a sequence older than the ring is stale, and the ring survives the wrap", "[network][ring]") {
    PacketRing ring;
    ring.note_received(65530);
    ring.note_received(65535);
    REQUIRE(ring.note_received(0) == PacketRing::Receive::Accepted);
    REQUIRE(ring.note_received(3) == PacketRing::Receive::Accepted);
    REQUIRE(ring.ack() == 3);
    // 2 no, 1 no, 0 yes (bit 2), 65535 yes (bit 3), 65534..65531 no, 65530 yes (bit 8).
    REQUIRE(ring.ack_bits() == ((1u << 2) | (1u << 3) | (1u << 8)));
    REQUIRE(ring.note_received(65535) == PacketRing::Receive::Replay);
    ring.note_received(300);
    REQUIRE(ring.note_received(3) == PacketRing::Receive::Stale);
}

TEST_CASE("N15 process_acks fires once per newly acked packet with its RTT sample", "[network][ring]") {
    PacketRing ring;
    for (std::uint16_t s = 0; s < 5; ++s) {
        SentPacket& sent = ring.record_sent(s, 1.0 + s * 0.01);
        sent.tag = 100 + s;
    }
    std::vector<std::uint16_t> acked;
    std::vector<double> samples;
    // The peer saw 4, 2, and 0: ack 4, bits for 2 (bit 1) and 0 (bit 3).
    ring.process_acks(4, (1u << 1) | (1u << 3), 1.2, [&](SentPacket& p, double rtt) {
        acked.push_back(p.sequence);
        samples.push_back(rtt);
    });
    REQUIRE(acked == std::vector<std::uint16_t>{4, 2, 0});
    REQUIRE(samples[0] == Approx(1.2 - 1.04));
    REQUIRE(samples[2] == Approx(0.2));
    // Again: nothing new.
    acked.clear();
    ring.process_acks(4, (1u << 1) | (1u << 3), 1.3, [&](SentPacket& p, double) { acked.push_back(p.sequence); });
    REQUIRE(acked.empty());
    // A later ack covering 1 and 3 fires those.
    ring.process_acks(4, 0xF, 1.4, [&](SentPacket& p, double) { acked.push_back(p.sequence); });
    REQUIRE(acked == std::vector<std::uint16_t>{3, 1});
}

TEST_CASE("N16 an ack of a sequence never sent, or whose slot was reused, does nothing", "[network][ring]") {
    PacketRing ring;
    ring.record_sent(7, 1.0);
    ring.record_sent(7 + 256, 2.0);   // reuses slot 7
    int fired = 0;
    ring.process_acks(7, 0, 2.1, [&](SentPacket&, double) { ++fired; });
    REQUIRE(fired == 0);
    ring.process_acks(0xFFFF, 0, 2.1, [&](SentPacket&, double) { ++fired; });
    REQUIRE(fired == 0);
    ring.process_acks(7 + 256, 0, 2.1, [&](SentPacket&, double) { ++fired; });
    REQUIRE(fired == 1);
}

TEST_CASE("N17 loss counts packets older than the timeout and not acked, over the last two seconds", "[network][ring]") {
    PacketRing ring;
    for (std::uint16_t s = 0; s < 10; ++s) ring.record_sent(s, 10.0 + s * 0.1);   // 10.0 .. 10.9
    // Ack the even ones.
    ring.process_acks(8, (1u << 1) | (1u << 3) | (1u << 5) | (1u << 7), 11.0, [](SentPacket&, double) {});
    // At 11.5 with a 0.3 s timeout, packets sent at or before 11.2 count: 0..9 all. Five lost of ten.
    REQUIRE(ring.loss(11.5, 0.3) == Approx(0.5f));
    // At 11.0, only those sent at or before 10.7 count: 0..7, four lost of eight.
    REQUIRE(ring.loss(11.0, 0.3) == Approx(0.5f));
    // At 13.5, those sent before 11.5 are outside the two-second window: none count.
    REQUIRE(ring.loss(13.5, 0.3) == Approx(0.0f));
}
```

Add `sandbox/network_ring_tests.cpp` to the sandbox list and `src/network/Packet.cpp`, `src/network/PacketRing.cpp` to `anarchy_network`.

- [ ] **Step 2: Build to see the failure**

Run: build. Expected: `Packet.hpp` cannot be opened.

- [ ] **Step 3: Write `Packet.hpp` and `Packet.cpp`**

`Packet.hpp`:

```cpp
#pragma once

#include "Bits.hpp"

#include <cstddef>
#include <cstdint>

namespace network {

// Bumped whenever the wire changes. A packet with another id is dropped
// without a reply.
constexpr std::uint16_t kProtocolId = 0xA11C;
// One datagram, under every common path MTU so IP never fragments it.
constexpr std::size_t kMaxPacketBytes = 1200;
constexpr std::size_t kHeaderBytes = 13;
constexpr std::size_t kMaxPayloadBytes = kMaxPacketBytes - kHeaderBytes;

enum class PacketType : std::uint8_t { ConnectRequest, ConnectAccept, ConnectDeny, Disconnect, KeepAlive, Payload };

// Every packet starts with this. ack and ack_bits are the peer's view of what
// it has received: bit n of ack_bits means sequence (ack - n - 1) arrived.
struct PacketHeader {
    std::uint16_t protocol = kProtocolId;
    PacketType type = PacketType::Payload;
    std::uint16_t session = 0;   // low 16 bits of client salt XOR server salt; 0 while connecting
    std::uint16_t sequence = 0;
    std::uint16_t ack = 0xFFFF;
    std::uint32_t ack_bits = 0;
};

void write_header(BitWriter& writer, const PacketHeader& header);
// False when the bytes are too short or carry another protocol id.
bool read_header(BitReader& reader, PacketHeader& header);

// Sub-project 7's hook: encryption goes here. Today both are the identity.
// encode returns the length to send; decode returns the length to read, or 0
// to drop the packet.
std::size_t encode_packet(std::uint8_t* bytes, std::size_t length, std::size_t capacity);
std::size_t decode_packet(std::uint8_t* bytes, std::size_t length);

}  // namespace network
```

`Packet.cpp`:

```cpp
#include "Packet.hpp"

namespace network {

void write_header(BitWriter& writer, const PacketHeader& header) {
    writer.bits(header.protocol, 16);
    writer.bits(static_cast<std::uint32_t>(header.type), 8);
    writer.bits(header.session, 16);
    writer.bits(header.sequence, 16);
    writer.bits(header.ack, 16);
    writer.bits(header.ack_bits, 32);
}

bool read_header(BitReader& reader, PacketHeader& header) {
    header.protocol = static_cast<std::uint16_t>(reader.bits(16));
    const std::uint32_t type = reader.bits(8);
    header.session = static_cast<std::uint16_t>(reader.bits(16));
    header.sequence = static_cast<std::uint16_t>(reader.bits(16));
    header.ack = static_cast<std::uint16_t>(reader.bits(16));
    header.ack_bits = reader.bits(32);
    if (reader.failed() || header.protocol != kProtocolId || type > static_cast<std::uint32_t>(PacketType::Payload)) {
        return false;
    }
    header.type = static_cast<PacketType>(type);
    return true;
}

std::size_t encode_packet(std::uint8_t*, std::size_t length, std::size_t) { return length; }

std::size_t decode_packet(std::uint8_t*, std::size_t length) { return length; }

}  // namespace network
```

- [ ] **Step 4: Write `PacketRing.hpp` and `PacketRing.cpp`**

`PacketRing.hpp`:

```cpp
#pragma once

#include "Sequence.hpp"

#include <cstddef>
#include <cstdint>

namespace network {

constexpr std::size_t kRingSize = 256;
constexpr int kMaxRefsPerPacket = 64;
// A MessageRef with this fragment is a whole reliable message.
constexpr std::uint16_t kWholeMessage = 0xFFFF;

// What a packet carried of the reliable channel, so an ack can mark it.
struct MessageRef {
    std::uint16_t id = 0;
    std::uint16_t fragment = kWholeMessage;
};

// One packet we sent, until its slot is reused 256 packets later.
struct SentPacket {
    bool valid = false;
    bool acked = false;
    bool has_payload = false;
    std::uint16_t sequence = 0;
    std::uint16_t bytes = 0;
    std::uint32_t tag = 0;
    double time = 0.0;
    std::uint8_t ref_count = 0;
    MessageRef refs[kMaxRefsPerPacket];
};

// Fiedler's two sequence buffers: what we sent, to learn from the peer's acks
// which packets arrived; and what we received, to tell the peer the same.
class PacketRing {
public:
    SentPacket& record_sent(std::uint16_t sequence, double now);

    enum class Receive { Accepted, Replay, Stale };
    Receive note_received(std::uint16_t sequence);
    bool any_received() const { return any_received_; }
    std::uint16_t ack() const { return any_received_ ? latest_ : 0xFFFF; }
    std::uint32_t ack_bits() const;

    // For each sequence the peer's ack covers that we sent and had not seen
    // acked: marks it and calls on_acked(SentPacket&, rtt_sample). A late ack
    // still counts: the peer holds that packet, whatever the loss estimate said.
    template <class F>
    void process_acks(std::uint16_t ack, std::uint32_t ack_bits, double now, F&& on_acked) {
        for (int n = -1; n < 32; ++n) {
            if (n >= 0 && (ack_bits & (1u << n)) == 0) continue;
            const std::uint16_t sequence = static_cast<std::uint16_t>(n < 0 ? ack : ack - n - 1);
            SentPacket& sent = sent_[sequence % kRingSize];
            if (!sent.valid || sent.acked || sent.sequence != sequence) continue;
            sent.acked = true;
            on_acked(sent, now - sent.time);
        }
    }

    // Of the packets sent in the last 2 s that are at least timeout old, the
    // share not acked. 0 when none qualify.
    float loss(double now, double timeout) const;

private:
    SentPacket sent_[kRingSize];
    std::uint16_t received_[kRingSize] = {};
    bool received_valid_[kRingSize] = {};
    std::uint16_t latest_ = 0;
    bool any_received_ = false;
};

}  // namespace network
```

`PacketRing.cpp`:

```cpp
#include "PacketRing.hpp"

namespace network {

SentPacket& PacketRing::record_sent(std::uint16_t sequence, double now) {
    SentPacket& sent = sent_[sequence % kRingSize];
    sent.valid = true;
    sent.acked = false;
    sent.has_payload = false;
    sent.sequence = sequence;
    sent.bytes = 0;
    sent.tag = 0;
    sent.time = now;
    sent.ref_count = 0;
    return sent;
}

PacketRing::Receive PacketRing::note_received(std::uint16_t sequence) {
    if (!any_received_) {
        any_received_ = true;
        latest_ = sequence;
        received_[sequence % kRingSize] = sequence;
        received_valid_[sequence % kRingSize] = true;
        return Receive::Accepted;
    }
    if (sequence_greater_than(sequence, latest_)) {
        // The slots between the old latest and the new are not received.
        const int gap = sequence_diff(sequence, latest_);
        if (gap >= static_cast<int>(kRingSize)) {
            for (std::size_t i = 0; i < kRingSize; ++i) received_valid_[i] = false;
        } else {
            for (int i = 1; i < gap; ++i) {
                received_valid_[static_cast<std::uint16_t>(latest_ + i) % kRingSize] = false;
            }
        }
        latest_ = sequence;
    } else {
        const int age = sequence_diff(latest_, sequence);
        if (age >= static_cast<int>(kRingSize)) return Receive::Stale;
        const std::size_t slot = sequence % kRingSize;
        if (received_valid_[slot] && received_[slot] == sequence) return Receive::Replay;
    }
    received_[sequence % kRingSize] = sequence;
    received_valid_[sequence % kRingSize] = true;
    return Receive::Accepted;
}

std::uint32_t PacketRing::ack_bits() const {
    if (!any_received_) return 0;
    std::uint32_t bits = 0;
    for (int n = 0; n < 32; ++n) {
        const std::uint16_t sequence = static_cast<std::uint16_t>(latest_ - n - 1);
        const std::size_t slot = sequence % kRingSize;
        if (received_valid_[slot] && received_[slot] == sequence) bits |= 1u << n;
    }
    return bits;
}

float PacketRing::loss(double now, double timeout) const {
    int counted = 0;
    int lost = 0;
    for (const SentPacket& sent : sent_) {
        if (!sent.valid || sent.time < now - 2.0 || sent.time > now - timeout) continue;
        ++counted;
        if (!sent.acked) ++lost;
    }
    return counted == 0 ? 0.0f : static_cast<float>(lost) / static_cast<float>(counted);
}

}  // namespace network
```

- [ ] **Step 5: Build and run**

Run: build, then `./build/Release/sandbox.exe "[network]"`. Expected: N10 to N17 pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/network sandbox/network_ring_tests.cpp
git commit -m "Network: the packet header and the sent and received rings

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: Sockets: `Address`, `ISocket`, `LinkHub`, `UdpSocket`

**Files:**
- Create: `src/network/Socket.hpp`, `src/network/Socket.cpp`, `src/network/UdpSocket.cpp`, `sandbox/network_socket_tests.cpp`
- Modify: `CMakeLists.txt` (add the two .cpp to `anarchy_network`; add the test file to sandbox)

**Interfaces:**
- Produces:
  ```cpp
  namespace network {
  struct Address {
      enum class Family : std::uint8_t { None, IPv4, IPv6 };
      Family family = Family::None;
      std::array<std::uint8_t, 16> bytes{};   // 4 used for IPv4
      std::uint16_t port = 0;
      static Address ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port);
      static Address loopback(std::uint16_t port);                       // 127.0.0.1
      static std::optional<Address> parse(std::string_view text);        // "1.2.3.4:5", "[::1]:5"; names are not parsed
      std::string to_string() const;
      bool operator==(const Address&) const; bool operator!=(const Address&) const;
  };
  class ISocket {
  public:
      virtual ~ISocket() = default;
      virtual bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) = 0;
      virtual std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) = 0;  // 0: nothing waiting
      virtual bool failed() const = 0;
  };
  // Datagrams between sockets in one process, keyed by Address. For tests.
  class LinkHub {
  public:
      std::unique_ptr<ISocket> open(const Address& own);
      std::size_t pending() const;  // datagrams not yet received, across the hub
  };
  class UdpSocket : public ISocket {
  public:
      static std::unique_ptr<UdpSocket> open(std::uint16_t port, std::string& error);  // 0: any port
      std::uint16_t port() const;
  };
  }
  ```

- [ ] **Step 1: Write the failing tests**

`sandbox/network_socket_tests.cpp`:

```cpp
// Addresses, the in-process link, and the real UDP socket (spec sections 6 and 7).

#include "Socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

using network::Address;
using network::LinkHub;
using network::UdpSocket;

namespace {

// Polls a socket for up to half a second, since loopback delivery is not instant.
std::size_t wait_receive(network::ISocket& socket, Address& from, std::uint8_t* buffer, std::size_t capacity) {
    for (int i = 0; i < 100; ++i) {
        const std::size_t got = socket.receive(from, buffer, capacity);
        if (got > 0) return got;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}

}  // namespace

TEST_CASE("N20 addresses parse, print, and compare", "[network][socket]") {
    const auto v4 = Address::parse("192.168.1.20:7777");
    REQUIRE(v4.has_value());
    REQUIRE(v4->family == Address::Family::IPv4);
    REQUIRE(v4->port == 7777);
    REQUIRE(v4->to_string() == "192.168.1.20:7777");
    REQUIRE(*v4 == Address::ipv4(192, 168, 1, 20, 7777));
    REQUIRE(*v4 != Address::ipv4(192, 168, 1, 20, 7778));
    const auto v6 = Address::parse("[::1]:9000");
    REQUIRE(v6.has_value());
    REQUIRE(v6->family == Address::Family::IPv6);
    REQUIRE(v6->port == 9000);
    REQUIRE(v6->to_string() == "[::1]:9000");
    REQUIRE_FALSE(Address::parse("example.com:1").has_value());
    REQUIRE_FALSE(Address::parse("1.2.3.4").has_value());
    REQUIRE_FALSE(Address::parse("1.2.3.4:70000").has_value());
    REQUIRE(Address::loopback(5) == Address::ipv4(127, 0, 0, 1, 5));
}

TEST_CASE("N21 the link hub delivers datagrams by address, in order, and nothing to a stranger", "[network][socket]") {
    LinkHub hub;
    auto a = hub.open(Address::loopback(1));
    auto b = hub.open(Address::loopback(2));
    const std::uint8_t one[3] = {1, 2, 3};
    const std::uint8_t two[2] = {9, 9};
    REQUIRE(a->send(Address::loopback(2), one, 3));
    REQUIRE(a->send(Address::loopback(2), two, 2));
    REQUIRE_FALSE(a->send(Address::loopback(3), one, 3));   // no such socket
    REQUIRE(hub.pending() == 2);
    Address from;
    std::uint8_t buffer[16];
    REQUIRE(b->receive(from, buffer, sizeof(buffer)) == 3);
    REQUIRE(from == Address::loopback(1));
    REQUIRE(std::memcmp(buffer, one, 3) == 0);
    REQUIRE(b->receive(from, buffer, sizeof(buffer)) == 2);
    REQUIRE(b->receive(from, buffer, sizeof(buffer)) == 0);
    REQUIRE(a->receive(from, buffer, sizeof(buffer)) == 0);
    REQUIRE_FALSE(a->failed());
}

TEST_CASE("N22 a datagram larger than the receive buffer is dropped by the link, not truncated", "[network][socket]") {
    LinkHub hub;
    auto a = hub.open(Address::loopback(1));
    auto b = hub.open(Address::loopback(2));
    std::uint8_t big[1500] = {};
    REQUIRE(a->send(Address::loopback(2), big, sizeof(big)));
    Address from;
    std::uint8_t buffer[1200];
    REQUIRE(b->receive(from, buffer, sizeof(buffer)) == 0);
    REQUIRE(hub.pending() == 0);
}

TEST_CASE("N23 a real UDP socket sends to itself over loopback and drops an oversized datagram", "[network][socket]") {
    std::string error;
    auto a = UdpSocket::open(0, error);
    if (!a) {
        WARN("no socket: " << error);
        return;
    }
    auto b = UdpSocket::open(0, error);
    REQUIRE(b);
    REQUIRE(a->port() != 0);
    REQUIRE(b->port() != 0);
    const std::uint8_t data[5] = {5, 4, 3, 2, 1};
    REQUIRE(a->send(Address::loopback(b->port()), data, 5));
    Address from;
    std::uint8_t buffer[1200];
    REQUIRE(wait_receive(*b, from, buffer, sizeof(buffer)) == 5);
    REQUIRE(from.port == a->port());
    REQUIRE(std::memcmp(buffer, data, 5) == 0);
    REQUIRE_FALSE(a->failed());
    std::uint8_t big[1500] = {};
    REQUIRE(a->send(Address::loopback(b->port()), big, sizeof(big)));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(b->receive(from, buffer, sizeof(buffer)) == 0);
    REQUIRE_FALSE(b->failed());
    // The socket goes on working after the drop.
    REQUIRE(a->send(Address::loopback(b->port()), data, 5));
    REQUIRE(wait_receive(*b, from, buffer, sizeof(buffer)) == 5);
}

TEST_CASE("N24 opening a port twice fails with a message", "[network][socket]") {
    std::string error;
    auto a = UdpSocket::open(0, error);
    if (!a) return;
    auto b = UdpSocket::open(a->port(), error);
    REQUIRE_FALSE(b);
    REQUIRE_FALSE(error.empty());
}
```

Add `sandbox/network_socket_tests.cpp` to the sandbox list and `src/network/Socket.cpp`, `src/network/UdpSocket.cpp` to `anarchy_network`.

- [ ] **Step 2: Build to see the failure**

Run: build. Expected: `Socket.hpp` cannot be opened.

- [ ] **Step 3: Write `Socket.hpp`**

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace network {

// An IP address and port. Only literal addresses parse; a name is resolved
// by whoever asks the user for one.
struct Address {
    enum class Family : std::uint8_t { None, IPv4, IPv6 };
    Family family = Family::None;
    std::array<std::uint8_t, 16> bytes{};
    std::uint16_t port = 0;

    static Address ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port);
    static Address loopback(std::uint16_t port) { return ipv4(127, 0, 0, 1, port); }
    // "1.2.3.4:5" or "[::1]:5".
    static std::optional<Address> parse(std::string_view text);
    std::string to_string() const;

    bool operator==(const Address& other) const {
        return family == other.family && port == other.port && bytes == other.bytes;
    }
    bool operator!=(const Address& other) const { return !(*this == other); }
};

// What an Endpoint sends and receives through: a real socket, a link in one
// process, or the conditioner wrapping either.
class ISocket {
public:
    virtual ~ISocket() = default;
    virtual bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) = 0;
    // The next datagram waiting, or 0. A datagram over capacity is dropped.
    virtual std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) = 0;
    virtual bool failed() const = 0;
};

// Sockets in one process that deliver to each other by address, for tests.
class LinkHub {
public:
    LinkHub();
    ~LinkHub();
    std::unique_ptr<ISocket> open(const Address& own);
    std::size_t pending() const;

    struct State;

private:
    std::shared_ptr<State> state_;
};

// A non-blocking UDP socket over Winsock or BSD sockets, dual-stack: IPv4
// peers arrive as mapped addresses and are given back as IPv4.
class UdpSocket : public ISocket {
public:
    static std::unique_ptr<UdpSocket> open(std::uint16_t port, std::string& error);
    ~UdpSocket() override;
    std::uint16_t port() const { return port_; }
    bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) override;
    std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) override;
    bool failed() const override { return failed_; }

private:
    UdpSocket() = default;
    std::uintptr_t handle_ = 0;
    std::uint16_t port_ = 0;
    bool failed_ = false;
};

}  // namespace network
```

- [ ] **Step 4: Write `Socket.cpp` (Address and LinkHub)**

```cpp
#include "Socket.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <vector>

namespace network {

Address Address::ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port) {
    Address address;
    address.family = Family::IPv4;
    address.bytes = {a, b, c, d};
    address.port = port;
    return address;
}

namespace {

bool parse_port(std::string_view text, std::uint16_t& port) {
    if (text.empty() || text.size() > 5) return false;
    unsigned value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value > 65535) return false;
    port = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_ipv4(std::string_view text, std::array<std::uint8_t, 16>& bytes) {
    unsigned parts[4];
    int count = 0;
    unsigned value = 0;
    int digits = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '.') {
            if (digits == 0 || count == 4 || value > 255) return false;
            parts[count++] = value;
            value = 0;
            digits = 0;
            continue;
        }
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<unsigned>(text[i] - '0');
        ++digits;
    }
    if (count != 4) return false;
    for (int i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>(parts[i]);
    return true;
}

// Hex groups separated by ':', with one "::" standing for the zero groups.
bool parse_ipv6(std::string_view text, std::array<std::uint8_t, 16>& bytes) {
    std::vector<std::uint16_t> head;
    std::vector<std::uint16_t> tail;
    bool seen_gap = false;
    std::vector<std::uint16_t>* current = &head;
    std::size_t i = 0;
    if (text.size() >= 2 && text[0] == ':' && text[1] == ':') {
        seen_gap = true;
        current = &tail;
        i = 2;
    }
    while (i < text.size()) {
        unsigned value = 0;
        int digits = 0;
        while (i < text.size() && text[i] != ':') {
            const char c = text[i];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = 10u + static_cast<unsigned>(c - 'a');
            else if (c >= 'A' && c <= 'F') digit = 10u + static_cast<unsigned>(c - 'A');
            else return false;
            value = value * 16 + digit;
            if (++digits > 4) return false;
            ++i;
        }
        if (digits == 0) return false;
        current->push_back(static_cast<std::uint16_t>(value));
        if (i == text.size()) break;
        ++i;   // the ':'
        if (i < text.size() && text[i] == ':') {
            if (seen_gap) return false;
            seen_gap = true;
            current = &tail;
            ++i;
        } else if (i == text.size()) {
            return false;
        }
    }
    const std::size_t total = head.size() + tail.size();
    if ((seen_gap && total > 7) || (!seen_gap && total != 8)) return false;
    std::uint16_t groups[8] = {};
    for (std::size_t g = 0; g < head.size(); ++g) groups[g] = head[g];
    for (std::size_t g = 0; g < tail.size(); ++g) groups[8 - tail.size() + g] = tail[g];
    for (int g = 0; g < 8; ++g) {
        bytes[g * 2] = static_cast<std::uint8_t>(groups[g] >> 8);
        bytes[g * 2 + 1] = static_cast<std::uint8_t>(groups[g] & 0xFF);
    }
    return true;
}

}  // namespace

std::optional<Address> Address::parse(std::string_view text) {
    Address address;
    if (!text.empty() && text[0] == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':') return std::nullopt;
        if (!parse_ipv6(text.substr(1, close - 1), address.bytes)) return std::nullopt;
        if (!parse_port(text.substr(close + 2), address.port)) return std::nullopt;
        address.family = Family::IPv6;
        return address;
    }
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    if (!parse_ipv4(text.substr(0, colon), address.bytes)) return std::nullopt;
    if (!parse_port(text.substr(colon + 1), address.port)) return std::nullopt;
    address.family = Family::IPv4;
    return address;
}

std::string Address::to_string() const {
    char buffer[64];
    if (family == Family::IPv4) {
        std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u:%u", bytes[0], bytes[1], bytes[2], bytes[3], port);
        return buffer;
    }
    if (family == Family::IPv6) {
        // The longest run of two or more zero groups becomes "::".
        std::uint16_t groups[8];
        for (int g = 0; g < 8; ++g) groups[g] = static_cast<std::uint16_t>((bytes[g * 2] << 8) | bytes[g * 2 + 1]);
        int best_start = -1;
        int best_length = 0;
        for (int g = 0; g < 8;) {
            if (groups[g] != 0) {
                ++g;
                continue;
            }
            int end = g;
            while (end < 8 && groups[end] == 0) ++end;
            if (end - g >= 2 && end - g > best_length) {
                best_start = g;
                best_length = end - g;
            }
            g = end;
        }
        std::string text = "[";
        for (int g = 0; g < 8;) {
            if (g == best_start) {
                text += "::";
                g += best_length;
                continue;
            }
            if (text.back() != '[' && text.back() != ':') text += ':';
            std::snprintf(buffer, sizeof(buffer), "%x", groups[g]);
            text += buffer;
            ++g;
        }
        std::snprintf(buffer, sizeof(buffer), "]:%u", port);
        return text + buffer;
    }
    return "none";
}

// The hub's datagrams: one queue per opened address.
struct LinkHub::State {
    struct Datagram {
        Address from;
        std::vector<std::uint8_t> bytes;
    };
    struct Slot {
        Address address;
        std::deque<Datagram> queue;
    };
    std::vector<Slot> slots;
    // Looked up fresh on every call: open() may grow the vector.
    Slot* find(const Address& address) {
        for (Slot& slot : slots) {
            if (slot.address == address) return &slot;
        }
        return nullptr;
    }
};

namespace {

class LinkSocket : public ISocket {
public:
    LinkSocket(std::shared_ptr<LinkHub::State> state, Address own) : state_(std::move(state)), own_(own) {}
    bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) override {
        LinkHub::State::Slot* slot = state_->find(to);
        if (slot == nullptr) return false;
        slot->queue.push_back({own_, std::vector<std::uint8_t>(bytes, bytes + length)});
        return true;
    }
    std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) override {
        LinkHub::State::Slot* slot = state_->find(own_);
        while (slot != nullptr && !slot->queue.empty()) {
            LinkHub::State::Datagram datagram = std::move(slot->queue.front());
            slot->queue.pop_front();
            if (datagram.bytes.size() > capacity) continue;   // dropped, as a socket drops an oversized datagram
            from = datagram.from;
            std::copy(datagram.bytes.begin(), datagram.bytes.end(), buffer);
            return datagram.bytes.size();
        }
        return 0;
    }
    bool failed() const override { return false; }

private:
    std::shared_ptr<LinkHub::State> state_;
    Address own_;
};

}  // namespace

LinkHub::LinkHub() : state_(std::make_shared<State>()) {}
LinkHub::~LinkHub() = default;

std::unique_ptr<ISocket> LinkHub::open(const Address& own) {
    if (state_->find(own) == nullptr) state_->slots.push_back({own, {}});
    return std::make_unique<LinkSocket>(state_, own);
}

std::size_t LinkHub::pending() const {
    std::size_t count = 0;
    for (const State::Slot& slot : state_->slots) count += slot.queue.size();
    return count;
}

}  // namespace network
```

- [ ] **Step 5: Write `UdpSocket.cpp`**

```cpp
#include "Socket.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace network {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalid = INVALID_SOCKET;
// Winsock is started once for the process and never stopped: WSACleanup
// counts calls, and the last socket closing does not need it.
bool start_sockets() {
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}
int last_error() { return WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
bool oversized(int error) { return error == WSAEMSGSIZE; }
bool peer_gone(int error) { return error == WSAECONNRESET; }
void close_socket(socket_t s) { closesocket(s); }
constexpr int kReceiveFlags = 0;
#else
using socket_t = int;
constexpr socket_t kInvalid = -1;
bool start_sockets() { return true; }
int last_error() { return errno; }
bool would_block(int error) { return error == EWOULDBLOCK || error == EAGAIN; }
bool oversized(int) { return false; }
bool peer_gone(int error) { return error == ECONNREFUSED; }
void close_socket(socket_t s) { ::close(s); }
// MSG_TRUNC reports the datagram's real length, so an oversized one is dropped
// here as Windows and the link drop it.
constexpr int kReceiveFlags = MSG_TRUNC;
#endif

std::string error_text(int error) {
#ifdef _WIN32
    char* text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(error), 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string result = text != nullptr ? text : "socket error";
    if (text != nullptr) LocalFree(text);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) result.pop_back();
    return result;
#else
    return std::strerror(error);
#endif
}

void to_sockaddr(const Address& address, sockaddr_in6& out) {
    std::memset(&out, 0, sizeof(out));
    out.sin6_family = AF_INET6;
    out.sin6_port = htons(address.port);
    if (address.family == Address::Family::IPv6) {
        std::memcpy(&out.sin6_addr, address.bytes.data(), 16);
    } else {
        const std::uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF,
                                         address.bytes[0], address.bytes[1], address.bytes[2], address.bytes[3]};
        std::memcpy(&out.sin6_addr, mapped, 16);
    }
}

Address from_sockaddr(const sockaddr_in6& in) {
    const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(&in.sin6_addr);
    bool mapped = raw[10] == 0xFF && raw[11] == 0xFF;
    for (int i = 0; i < 10; ++i) mapped = mapped && raw[i] == 0;
    if (mapped) return Address::ipv4(raw[12], raw[13], raw[14], raw[15], ntohs(in.sin6_port));
    Address address;
    address.family = Address::Family::IPv6;
    std::memcpy(address.bytes.data(), raw, 16);
    address.port = ntohs(in.sin6_port);
    return address;
}

}  // namespace

std::unique_ptr<UdpSocket> UdpSocket::open(std::uint16_t port, std::string& error) {
    if (!start_sockets()) {
        error = "Winsock did not start";
        return nullptr;
    }
    const socket_t handle = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kInvalid) {
        error = error_text(last_error());
        return nullptr;
    }
    int off = 0;
    setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
    sockaddr_in6 bind_address;
    std::memset(&bind_address, 0, sizeof(bind_address));
    bind_address.sin6_family = AF_INET6;
    bind_address.sin6_addr = in6addr_any;
    bind_address.sin6_port = htons(port);
    if (::bind(handle, reinterpret_cast<const sockaddr*>(&bind_address), sizeof(bind_address)) != 0) {
        error = "cannot bind port " + std::to_string(port) + ": " + error_text(last_error());
        close_socket(handle);
        return nullptr;
    }
#ifdef _WIN32
    u_long non_blocking = 1;
    ioctlsocket(handle, FIONBIO, &non_blocking);
    // Without this, an ICMP port-unreachable from a peer that went away makes
    // the next recvfrom fail with WSAECONNRESET.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(handle, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#else
    fcntl(handle, F_SETFL, fcntl(handle, F_GETFL, 0) | O_NONBLOCK);
#endif
    sockaddr_in6 bound;
    socklen_t length = sizeof(bound);
    getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length);
    std::unique_ptr<UdpSocket> socket(new UdpSocket());
    socket->handle_ = static_cast<std::uintptr_t>(handle);
    socket->port_ = ntohs(bound.sin6_port);
    return socket;
}

UdpSocket::~UdpSocket() {
    if (handle_ != 0) close_socket(static_cast<socket_t>(handle_));
}

bool UdpSocket::send(const Address& to, const std::uint8_t* bytes, std::size_t length) {
    if (failed_ || to.family == Address::Family::None) return false;
    sockaddr_in6 address;
    to_sockaddr(to, address);
    const auto sent = ::sendto(static_cast<socket_t>(handle_), reinterpret_cast<const char*>(bytes), static_cast<int>(length), 0,
                               reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (sent < 0) {
        const int error = last_error();
        if (would_block(error) || peer_gone(error)) return false;
        failed_ = true;
        return false;
    }
    return true;
}

std::size_t UdpSocket::receive(Address& from, std::uint8_t* buffer, std::size_t capacity) {
    if (failed_) return 0;
    for (;;) {
        sockaddr_in6 address;
        socklen_t length = sizeof(address);
        const auto got = ::recvfrom(static_cast<socket_t>(handle_), reinterpret_cast<char*>(buffer), static_cast<int>(capacity),
                                    kReceiveFlags, reinterpret_cast<sockaddr*>(&address), &length);
        if (got < 0) {
            const int error = last_error();
            if (would_block(error)) return 0;
            if (oversized(error) || peer_gone(error)) continue;   // that datagram is gone; look for the next
            failed_ = true;
            return 0;
        }
        if (static_cast<std::size_t>(got) > capacity) continue;   // MSG_TRUNC: oversized, dropped
        from = from_sockaddr(address);
        return static_cast<std::size_t>(got);
    }
}

}  // namespace network
```

On macOS `MSG_TRUNC` is not an input flag and an oversized datagram is truncated silently; the truncated packet then fails the reliable section's validation or the payload is short, and it is dropped one layer up. That is acceptable: the spec's rule is that it never crashes or corrupts.

- [ ] **Step 6: Build and run**

Run: build, then `./build/Release/sandbox.exe "[network]"`. Expected: N20 to N24 pass. If N23 prints a WARN and returns, the machine has no socket; note it in the commit and go on.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/network sandbox/network_socket_tests.cpp
git commit -m "Network: addresses, the in-process link hub, and the UDP socket

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: The conditioner

**Files:**
- Create: `src/network/Conditioner.hpp`, `src/network/Conditioner.cpp`, `sandbox/network_conditioner_tests.cpp`
- Modify: `CMakeLists.txt` (add the .cpp; add the test file)

**Interfaces:**
- Consumes: `ISocket`, `Address`, `LinkHub` (Task 3).
- Produces:
  ```cpp
  namespace network {
  struct ConditionerSettings {
      double latency_seconds = 0.0;
      double jitter_seconds = 0.0;               // uniform in ±jitter
      float loss = 0.0f;                         // 0..1
      float duplicate = 0.0f;                    // 0..1
      double bandwidth_bytes_per_second = 0.0;   // 0: no cap
      bool active() const;
  };
  class Conditioner : public ISocket {
  public:
      explicit Conditioner(std::unique_ptr<ISocket> inner, std::uint32_t seed = 1);
      ConditionerSettings settings;
      void advance(double now);     // releases what is due into the inner socket; call before send/receive each pump
      std::size_t held() const;     // datagrams waiting to be released
  };
  }
  ```

- [ ] **Step 1: Write the failing tests**

`sandbox/network_conditioner_tests.cpp`:

```cpp
// Latency, jitter, loss, duplication, and a bandwidth cap on the way out
// (spec section 7).

#include "Conditioner.hpp"
#include "Socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

using network::Address;
using network::Conditioner;
using network::LinkHub;

namespace {

struct Pair {
    LinkHub hub;
    Conditioner a{hub.open(Address::loopback(1)), 7};
    std::unique_ptr<network::ISocket> b = hub.open(Address::loopback(2));
    std::uint8_t buffer[64];
    Address from;
    int drain() {
        int count = 0;
        while (b->receive(from, buffer, sizeof(buffer)) > 0) ++count;
        return count;
    }
};

const std::uint8_t kByte[1] = {42};

}  // namespace

TEST_CASE("N25 with every setting zero the conditioner passes datagrams straight through", "[network][conditioner]") {
    Pair pair;
    REQUIRE_FALSE(pair.a.settings.active());
    pair.a.advance(0.0);
    REQUIRE(pair.a.send(Address::loopback(2), kByte, 1));
    REQUIRE(pair.a.held() == 0);
    REQUIRE(pair.drain() == 1);
}

TEST_CASE("N26 latency holds a datagram until it is due", "[network][conditioner]") {
    Pair pair;
    pair.a.settings.latency_seconds = 0.150;
    pair.a.advance(0.0);
    REQUIRE(pair.a.send(Address::loopback(2), kByte, 1));
    REQUIRE(pair.a.held() == 1);
    REQUIRE(pair.drain() == 0);
    pair.a.advance(0.100);
    REQUIRE(pair.drain() == 0);
    pair.a.advance(0.150);
    REQUIRE(pair.a.held() == 0);
    REQUIRE(pair.drain() == 1);
}

TEST_CASE("N27 jitter releases datagrams out of order within the window", "[network][conditioner]") {
    Pair pair;
    pair.a.settings.latency_seconds = 0.100;
    pair.a.settings.jitter_seconds = 0.050;
    pair.a.advance(0.0);
    for (std::uint8_t i = 0; i < 40; ++i) {
        const std::uint8_t byte[1] = {i};
        pair.a.send(Address::loopback(2), byte, 1);
    }
    REQUIRE(pair.drain() == 0);
    pair.a.advance(0.049);
    REQUIRE(pair.drain() == 0);
    pair.a.advance(0.151);
    bool reordered = false;
    std::uint8_t previous = 0;
    int count = 0;
    while (pair.b->receive(pair.from, pair.buffer, sizeof(pair.buffer)) > 0) {
        if (count > 0 && pair.buffer[0] < previous) reordered = true;
        previous = pair.buffer[0];
        ++count;
    }
    REQUIRE(count == 40);
    REQUIRE(reordered);
}

TEST_CASE("N28 loss and duplication hit their rates over many datagrams", "[network][conditioner]") {
    Pair pair;
    pair.a.settings.loss = 0.25f;
    pair.a.advance(0.0);
    for (int i = 0; i < 4000; ++i) pair.a.send(Address::loopback(2), kByte, 1);
    const int delivered = pair.drain();
    REQUIRE(delivered > 2800);
    REQUIRE(delivered < 3200);
    pair.a.settings.loss = 0.0f;
    pair.a.settings.duplicate = 0.25f;
    for (int i = 0; i < 4000; ++i) pair.a.send(Address::loopback(2), kByte, 1);
    const int doubled = pair.drain();
    REQUIRE(doubled > 4800);
    REQUIRE(doubled < 5200);
}

TEST_CASE("N29 a bandwidth cap drops what exceeds it each second", "[network][conditioner]") {
    Pair pair;
    pair.a.settings.bandwidth_bytes_per_second = 100.0;
    std::uint8_t ten[10] = {};
    pair.a.advance(0.0);
    int accepted = 0;
    for (int i = 0; i < 20; ++i) accepted += pair.a.send(Address::loopback(2), ten, 10) ? 1 : 0;
    REQUIRE(accepted == 10);
    REQUIRE(pair.drain() == 10);
    pair.a.advance(1.0);
    REQUIRE(pair.a.send(Address::loopback(2), ten, 10));
}
```

- [ ] **Step 2: Build to see the failure**

Run: build. Expected: `Conditioner.hpp` cannot be opened.

- [ ] **Step 3: Write `Conditioner.hpp`**

```cpp
#pragma once

#include "Socket.hpp"

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace network {

// How bad the network is. All zero is a perfect one.
struct ConditionerSettings {
    double latency_seconds = 0.0;
    double jitter_seconds = 0.0;
    float loss = 0.0f;
    float duplicate = 0.0f;
    double bandwidth_bytes_per_second = 0.0;
    bool active() const {
        return latency_seconds > 0.0 || jitter_seconds > 0.0 || loss > 0.0f || duplicate > 0.0f ||
               bandwidth_bytes_per_second > 0.0;
    }
};

// Wraps a socket and delays, jitters, drops, duplicates, and caps what is
// sent through it. With every setting zero it passes datagrams straight
// through and holds nothing. Only sending is conditioned; wrap both
// endpoints to condition both directions.
class Conditioner : public ISocket {
public:
    explicit Conditioner(std::unique_ptr<ISocket> inner, std::uint32_t seed = 1);

    ConditionerSettings settings;

    // Hands every held datagram that is due to the inner socket.
    void advance(double now);
    bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) override;
    std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) override;
    bool failed() const override { return inner_->failed(); }
    std::size_t held() const { return held_.size(); }

private:
    struct Held {
        double due;
        Address to;
        std::vector<std::uint8_t> bytes;
    };
    void hold(const Address& to, const std::uint8_t* bytes, std::size_t length);

    std::unique_ptr<ISocket> inner_;
    std::vector<Held> held_;
    std::mt19937 random_;
    double now_ = 0.0;
    double second_start_ = 0.0;
    double bytes_this_second_ = 0.0;
};

}  // namespace network
```

- [ ] **Step 4: Write `Conditioner.cpp`**

```cpp
#include "Conditioner.hpp"

#include <algorithm>

namespace network {

Conditioner::Conditioner(std::unique_ptr<ISocket> inner, std::uint32_t seed)
    : inner_(std::move(inner)), random_(seed) {}

void Conditioner::advance(double now) {
    now_ = now;
    if (now_ - second_start_ >= 1.0) {
        second_start_ = now_;
        bytes_this_second_ = 0.0;
    }
    if (held_.empty()) return;
    // Due ones leave in due order, so jitter reorders exactly what it should.
    std::stable_sort(held_.begin(), held_.end(), [](const Held& a, const Held& b) { return a.due < b.due; });
    std::size_t released = 0;
    while (released < held_.size() && held_[released].due <= now_) {
        inner_->send(held_[released].to, held_[released].bytes.data(), held_[released].bytes.size());
        ++released;
    }
    held_.erase(held_.begin(), held_.begin() + static_cast<std::ptrdiff_t>(released));
}

void Conditioner::hold(const Address& to, const std::uint8_t* bytes, std::size_t length) {
    double delay = settings.latency_seconds;
    if (settings.jitter_seconds > 0.0) {
        std::uniform_real_distribution<double> jitter(-settings.jitter_seconds, settings.jitter_seconds);
        delay += jitter(random_);
    }
    held_.push_back({now_ + std::max(0.0, delay), to, std::vector<std::uint8_t>(bytes, bytes + length)});
}

bool Conditioner::send(const Address& to, const std::uint8_t* bytes, std::size_t length) {
    if (!settings.active()) return inner_->send(to, bytes, length);
    if (settings.bandwidth_bytes_per_second > 0.0) {
        if (bytes_this_second_ + static_cast<double>(length) > settings.bandwidth_bytes_per_second) return false;
        bytes_this_second_ += static_cast<double>(length);
    }
    std::uniform_real_distribution<float> chance(0.0f, 1.0f);
    // A lost datagram was sent, as far as the caller can tell.
    if (settings.loss > 0.0f && chance(random_) < settings.loss) return true;
    const int copies = (settings.duplicate > 0.0f && chance(random_) < settings.duplicate) ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
        if (settings.latency_seconds > 0.0 || settings.jitter_seconds > 0.0) {
            hold(to, bytes, length);
        } else {
            inner_->send(to, bytes, length);
        }
    }
    return true;
}

std::size_t Conditioner::receive(Address& from, std::uint8_t* buffer, std::size_t capacity) {
    return inner_->receive(from, buffer, capacity);
}

}  // namespace network
```

- [ ] **Step 5: Build and run**

Run: build, then `./build/Release/sandbox.exe "[network]"`. Expected: N25 to N29 pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/network sandbox/network_conditioner_tests.cpp
git commit -m "Network: the conditioner delays, jitters, drops, duplicates, and caps

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: The reliable channel

**Files:**
- Create: `src/network/ReliableChannel.hpp`, `src/network/ReliableChannel.cpp`, `sandbox/network_reliable_tests.cpp`
- Modify: `CMakeLists.txt` (add the .cpp; add the test file)

**Interfaces:**
- Consumes: `BitWriter`, `BitReader`, `sequence_diff` (Task 1); `SentPacket`, `MessageRef`, `kWholeMessage`, `kMaxRefsPerPacket`, `PacketRing` (Task 2, the tests use the ring).
- Produces:
  ```cpp
  namespace network {
  constexpr std::size_t kFragmentBytes = 1000;
  constexpr std::size_t kWindowSize = 256;
  constexpr std::size_t kMaxBlockBytes = 16u << 20;
  class ReliableChannel {
  public:
      bool send(const std::uint8_t* bytes, std::size_t length);     // false: window full or over kMaxBlockBytes
      bool has_due(double now, double rtt) const;
      std::size_t bytes_due(double now, double rtt) const;          // what write() would like to send, with per-entry overhead
      // The reliable section, always terminated. reserve_bytes are kept free after it for the payload.
      void write(BitWriter& writer, double now, double rtt, SentPacket& packet, std::size_t reserve_bytes = 0);
      bool validate(BitReader reader) const;                        // by value: the caller's reader is not moved
      bool read(BitReader& reader);                                 // validate, then apply; false: malformed, nothing applied
      template <class F> void deliver(F&& on_message);              // on_message(const std::uint8_t*, std::size_t), in order
      void acked(const MessageRef& ref);
      std::size_t unacked() const;                                  // messages in the send window
  };
  }
  ```

The wire format of the reliable section, right after the header: for each entry a `1` bit, then `id` (16), `fragment_count` (16; 0 for a whole message), `fragment_index` (16, only when `fragment_count > 0`), `length` (16; the bytes in this entry, at most 1,000), align, the bytes. A `0` bit ends the section. The unreliable payload, if any, follows after an align.

- [ ] **Step 1: Write the failing tests**

`sandbox/network_reliable_tests.cpp`:

```cpp
// Ordered, exactly-once messages and blocks, sent until acked (spec section 2).

#include "Bits.hpp"
#include "Packet.hpp"
#include "PacketRing.hpp"
#include "ReliableChannel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

using network::BitReader;
using network::BitWriter;
using network::MessageRef;
using network::PacketRing;
using network::ReliableChannel;
using network::SentPacket;

namespace {

struct Packet {
    std::uint16_t sequence;
    std::vector<std::uint8_t> bytes;
};

// One side of a link: a channel and the ring that tracks its packets.
struct Side {
    ReliableChannel channel;
    PacketRing ring;
    std::uint16_t next_sequence = 0;
    std::vector<std::vector<std::uint8_t>> received;

    Packet build(double now, double rtt) {
        Packet packet;
        packet.sequence = next_sequence++;
        packet.bytes.resize(network::kMaxPayloadBytes);
        BitWriter writer(packet.bytes.data(), packet.bytes.size());
        SentPacket& sent = ring.record_sent(packet.sequence, now);
        channel.write(writer, now, rtt, sent);
        REQUIRE_FALSE(writer.overflowed());
        packet.bytes.resize(writer.bytes_written());
        return packet;
    }
    void take(const Packet& packet) {
        REQUIRE(ring.note_received(packet.sequence) == PacketRing::Receive::Accepted);
        BitReader reader(packet.bytes.data(), packet.bytes.size());
        REQUIRE(channel.read(reader));
        channel.deliver([&](const std::uint8_t* data, std::size_t length) { received.emplace_back(data, data + length); });
    }
    // Learns from the other side's ack state which of our packets arrived.
    void learn_acks(const Side& other, double now) {
        ring.process_acks(other.ring.ack(), other.ring.ack_bits(), now, [&](SentPacket& sent, double) {
            for (int i = 0; i < sent.ref_count; ++i) channel.acked(sent.refs[i]);
        });
    }
};

std::vector<std::uint8_t> message(std::uint32_t seed, std::size_t length) {
    std::vector<std::uint8_t> bytes(length);
    std::mt19937 random(seed);
    for (std::uint8_t& b : bytes) b = static_cast<std::uint8_t>(random());
    return bytes;
}

}  // namespace

TEST_CASE("N30 an empty message and a full one round-trip in order; one over 16 MB is refused", "[network][reliable]") {
    Side a, b;
    const std::vector<std::uint8_t> full = message(1, 1000);
    REQUIRE(a.channel.send(nullptr, 0));
    REQUIRE(a.channel.send(full.data(), full.size()));
    REQUIRE_FALSE(a.channel.send(full.data(), (16u << 20) + 1));
    REQUIRE(a.channel.unacked() == 2);
    b.take(a.build(0.0, 0.1));
    REQUIRE(b.received.size() == 2);
    REQUIRE(b.received[0].empty());
    REQUIRE(b.received[1] == full);
    a.learn_acks(b, 0.05);
    REQUIRE(a.channel.unacked() == 0);
}

TEST_CASE("N31 the send window holds 256 messages and refuses the 257th until one is acked", "[network][reliable]") {
    Side a, b;
    const std::uint8_t byte = 1;
    for (int i = 0; i < 256; ++i) REQUIRE(a.channel.send(&byte, 1));
    REQUIRE_FALSE(a.channel.send(&byte, 1));
    b.take(a.build(0.0, 0.1));   // the first 64 fit in one packet
    a.learn_acks(b, 0.01);
    REQUIRE(a.channel.send(&byte, 1));
}

TEST_CASE("N32 messages arrive in order, exactly once, under 30% loss and reordering", "[network][reliable]") {
    Side a, b;
    std::mt19937 random(3);
    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<Packet> in_flight;
    const double rtt = 0.05;
    double now = 0.0;
    int next_message = 0;
    for (int tick = 0; tick < 2000 && (b.received.size() < 500); ++tick) {
        now = tick / 60.0;
        if (next_message < 500 && tick % 2 == 0) {
            sent.push_back(message(static_cast<std::uint32_t>(next_message), 1 + next_message % 200));
            if (a.channel.send(sent.back().data(), sent.back().size())) ++next_message;
            else sent.pop_back();
        }
        Packet packet = a.build(now, rtt);
        if (std::uniform_real_distribution<float>(0.0f, 1.0f)(random) >= 0.3f) in_flight.push_back(std::move(packet));
        // Deliver in a shuffled order, a few per tick.
        std::shuffle(in_flight.begin(), in_flight.end(), random);
        for (int d = 0; d < 2 && !in_flight.empty(); ++d) {
            b.take(in_flight.back());
            in_flight.pop_back();
        }
        a.learn_acks(b, now);
    }
    REQUIRE(b.received.size() == 500);
    for (int i = 0; i < 500; ++i) REQUIRE(b.received[i] == sent[i]);
}

TEST_CASE("N33 a 100 KB block arrives whole under 10% loss", "[network][reliable]") {
    Side a, b;
    std::mt19937 random(5);
    const std::vector<std::uint8_t> block = message(9, 100 * 1024);
    REQUIRE(a.channel.send(block.data(), block.size()));
    for (int tick = 0; tick < 3000 && b.received.empty(); ++tick) {
        const double now = tick / 60.0;
        Packet packet = a.build(now, 0.05);
        if (std::uniform_real_distribution<float>(0.0f, 1.0f)(random) >= 0.1f) b.take(packet);
        a.learn_acks(b, now);
    }
    REQUIRE(b.received.size() == 1);
    REQUIRE(b.received[0] == block);
    REQUIRE(a.channel.unacked() == 0);
}

TEST_CASE("N34 a message is resent only after RTT times 1.25", "[network][reliable]") {
    Side a;
    const std::uint8_t byte = 7;
    REQUIRE(a.channel.send(&byte, 1));
    REQUIRE(a.channel.has_due(0.0, 0.1));
    REQUIRE(a.build(0.0, 0.1).bytes.size() > 1);
    REQUIRE_FALSE(a.channel.has_due(0.05, 0.1));
    REQUIRE(a.build(0.05, 0.1).bytes.size() == 1);   // just the terminating bit
    REQUIRE(a.channel.has_due(0.125, 0.1));
    REQUIRE(a.build(0.125, 0.1).bytes.size() > 1);
}

TEST_CASE("N35 a packet carries at most 64 entries and never overflows its space", "[network][reliable]") {
    Side a, b;
    const std::vector<std::uint8_t> ten = message(2, 10);
    for (int i = 0; i < 200; ++i) REQUIRE(a.channel.send(ten.data(), ten.size()));
    const Packet first = a.build(0.0, 0.1);
    REQUIRE(first.bytes.size() <= network::kMaxPayloadBytes);
    b.take(first);
    REQUIRE(b.received.size() == 64);
    // Big messages: as many 1000-byte entries as fit, which is one.
    Side c, d;
    const std::vector<std::uint8_t> big = message(4, 1000);
    for (int i = 0; i < 3; ++i) REQUIRE(c.channel.send(big.data(), big.size()));
    d.take(c.build(0.0, 0.1));
    REQUIRE(d.received.size() == 1);
}

TEST_CASE("N36 a malformed section is refused and bytes_due counts what is waiting", "[network][reliable]") {
    ReliableChannel channel;
    REQUIRE_FALSE(channel.has_due(0.0, 0.1));
    REQUIRE(channel.bytes_due(0.0, 0.1) == 0);
    const std::uint8_t byte = 1;
    REQUIRE(channel.send(&byte, 1));
    REQUIRE(channel.bytes_due(0.0, 0.1) >= 1);
    // An entry that claims 1,001 bytes.
    std::uint8_t buffer[16];
    BitWriter writer(buffer, sizeof(buffer));
    writer.boolean(true);
    writer.bits(0, 16);
    writer.bits(0, 16);
    writer.bits(1001, 16);
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE_FALSE(channel.read(reader));
    // An entry cut off before its bytes.
    BitWriter cut(buffer, sizeof(buffer));
    cut.boolean(true);
    cut.bits(0, 16);
    cut.bits(0, 16);
    cut.bits(100, 16);
    BitReader short_reader(buffer, cut.bytes_written());
    REQUIRE_FALSE(channel.read(short_reader));
}
```

Add the test file to the sandbox list and `src/network/ReliableChannel.cpp` to `anarchy_network`.

- [ ] **Step 2: Build to see the failure**

Run: build. Expected: `ReliableChannel.hpp` cannot be opened.

- [ ] **Step 3: Write `ReliableChannel.hpp`**

```cpp
#pragma once

#include "Bits.hpp"
#include "PacketRing.hpp"
#include "Sequence.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace network {

// A message over this many bytes is a block, sent as fragments of this size.
constexpr std::size_t kFragmentBytes = 1000;
// Messages in flight, and messages the receiver holds for ordering.
constexpr std::size_t kWindowSize = 256;
constexpr std::size_t kMaxBlockBytes = 16u << 20;

// Ordered messages delivered exactly once. Each is sent in every packet
// that has room, until acked, no sooner than RTT × 1.25 after its last
// send; a block's fragments are acked one by one. Small messages live
// inline; only blocks use the heap.
class ReliableChannel {
public:
    bool send(const std::uint8_t* bytes, std::size_t length);
    bool has_due(double now, double rtt) const;
    std::size_t bytes_due(double now, double rtt) const;
    // Appends the section and records what it carried in packet.refs. The
    // last reserve_bytes of the writer are left for the payload.
    void write(BitWriter& writer, double now, double rtt, SentPacket& packet, std::size_t reserve_bytes = 0);
    // Walks a section without applying it. Connection checks before it
    // records the packet as received, so a bad packet is as if it never came.
    bool validate(BitReader reader) const;
    bool read(BitReader& reader);
    template <class F>
    void deliver(F&& on_message) {
        for (;;) {
            Incoming& in = in_[next_deliver_ % kWindowSize];
            if (!in.valid || !in.complete || in.id != next_deliver_) return;
            on_message(in.data(), in.length);
            in.valid = false;
            in.block = std::vector<std::uint8_t>();
            in.fragment_present = std::vector<std::uint8_t>();
            ++next_deliver_;
        }
    }
    void acked(const MessageRef& ref);
    std::size_t unacked() const { return static_cast<std::size_t>(sequence_diff(next_send_id_, oldest_unacked_)); }

private:
    struct Outgoing {
        bool valid = false;
        bool acked = false;
        std::uint16_t id = 0;
        std::uint32_t length = 0;
        std::uint16_t fragment_count = 0;   // 0: a whole message in inline_bytes
        std::uint16_t fragments_acked = 0;
        double last_send = -1e9;
        std::uint8_t inline_bytes[kFragmentBytes];
        std::vector<std::uint8_t> block;
        std::vector<double> fragment_last_send;
        std::vector<std::uint8_t> fragment_acked;
        const std::uint8_t* data() const { return fragment_count == 0 ? inline_bytes : block.data(); }
    };
    struct Incoming {
        bool valid = false;
        bool complete = false;
        std::uint16_t id = 0;
        std::uint32_t length = 0;
        std::uint16_t fragment_count = 0;
        std::uint16_t fragments_received = 0;
        std::uint8_t inline_bytes[kFragmentBytes];
        std::vector<std::uint8_t> block;
        std::vector<std::uint8_t> fragment_present;
        const std::uint8_t* data() const { return fragment_count == 0 ? inline_bytes : block.data(); }
    };
    static bool due(double last_send, double now, double rtt) { return now - last_send >= rtt * 1.25; }
    static std::size_t entry_bits(bool fragment, std::size_t length) {
        // The continuation bit, id, fragment count, index, length, alignment, bytes, and the terminating bit.
        return 1 + 16 + 16 + (fragment ? 16 : 0) + 16 + 7 + length * 8 + 1;
    }

    // Each window is about 260 KB, made once here so a Connection stays small
    // enough for a stack and nothing allocates per packet.
    std::vector<Outgoing> out_ = std::vector<Outgoing>(kWindowSize);
    std::uint16_t next_send_id_ = 0;
    std::uint16_t oldest_unacked_ = 0;
    std::vector<Incoming> in_ = std::vector<Incoming>(kWindowSize);
    std::uint16_t next_deliver_ = 0;
};

}  // namespace network
```

- [ ] **Step 4: Write `ReliableChannel.cpp`**

```cpp
#include "ReliableChannel.hpp"

#include <algorithm>
#include <cstring>

namespace network {

bool ReliableChannel::send(const std::uint8_t* bytes, std::size_t length) {
    if (length > kMaxBlockBytes) return false;
    if (unacked() >= kWindowSize) return false;
    Outgoing& out = out_[next_send_id_ % kWindowSize];
    out.valid = true;
    out.acked = false;
    out.id = next_send_id_;
    out.length = static_cast<std::uint32_t>(length);
    out.fragments_acked = 0;
    out.last_send = -1e9;
    if (length <= kFragmentBytes) {
        out.fragment_count = 0;
        if (length > 0) std::memcpy(out.inline_bytes, bytes, length);
        out.block = std::vector<std::uint8_t>();
    } else {
        out.fragment_count = static_cast<std::uint16_t>((length + kFragmentBytes - 1) / kFragmentBytes);
        out.block.assign(bytes, bytes + length);
        out.fragment_last_send.assign(out.fragment_count, -1e9);
        out.fragment_acked.assign(out.fragment_count, 0);
    }
    ++next_send_id_;
    return true;
}

bool ReliableChannel::has_due(double now, double rtt) const { return bytes_due(now, rtt) > 0; }

std::size_t ReliableChannel::bytes_due(double now, double rtt) const {
    std::size_t total = 0;
    for (std::uint16_t id = oldest_unacked_; id != next_send_id_; ++id) {
        const Outgoing& out = out_[id % kWindowSize];
        if (!out.valid || out.acked) continue;
        if (out.fragment_count == 0) {
            if (due(out.last_send, now, rtt)) total += entry_bits(false, out.length) / 8;
            continue;
        }
        for (std::uint16_t f = 0; f < out.fragment_count; ++f) {
            if (out.fragment_acked[f] || !due(out.fragment_last_send[f], now, rtt)) continue;
            total += entry_bits(true, std::min(kFragmentBytes, out.length - f * kFragmentBytes)) / 8;
        }
    }
    return total;
}

void ReliableChannel::write(BitWriter& writer, double now, double rtt, SentPacket& packet, std::size_t reserve_bytes) {
    const std::size_t reserved = reserve_bytes * 8;
    const auto room = [&]() { return writer.bits_left() > reserved ? writer.bits_left() - reserved : 0; };
    for (std::uint16_t id = oldest_unacked_; id != next_send_id_; ++id) {
        Outgoing& out = out_[id % kWindowSize];
        if (!out.valid || out.acked) continue;
        if (packet.ref_count >= kMaxRefsPerPacket) break;
        if (out.fragment_count == 0) {
            if (!due(out.last_send, now, rtt)) continue;
            if (room() < entry_bits(false, out.length)) continue;   // a smaller one later may still fit
            writer.boolean(true);
            writer.bits(out.id, 16);
            writer.bits(0, 16);
            writer.bits(out.length, 16);
            writer.bytes(out.inline_bytes, out.length);
            out.last_send = now;
            packet.refs[packet.ref_count++] = MessageRef{out.id, kWholeMessage};
            continue;
        }
        for (std::uint16_t f = 0; f < out.fragment_count; ++f) {
            if (out.fragment_acked[f] || !due(out.fragment_last_send[f], now, rtt)) continue;
            if (packet.ref_count >= kMaxRefsPerPacket) break;
            const std::size_t length = std::min(kFragmentBytes, out.length - f * kFragmentBytes);
            if (room() < entry_bits(true, length)) break;
            writer.boolean(true);
            writer.bits(out.id, 16);
            writer.bits(out.fragment_count, 16);
            writer.bits(f, 16);
            writer.bits(static_cast<std::uint32_t>(length), 16);
            writer.bytes(out.block.data() + f * kFragmentBytes, length);
            out.fragment_last_send[f] = now;
            packet.refs[packet.ref_count++] = MessageRef{out.id, f};
        }
    }
    writer.boolean(false);
}

namespace {

// One entry's fields. False when they cannot be right.
bool read_entry(BitReader& reader, std::uint16_t& id, std::uint16_t& fragment_count, std::uint16_t& fragment_index,
                std::uint32_t& length) {
    id = static_cast<std::uint16_t>(reader.bits(16));
    fragment_count = static_cast<std::uint16_t>(reader.bits(16));
    fragment_index = fragment_count > 0 ? static_cast<std::uint16_t>(reader.bits(16)) : 0;
    length = reader.bits(16);
    if (reader.failed() || length > kFragmentBytes) return false;
    if (fragment_count > 0) {
        if (fragment_index >= fragment_count) return false;
        if (static_cast<std::size_t>(fragment_count) * kFragmentBytes > kMaxBlockBytes) return false;
        if (fragment_index + 1 < fragment_count && length != kFragmentBytes) return false;
    }
    reader.align();
    return reader.bytes_left() >= length;
}

}  // namespace

bool ReliableChannel::validate(BitReader reader) const {
    std::uint8_t scratch[kFragmentBytes];
    while (reader.boolean()) {
        if (reader.failed()) return false;
        std::uint16_t id, fragment_count, fragment_index;
        std::uint32_t length;
        if (!read_entry(reader, id, fragment_count, fragment_index, length)) return false;
        reader.bytes(scratch, length);
    }
    return !reader.failed();
}

bool ReliableChannel::read(BitReader& reader) {
    if (!validate(reader)) return false;
    std::uint8_t scratch[kFragmentBytes];
    while (reader.boolean()) {
        std::uint16_t id, fragment_count, fragment_index;
        std::uint32_t length;
        read_entry(reader, id, fragment_count, fragment_index, length);
        const int offset = sequence_diff(id, next_deliver_);
        if (offset < 0 || offset >= static_cast<int>(kWindowSize)) {
            reader.bytes(scratch, length);   // already delivered, or beyond the window: skip it
            continue;
        }
        Incoming& in = in_[id % kWindowSize];
        if (!in.valid || in.id != id) {
            in.valid = true;
            in.complete = false;
            in.id = id;
            in.length = 0;
            in.fragment_count = fragment_count;
            in.fragments_received = 0;
            if (fragment_count > 0) {
                in.block.assign(static_cast<std::size_t>(fragment_count) * kFragmentBytes, 0);
                in.fragment_present.assign(fragment_count, 0);
            }
        }
        if (in.complete || in.fragment_count != fragment_count) {
            reader.bytes(scratch, length);
            continue;
        }
        if (fragment_count == 0) {
            reader.bytes(in.inline_bytes, length);
            in.length = length;
            in.complete = true;
            continue;
        }
        if (in.fragment_present[fragment_index]) {
            reader.bytes(scratch, length);
            continue;
        }
        reader.bytes(in.block.data() + static_cast<std::size_t>(fragment_index) * kFragmentBytes, length);
        in.fragment_present[fragment_index] = 1;
        ++in.fragments_received;
        if (fragment_index + 1 == fragment_count) in.length = fragment_index * kFragmentBytes + length;
        if (in.fragments_received == fragment_count) in.complete = true;
    }
    return !reader.failed();
}

void ReliableChannel::acked(const MessageRef& ref) {
    Outgoing& out = out_[ref.id % kWindowSize];
    if (!out.valid || out.id != ref.id || out.acked) return;
    if (ref.fragment == kWholeMessage) {
        if (out.fragment_count != 0) return;
        out.acked = true;
    } else {
        if (ref.fragment >= out.fragment_count || out.fragment_acked[ref.fragment]) return;
        out.fragment_acked[ref.fragment] = 1;
        if (++out.fragments_acked == out.fragment_count) out.acked = true;
    }
    // The window slides past every acked message at its front.
    while (sequence_diff(next_send_id_, oldest_unacked_) > 0) {
        Outgoing& front = out_[oldest_unacked_ % kWindowSize];
        if (!front.acked) break;
        front.valid = false;
        front.block = std::vector<std::uint8_t>();
        front.fragment_last_send = std::vector<double>();
        front.fragment_acked = std::vector<std::uint8_t>();
        ++oldest_unacked_;
    }
}

}  // namespace network
```

- [ ] **Step 5: Build and run**

Run: build, then `./build/Release/sandbox.exe "[network]"`. Expected: N30 to N36 pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/network sandbox/network_reliable_tests.cpp
git commit -m "Network: the reliable channel sends messages and blocks until acked

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: Flow control

**Files:**
- Create: `src/network/FlowControl.hpp`, `src/network/FlowControl.cpp`, `sandbox/network_flow_tests.cpp`
- Modify: `CMakeLists.txt` (add the .cpp; add the test file)

**Interfaces:**
- Produces:
  ```cpp
  namespace network {
  class FlowControl {
  public:
      explicit FlowControl(double bytes_per_second);
      void update(double now, double rtt, float loss);
      double bytes_per_second() const;    // the configured rate times the current fraction
      bool good() const;                  // fraction is 1
  };
  }
  ```

- [ ] **Step 1: Write the failing tests**

`sandbox/network_flow_tests.cpp`:

```cpp
// The two-mode budget: full, or half while the link is bad (spec section 4).

#include "FlowControl.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using network::FlowControl;

TEST_CASE("N40 a good link keeps the configured rate", "[network][flow]") {
    FlowControl flow(32000.0);
    flow.update(0.0, 0.05, 0.0f);
    flow.update(1.0, 0.05, 0.05f);
    REQUIRE(flow.good());
    REQUIRE(flow.bytes_per_second() == Approx(32000.0));
}

TEST_CASE("N41 loss over 10% or RTT over 250 ms halves the rate at once", "[network][flow]") {
    FlowControl by_loss(32000.0);
    by_loss.update(0.0, 0.05, 0.0f);
    by_loss.update(0.1, 0.05, 0.11f);
    REQUIRE_FALSE(by_loss.good());
    REQUIRE(by_loss.bytes_per_second() == Approx(16000.0));
    FlowControl by_rtt(32000.0);
    by_rtt.update(0.0, 0.05, 0.0f);
    by_rtt.update(0.1, 0.26, 0.0f);
    REQUIRE(by_rtt.bytes_per_second() == Approx(16000.0));
}

TEST_CASE("N42 the rate recovers 10% a second and stops at the configured rate", "[network][flow]") {
    FlowControl flow(32000.0);
    flow.update(0.0, 0.05, 0.0f);
    flow.update(0.1, 0.05, 0.5f);
    REQUIRE(flow.bytes_per_second() == Approx(16000.0));
    flow.update(1.1, 0.05, 0.0f);
    REQUIRE(flow.bytes_per_second() == Approx(32000.0 * 0.6));
    flow.update(3.1, 0.05, 0.0f);
    REQUIRE(flow.bytes_per_second() == Approx(32000.0 * 0.8));
    flow.update(10.0, 0.05, 0.0f);
    REQUIRE(flow.good());
    REQUIRE(flow.bytes_per_second() == Approx(32000.0));
    // Bad again while recovering: back to half.
    flow.update(10.5, 0.05, 0.2f);
    REQUIRE(flow.bytes_per_second() == Approx(16000.0));
}
```

Add the test file to the sandbox list and `src/network/FlowControl.cpp` to `anarchy_network`.

- [ ] **Step 2: Build to see the failure**

Run: build. Expected: `FlowControl.hpp` cannot be opened.

- [ ] **Step 3: Write `FlowControl.hpp` and `FlowControl.cpp`**

`FlowControl.hpp`:

```cpp
#pragma once

namespace network {

// Fiedler's two-mode flow control: the full rate on a good link, half of it
// the moment loss passes 10% or RTT passes 250 ms, and 10% a second back
// toward full once the link is good again.
class FlowControl {
public:
    explicit FlowControl(double bytes_per_second) : configured_(bytes_per_second) {}
    void update(double now, double rtt, float loss);
    double bytes_per_second() const { return configured_ * fraction_; }
    bool good() const { return fraction_ >= 1.0; }

private:
    double configured_;
    double fraction_ = 1.0;
    double last_update_ = -1.0;
};

}  // namespace network
```

`FlowControl.cpp`:

```cpp
#include "FlowControl.hpp"

#include <algorithm>

namespace network {

void FlowControl::update(double now, double rtt, float loss) {
    const double dt = last_update_ < 0.0 ? 0.0 : std::max(0.0, now - last_update_);
    last_update_ = now;
    if (loss > 0.10f || rtt > 0.250) {
        fraction_ = 0.5;
        return;
    }
    fraction_ = std::min(1.0, fraction_ + 0.1 * dt);
}

}  // namespace network
```

- [ ] **Step 4: Build and run**

Run: build, then `./build/Release/sandbox.exe "[network]"`. Expected: N40 to N42 pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/network sandbox/network_flow_tests.cpp
git commit -m "Network: two-mode flow control

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: Connection

**Files:**
- Create: `src/network/Connection.hpp`, `src/network/Connection.cpp`, `sandbox/network_connection_tests.cpp`
- Modify: `CMakeLists.txt` (add the .cpp; add the test file)

**Interfaces:**
- Consumes: everything from Tasks 1, 2, 5, 6: `BitWriter`, `BitReader`, `PacketHeader`, `write_header`, `read_header`, `encode_packet`, `kMaxPayloadBytes`, `PacketRing`, `SentPacket`, `ReliableChannel` (with `validate`, `read`, `write(writer, now, rtt, packet, reserve_bytes)`, `bytes_due`, `has_due`, `deliver`, `acked`), `FlowControl`.
- Produces:
  ```cpp
  namespace network {
  struct ConnectionStats {
      double rtt = 0.0; float loss = 0.0f;
      double bytes_in_per_second = 0.0, bytes_out_per_second = 0.0, budget_bytes_per_second = 0.0;
      std::uint64_t packets_sent = 0, packets_received = 0, dropped_replay = 0, dropped_malformed = 0;
  };
  struct ConnectionCallbacks {
      std::function<void(const std::uint8_t*, std::size_t)> on_message;
      std::function<void(std::uint16_t sequence, const std::uint8_t*, std::size_t)> on_payload;
      std::function<void(std::uint16_t sequence, std::uint32_t tag)> on_delivered;
  };
  class Connection {
  public:
      Connection(double bytes_per_second, double send_interval);
      ConnectionCallbacks callbacks;
      void set_session(std::uint16_t session); std::uint16_t session() const;
      bool send_message(const std::uint8_t* bytes, std::size_t length);
      std::size_t payload_budget(double now) const;
      std::optional<std::uint16_t> set_payload(const std::uint8_t* bytes, std::size_t length, std::uint32_t tag);
      bool wants_packet(double now) const;
      std::size_t write_packet(std::uint8_t* out, std::size_t capacity, double now);   // bytes to send; 0 never happens for a wanted packet
      // reader is positioned just after the header; bytes/length are the whole decoded packet.
      bool read_packet(const PacketHeader& header, BitReader& reader, const std::uint8_t* bytes, std::size_t length, double now);
      double last_receive() const;   // -1 before any packet
      double rtt() const;            // 0.1 until a sample
      ConnectionStats stats(double now) const;
  };
  }
  ```

- [ ] **Step 1: Write the failing tests**

`sandbox/network_connection_tests.cpp`:

```cpp
// One peer's state: packets out and in, acks, RTT, the payload budget
// (spec sections 1 to 4).

#include "Bits.hpp"
#include "Connection.hpp"
#include "Packet.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

using Catch::Approx;
using network::BitReader;
using network::Connection;
using network::PacketHeader;
using network::PacketType;

namespace {

struct Peer {
    Connection c{32000.0, 1.0 / 60.0};
    std::vector<std::vector<std::uint8_t>> messages;
    std::vector<std::pair<std::uint16_t, std::vector<std::uint8_t>>> payloads;
    std::vector<std::pair<std::uint16_t, std::uint32_t>> delivered;
    Peer() {
        c.callbacks.on_message = [this](const std::uint8_t* d, std::size_t n) { messages.emplace_back(d, d + n); };
        c.callbacks.on_payload = [this](std::uint16_t s, const std::uint8_t* d, std::size_t n) {
            payloads.emplace_back(s, std::vector<std::uint8_t>(d, d + n));
        };
        c.callbacks.on_delivered = [this](std::uint16_t s, std::uint32_t tag) { delivered.emplace_back(s, tag); };
    }
};

// Reads the header the way Endpoint will, then hands the rest to the connection.
bool feed(Peer& to, std::uint8_t* bytes, std::size_t length, double now) {
    length = network::decode_packet(bytes, length);
    BitReader reader(bytes, length);
    PacketHeader header;
    REQUIRE(network::read_header(reader, header));
    return to.c.read_packet(header, reader, bytes, length, now);
}

PacketType exchange(Peer& from, Peer& to, double now) {
    std::uint8_t buffer[network::kMaxPacketBytes];
    const std::size_t length = from.c.write_packet(buffer, sizeof(buffer), now);
    REQUIRE(length >= network::kHeaderBytes + 1);
    const PacketType type = static_cast<PacketType>(buffer[2]);
    feed(to, buffer, length, now);
    return type;
}

}  // namespace

TEST_CASE("N44 an ack that arrives before its packet's send time leaves RTT at zero, not negative", "[network][connection]") {
    Peer a, b;
    exchange(a, b, 1.0);
    // b's reply acks a's packet, delivered with a clock that went backwards.
    exchange(b, a, 0.5);
    REQUIRE(a.c.rtt() >= 0.0);
    REQUIRE(a.c.rtt() == Approx(0.0));
}

TEST_CASE("N45 a message, a payload, and its delivery report cross a round trip", "[network][connection]") {
    Peer a, b;
    const std::uint8_t hello[5] = {'h', 'e', 'l', 'l', 'o'};
    const std::uint8_t state[3] = {1, 2, 3};
    REQUIRE(a.c.send_message(hello, 5));
    const auto sequence = a.c.set_payload(state, 3, 77);
    REQUIRE(sequence.has_value());
    REQUIRE(exchange(a, b, 0.0) == PacketType::Payload);
    REQUIRE(b.messages.size() == 1);
    REQUIRE(std::memcmp(b.messages[0].data(), hello, 5) == 0);
    REQUIRE(b.payloads.size() == 1);
    REQUIRE(b.payloads[0].first == *sequence);
    REQUIRE(b.payloads[0].second == std::vector<std::uint8_t>{1, 2, 3});
    REQUIRE(a.delivered.empty());
    exchange(b, a, 0.04);
    REQUIRE(a.delivered.size() == 1);
    REQUIRE(a.delivered[0].first == *sequence);
    REQUIRE(a.delivered[0].second == 77);
    REQUIRE(a.c.rtt() == Approx(0.04));
    // A keep-alive from b carried nothing to report.
    REQUIRE(b.delivered.empty());
}

TEST_CASE("N46 a quiet connection wants a keep-alive every 100 ms and nothing sooner", "[network][connection]") {
    Peer a;
    REQUIRE(a.c.wants_packet(0.0));   // the first packet
    std::uint8_t buffer[network::kMaxPacketBytes];
    REQUIRE(a.c.write_packet(buffer, sizeof(buffer), 0.0) == network::kHeaderBytes + 1);
    REQUIRE(static_cast<PacketType>(buffer[2]) == PacketType::KeepAlive);
    REQUIRE_FALSE(a.c.wants_packet(0.05));
    REQUIRE(a.c.wants_packet(0.1));
    const std::uint8_t byte = 1;
    a.c.send_message(&byte, 1);
    REQUIRE(a.c.wants_packet(0.0));   // a message is due at once
}

TEST_CASE("N47 a replayed packet is dropped and counted", "[network][connection]") {
    Peer a, b;
    std::uint8_t buffer[network::kMaxPacketBytes];
    const std::size_t length = a.c.write_packet(buffer, sizeof(buffer), 0.0);
    std::uint8_t copy[network::kMaxPacketBytes];
    std::memcpy(copy, buffer, length);
    REQUIRE(feed(b, buffer, length, 0.0));
    REQUIRE_FALSE(feed(b, copy, length, 0.01));
    REQUIRE(b.c.stats(0.01).dropped_replay == 1);
    REQUIRE(b.c.stats(0.01).packets_received == 1);
}

TEST_CASE("N48 a malformed reliable section drops the packet without marking its sequence received", "[network][connection]") {
    Peer a, b;
    const std::uint8_t byte = 9;
    a.c.send_message(&byte, 1);
    std::uint8_t buffer[network::kMaxPacketBytes];
    const std::size_t length = a.c.write_packet(buffer, sizeof(buffer), 0.0);
    // Corrupt the entry's length field (bits 49..64 after the 13-byte header and the continuation bit).
    std::uint8_t broken[network::kMaxPacketBytes];
    std::memcpy(broken, buffer, length);
    broken[network::kHeaderBytes + 4] = 0xFF;
    broken[network::kHeaderBytes + 5] = 0xFF;
    REQUIRE_FALSE(feed(b, broken, length, 0.0));
    REQUIRE(b.c.stats(0.0).dropped_malformed == 1);
    REQUIRE(b.messages.empty());
    // The good copy of the same sequence is still accepted.
    REQUIRE(feed(b, buffer, length, 0.01));
    REQUIRE(b.messages.size() == 1);
}

TEST_CASE("N49 the payload budget is the tick's share of the rate, less what the reliable channel needs", "[network][connection]") {
    Peer a;
    REQUIRE(a.c.payload_budget(0.0) == 533);   // 32000 / 60
    std::vector<std::uint8_t> message(100, 1);
    a.c.send_message(message.data(), message.size());
    REQUIRE(a.c.payload_budget(0.0) < 533);
    REQUIRE(a.c.payload_budget(0.0) >= 533 - 110);
    Connection fast(10'000'000.0, 1.0 / 60.0);
    REQUIRE(fast.payload_budget(0.0) == network::kMaxPayloadBytes - 1);
}

TEST_CASE("N50 stats count packets and bytes per second", "[network][connection]") {
    Peer a, b;
    // Both directions, so a's packets are acked and its budget stays full.
    for (int i = 0; i < 60; ++i) {
        exchange(a, b, i / 60.0);
        exchange(b, a, i / 60.0);
    }
    exchange(a, b, 1.0);
    exchange(b, a, 1.0);
    REQUIRE(a.c.stats(1.0).packets_sent == 61);
    REQUIRE(b.c.stats(1.0).packets_received == 61);
    REQUIRE(a.c.stats(1.0).bytes_out_per_second == Approx(60.0 * 14.0).epsilon(0.05));
    REQUIRE(b.c.stats(1.0).bytes_in_per_second == Approx(60.0 * 14.0).epsilon(0.05));
    REQUIRE(a.c.stats(1.0).budget_bytes_per_second == Approx(32000.0));
}

TEST_CASE("N51 a payload keeps its room beside a full reliable section, and oversized or doubled payloads are refused", "[network][connection]") {
    Peer a, b;
    std::vector<std::uint8_t> big(1000, 5);
    std::vector<std::uint8_t> state(1000, 6);
    REQUIRE(a.c.send_message(big.data(), big.size()));
    REQUIRE(a.c.set_payload(state.data(), state.size(), 1).has_value());
    REQUIRE_FALSE(a.c.set_payload(state.data(), state.size(), 2).has_value());   // one per packet
    exchange(a, b, 0.0);
    REQUIRE(b.payloads.size() == 1);
    REQUIRE(b.payloads[0].second == state);
    REQUIRE(b.messages.empty());   // the message did not fit beside it and waits
    exchange(a, b, 0.2);
    REQUIRE(b.messages.size() == 1);
    REQUIRE(b.messages[0] == big);
    std::vector<std::uint8_t> huge(network::kMaxPayloadBytes + 1, 0);
    REQUIRE_FALSE(a.c.set_payload(huge.data(), huge.size(), 3).has_value());
}
```

Add the test file to the sandbox list and `src/network/Connection.cpp` to `anarchy_network`.

- [ ] **Step 2: Build to see the failure**

Run: build. Expected: `Connection.hpp` cannot be opened.

- [ ] **Step 3: Write `Connection.hpp`**

```cpp
#pragma once

#include "Bits.hpp"
#include "FlowControl.hpp"
#include "Packet.hpp"
#include "PacketRing.hpp"
#include "ReliableChannel.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace network {

struct ConnectionStats {
    double rtt = 0.0;
    float loss = 0.0f;
    double bytes_in_per_second = 0.0;
    double bytes_out_per_second = 0.0;
    double budget_bytes_per_second = 0.0;
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t dropped_replay = 0;
    std::uint64_t dropped_malformed = 0;
};

// Set once at connect; none is called from inside another.
struct ConnectionCallbacks {
    std::function<void(const std::uint8_t*, std::size_t)> on_message;
    std::function<void(std::uint16_t sequence, const std::uint8_t*, std::size_t)> on_payload;
    std::function<void(std::uint16_t sequence, std::uint32_t tag)> on_delivered;
};

// One peer: what we sent it and what it sent us. Endpoint routes packets
// to it and asks it for the packet to send each tick.
class Connection {
public:
    Connection(double bytes_per_second, double send_interval);

    ConnectionCallbacks callbacks;

    void set_session(std::uint16_t session) { session_ = session; }
    std::uint16_t session() const { return session_; }

    bool send_message(const std::uint8_t* bytes, std::size_t length) { return reliable_.send(bytes, length); }
    // Payload bytes this tick's packet may carry: the rate's share of a tick,
    // less what the reliable channel is due to send, never more than fits.
    std::size_t payload_budget(double now) const;
    // This tick's unreliable payload, once; returns the sequence it will ride in.
    std::optional<std::uint16_t> set_payload(const std::uint8_t* bytes, std::size_t length, std::uint32_t tag);
    bool wants_packet(double now) const;
    std::size_t write_packet(std::uint8_t* out, std::size_t capacity, double now);
    bool read_packet(const PacketHeader& header, BitReader& reader, const std::uint8_t* bytes, std::size_t length, double now);

    double last_receive() const { return last_receive_; }
    double rtt() const { return has_rtt_ ? rtt_ : 0.1; }
    ConnectionStats stats(double now) const;

private:
    double loss_timeout() const { return rtt() * 2.0 > 0.1 ? rtt() * 2.0 : 0.1; }
    void sample_rtt(double sample);
    void account(double& window_bytes, double& rate, double& window_start, std::size_t bytes, double now);

    PacketRing ring_;
    ReliableChannel reliable_;
    FlowControl flow_;
    double send_interval_;
    std::uint16_t session_ = 0;
    std::uint16_t next_sequence_ = 0;
    double rtt_ = 0.0;
    bool has_rtt_ = false;
    double last_send_ = -1.0;
    double last_receive_ = -1.0;
    bool has_payload_ = false;
    std::uint16_t payload_length_ = 0;
    std::uint32_t payload_tag_ = 0;
    std::uint8_t payload_[kMaxPayloadBytes];
    ConnectionStats stats_;
    double in_window_bytes_ = 0.0, in_rate_ = 0.0, in_window_start_ = -1.0;
    double out_window_bytes_ = 0.0, out_rate_ = 0.0, out_window_start_ = -1.0;
};

}  // namespace network
```

- [ ] **Step 4: Write `Connection.cpp`**

```cpp
#include "Connection.hpp"

#include <algorithm>
#include <cstring>

namespace network {

Connection::Connection(double bytes_per_second, double send_interval)
    : flow_(bytes_per_second), send_interval_(send_interval) {}

std::size_t Connection::payload_budget(double now) const {
    const std::size_t due = reliable_.bytes_due(now, rtt());
    // One byte stays for the section's terminating bit.
    const std::size_t room = kMaxPayloadBytes - 1;
    const std::size_t space = due >= room ? 0 : room - due;
    const double tick = flow_.bytes_per_second() * send_interval_;
    const std::size_t by_rate = tick > static_cast<double>(due) ? static_cast<std::size_t>(tick - static_cast<double>(due)) : 0;
    return std::min(space, by_rate);
}

std::optional<std::uint16_t> Connection::set_payload(const std::uint8_t* bytes, std::size_t length, std::uint32_t tag) {
    if (has_payload_ || length > kMaxPayloadBytes - 1) return std::nullopt;
    if (length > 0) std::memcpy(payload_, bytes, length);
    payload_length_ = static_cast<std::uint16_t>(length);
    payload_tag_ = tag;
    has_payload_ = true;
    return next_sequence_;
}

bool Connection::wants_packet(double now) const {
    return has_payload_ || reliable_.has_due(now, rtt()) || last_send_ < 0.0 || now - last_send_ >= 0.1;
}

std::size_t Connection::write_packet(std::uint8_t* out, std::size_t capacity, double now) {
    PacketHeader header;
    header.type = has_payload_ ? PacketType::Payload : PacketType::KeepAlive;
    header.session = session_;
    header.sequence = next_sequence_;
    header.ack = ring_.ack();
    header.ack_bits = ring_.ack_bits();
    BitWriter writer(out, capacity);
    write_header(writer, header);
    SentPacket& sent = ring_.record_sent(next_sequence_, now);
    // The payload was budgeted first; the reliable section takes what is left.
    reliable_.write(writer, now, rtt(), sent, has_payload_ ? payload_length_ + 1u : 0u);
    writer.align();
    if (has_payload_) {
        writer.bytes(payload_, payload_length_);
        sent.has_payload = true;
        sent.tag = payload_tag_;
    }
    const std::size_t length = writer.bytes_written();
    sent.bytes = static_cast<std::uint16_t>(length);
    has_payload_ = false;
    ++next_sequence_;
    last_send_ = now;
    ++stats_.packets_sent;
    account(out_window_bytes_, out_rate_, out_window_start_, length, now);
    flow_.update(now, rtt(), ring_.loss(now, loss_timeout()));
    return encode_packet(out, length, capacity);
}

bool Connection::read_packet(const PacketHeader& header, BitReader& reader, const std::uint8_t* bytes, std::size_t length, double now) {
    // Checked before anything is recorded, so a bad packet is as if it never came.
    if (!reliable_.validate(reader)) {
        ++stats_.dropped_malformed;
        return false;
    }
    if (ring_.note_received(header.sequence) != PacketRing::Receive::Accepted) {
        ++stats_.dropped_replay;
        return false;
    }
    ring_.process_acks(header.ack, header.ack_bits, now, [&](SentPacket& sent, double sample) {
        sample_rtt(sample);
        for (int i = 0; i < sent.ref_count; ++i) reliable_.acked(sent.refs[i]);
        if (sent.has_payload && callbacks.on_delivered) callbacks.on_delivered(sent.sequence, sent.tag);
    });
    reliable_.read(reader);
    reader.align();
    const std::size_t offset = reader.byte_offset();
    last_receive_ = now;
    ++stats_.packets_received;
    account(in_window_bytes_, in_rate_, in_window_start_, length, now);
    flow_.update(now, rtt(), ring_.loss(now, loss_timeout()));
    if (offset < length && callbacks.on_payload) callbacks.on_payload(header.sequence, bytes + offset, length - offset);
    if (callbacks.on_message) {
        reliable_.deliver(callbacks.on_message);
    } else {
        reliable_.deliver([](const std::uint8_t*, std::size_t) {});
    }
    return true;
}

void Connection::sample_rtt(double sample) {
    sample = std::max(0.0, sample);
    rtt_ = has_rtt_ ? rtt_ + (sample - rtt_) * 0.1 : sample;
    has_rtt_ = true;
}

// One-second windows; the rate reported is the last whole window's.
void Connection::account(double& window_bytes, double& rate, double& window_start, std::size_t bytes, double now) {
    if (window_start < 0.0) window_start = now;
    if (now - window_start >= 1.0) {
        rate = window_bytes / (now - window_start);
        window_bytes = 0.0;
        window_start = now;
    }
    window_bytes += static_cast<double>(bytes);
}

ConnectionStats Connection::stats(double now) const {
    ConnectionStats stats = stats_;
    stats.rtt = rtt();
    stats.loss = ring_.loss(now, loss_timeout());
    stats.bytes_in_per_second = in_rate_;
    stats.bytes_out_per_second = out_rate_;
    stats.budget_bytes_per_second = flow_.bytes_per_second();
    return stats;
}

}  // namespace network
```

- [ ] **Step 5: Build and run**

Run: build, then `./build/Release/sandbox.exe "[network]"`. Expected: N44 to N51 pass, and N30 to N36 still pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/network sandbox/network_connection_tests.cpp
git commit -m "Network: a Connection writes and reads one peer's packets

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: Game identity and the resources hash

**Files:**
- Create: `src/network/GameIdentity.hpp`, `src/network/GameIdentity.cpp`, `sandbox/network_identity_tests.cpp`
- Modify: `CMakeLists.txt` (add the .cpp; add the test file)

**Interfaces:**
- Consumes: `BitWriter`, `BitReader` (Task 1); zstd's `common/xxhash.h`.
- Produces:
  ```cpp
  namespace network {
  struct GameIdentity {
      std::array<std::uint8_t, 16> game_id{};
      std::string version = "1.0.0";
      std::uint64_t resources_hash = 0;
  };
  bool parse_game_id(std::string_view hex32, std::array<std::uint8_t, 16>& out);
  std::string format_game_id(const std::array<std::uint8_t, 16>& id);   // 32 lowercase hex characters
  std::uint64_t hash_resources(const std::filesystem::path& folder);    // 0 when the folder is missing
  void write_identity(BitWriter& writer, const GameIdentity& identity); // version clamped to 255 bytes
  bool read_identity(BitReader& reader, GameIdentity& identity);
  }
  ```

- [ ] **Step 1: Write the failing tests**

`sandbox/network_identity_tests.cpp`:

```cpp
// Which game a client belongs to, and whether it has the server's resources
// (spec section 5).

#include "Bits.hpp"
#include "GameIdentity.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using network::GameIdentity;

namespace {

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

}  // namespace

TEST_CASE("N52 a game id is 32 hex characters, parsed and printed", "[network][identity]") {
    std::array<std::uint8_t, 16> id{};
    REQUIRE(network::parse_game_id("00112233445566778899aabbccddeeff", id));
    REQUIRE(id[0] == 0x00);
    REQUIRE(id[1] == 0x11);
    REQUIRE(id[15] == 0xFF);
    REQUIRE(network::format_game_id(id) == "00112233445566778899aabbccddeeff");
    REQUIRE(network::parse_game_id("00112233445566778899AABBCCDDEEFF", id));
    REQUIRE_FALSE(network::parse_game_id("0011223344556677", id));
    REQUIRE_FALSE(network::parse_game_id("00112233445566778899aabbccddeegg", id));
}

TEST_CASE("N53 the resources hash depends on paths and bytes, not on the order files were made", "[network][identity]") {
    TempDir a;
    write(a.path / "textures" / "brick.png", "bricks");
    write(a.path / "meshes" / "cube.amesh", "cube");
    TempDir b;
    write(b.path / "meshes" / "cube.amesh", "cube");
    write(b.path / "textures" / "brick.png", "bricks");
    const std::uint64_t ha = network::hash_resources(a.path);
    REQUIRE(ha != 0);
    REQUIRE(ha == network::hash_resources(b.path));
    write(b.path / "textures" / "brick.png", "brickz");
    REQUIRE(ha != network::hash_resources(b.path));
    TempDir c;
    write(c.path / "textures" / "wall.png", "bricks");
    write(c.path / "meshes" / "cube.amesh", "cube");
    REQUIRE(ha != network::hash_resources(c.path));
    REQUIRE(network::hash_resources(a.path / "missing") == 0);
}

TEST_CASE("N54 an identity round-trips, with the version clamped to 255 bytes", "[network][identity]") {
    GameIdentity in;
    REQUIRE(network::parse_game_id("0123456789abcdef0123456789abcdef", in.game_id));
    in.version = "2.3.4-beta";
    in.resources_hash = 0x1122334455667788ull;
    std::uint8_t buffer[512];
    network::BitWriter writer(buffer, sizeof(buffer));
    network::write_identity(writer, in);
    GameIdentity out;
    network::BitReader reader(buffer, writer.bytes_written());
    REQUIRE(network::read_identity(reader, out));
    REQUIRE(out.game_id == in.game_id);
    REQUIRE(out.version == in.version);
    REQUIRE(out.resources_hash == in.resources_hash);
    in.version = std::string(300, 'v');
    network::BitWriter long_writer(buffer, sizeof(buffer));
    network::write_identity(long_writer, in);
    network::BitReader long_reader(buffer, long_writer.bytes_written());
    REQUIRE(network::read_identity(long_reader, out));
    REQUIRE(out.version.size() == 255);
    network::BitReader cut(buffer, 10);
    REQUIRE_FALSE(network::read_identity(cut, out));
}
```

Add the test file to the sandbox list and `src/network/GameIdentity.cpp` to `anarchy_network`.

- [ ] **Step 2: Build to see the failure**

Run: build. Expected: `GameIdentity.hpp` cannot be opened.

- [ ] **Step 3: Write `GameIdentity.hpp`**

```cpp
#pragma once

#include "Bits.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace network {

// What a client sends to prove it belongs to this game: the project's
// gameId, its version, and a hash of the resources folder it shipped with.
struct GameIdentity {
    std::array<std::uint8_t, 16> game_id{};
    std::string version = "1.0.0";
    std::uint64_t resources_hash = 0;
};

bool parse_game_id(std::string_view hex32, std::array<std::uint8_t, 16>& out);
std::string format_game_id(const std::array<std::uint8_t, 16>& id);

// XXH64 over every file under folder: its path relative to folder with '/'
// separators, then its bytes, in sorted path order. 0 for a missing folder.
std::uint64_t hash_resources(const std::filesystem::path& folder);

void write_identity(BitWriter& writer, const GameIdentity& identity);
bool read_identity(BitReader& reader, GameIdentity& identity);

}  // namespace network
```

- [ ] **Step 4: Write `GameIdentity.cpp`**

```cpp
#include "GameIdentity.hpp"

#include "common/xxhash.h"   // zstd's copy; its names are prefixed ZSTD_ through XXH_NAMESPACE

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <utility>
#include <vector>

namespace network {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

}  // namespace

bool parse_game_id(std::string_view hex32, std::array<std::uint8_t, 16>& out) {
    if (hex32.size() != 32) return false;
    std::array<std::uint8_t, 16> parsed{};
    for (int i = 0; i < 16; ++i) {
        const int high = hex_value(hex32[i * 2]);
        const int low = hex_value(hex32[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        parsed[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    out = parsed;
    return true;
}

std::string format_game_id(const std::array<std::uint8_t, 16>& id) {
    std::string text;
    char buffer[3];
    for (std::uint8_t byte : id) {
        std::snprintf(buffer, sizeof(buffer), "%02x", byte);
        text += buffer;
    }
    return text;
}

std::uint64_t hash_resources(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_directory(folder, error)) return 0;
    std::vector<std::pair<std::string, fs::path>> files;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        files.emplace_back(fs::relative(it->path(), folder, error).generic_string(), it->path());
    }
    std::sort(files.begin(), files.end());
    XXH64_state_t* state = XXH64_createState();
    XXH64_reset(state, 0);
    std::vector<char> chunk(64 * 1024);
    for (const auto& [relative, path] : files) {
        XXH64_update(state, relative.data(), relative.size());
        const char zero = 0;
        XXH64_update(state, &zero, 1);
        std::ifstream in(path, std::ios::binary);
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = in.gcount();
            if (got > 0) XXH64_update(state, chunk.data(), static_cast<std::size_t>(got));
        }
    }
    const std::uint64_t hash = XXH64_digest(state);
    XXH64_freeState(state);
    return hash;
}

void write_identity(BitWriter& writer, const GameIdentity& identity) {
    writer.bytes(identity.game_id.data(), identity.game_id.size());
    const std::size_t length = std::min<std::size_t>(identity.version.size(), 255);
    writer.bits(static_cast<std::uint32_t>(length), 8);
    writer.bytes(reinterpret_cast<const std::uint8_t*>(identity.version.data()), length);
    writer.bits(static_cast<std::uint32_t>(identity.resources_hash & 0xFFFFFFFFu), 32);
    writer.bits(static_cast<std::uint32_t>(identity.resources_hash >> 32), 32);
}

bool read_identity(BitReader& reader, GameIdentity& identity) {
    GameIdentity read;
    reader.bytes(read.game_id.data(), read.game_id.size());
    const std::uint32_t length = reader.bits(8);
    if (reader.failed() || reader.bytes_left() < length) return false;
    read.version.resize(length);
    reader.bytes(reinterpret_cast<std::uint8_t*>(read.version.data()), length);
    const std::uint64_t low = reader.bits(32);
    const std::uint64_t high = reader.bits(32);
    if (reader.failed()) return false;
    read.resources_hash = low | (high << 32);
    identity = std::move(read);
    return true;
}

}  // namespace network
```

If `common/xxhash.h` does not resolve, zstd's include directory is `${ZSTD_SOURCE_DIR}/lib`, PUBLIC on the `zstd` target; `anarchy_network` links `zstd` PRIVATE (Task 1), which carries it. If the linker cannot find `ZSTD_XXH64_createState`, the header was included without the namespace: add `target_compile_definitions(anarchy_network PRIVATE XXH_NAMESPACE=ZSTD_)` beside the link line.

- [ ] **Step 5: Build and run**

Run: build, then `./build/Release/sandbox.exe "[network]"`. Expected: N52 to N54 pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/network sandbox/network_identity_tests.cpp
git commit -m "Network: the game identity and the resources hash

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: Endpoint: the handshake, peers, and pumping

**Files:**
- Create: `src/network/Endpoint.hpp`, `src/network/Endpoint.cpp`, `sandbox/network_endpoint_tests.cpp`
- Modify: `CMakeLists.txt` (add the .cpp; add the test file)

**Interfaces:**
- Consumes: `ISocket`, `Address`, `LinkHub` (Task 3); `Conditioner`, `ConditionerSettings` (Task 4); `Connection`, `ConnectionStats` (Task 7); `GameIdentity`, `write_identity`, `read_identity` (Task 8); `PacketHeader`, `PacketType`, `write_header`, `read_header`, `encode_packet`, `decode_packet`, `kMaxPacketBytes` (Task 2).
- Produces:
  ```cpp
  namespace network {
  using PeerId = std::uint8_t;
  constexpr PeerId kNoPeer = 0xFF;
  enum class Role { Server, Client };
  enum class ConnectionState { Disconnected, Connecting, Connected };
  enum class DisconnectReason { None, Requested, Timeout, Denied, Socket };
  enum class DenyReason : std::uint8_t { None, Full, WrongProtocol, WrongGame, WrongVersion, WrongContent };
  struct EndpointConfig { Role role = Role::Server; std::uint8_t max_peers = 64; double bytes_per_second = 32000.0; double send_interval = 1.0 / 60.0; GameIdentity identity; };
  struct EndpointCallbacks {
      std::function<void(PeerId)> on_connected;
      std::function<void(PeerId, DisconnectReason, DenyReason)> on_disconnected;
      std::function<void(PeerId, const std::uint8_t*, std::size_t)> on_message;
      std::function<void(PeerId, std::uint16_t sequence, const std::uint8_t*, std::size_t)> on_payload;
      std::function<void(PeerId, std::uint16_t sequence, std::uint32_t tag)> on_delivered;
  };
  struct EndpointStats { std::uint64_t dropped_bad_protocol = 0, dropped_bad_session = 0, dropped_unknown_peer = 0, dropped_malformed = 0; };
  class Endpoint {
  public:
      Endpoint(std::unique_ptr<ISocket> socket, EndpointConfig config, EndpointCallbacks callbacks);
      ConditionerSettings& conditioner();
      bool connect(const Address& server, double now);          // client only; false while a connection exists
      void disconnect(PeerId peer, double now);
      void receive(double now);
      void send(double now);
      ConnectionState state(PeerId peer) const;
      std::uint16_t session(PeerId peer) const;                  // 0 unless Connected
      ConnectionStats stats(PeerId peer, double now) const;
      const EndpointStats& stats() const;
      bool send_message(PeerId peer, const std::uint8_t* bytes, std::size_t length);
      std::size_t payload_budget(PeerId peer, double now) const;
      std::optional<std::uint16_t> send_payload(PeerId peer, const std::uint8_t* bytes, std::size_t length, std::uint32_t tag);
      std::vector<PeerId> peers() const;                         // Connected ones
      const std::string& denied_version() const;                 // client: the server's version after a WrongVersion deny
      bool failed() const;                                       // the socket failed; every peer was dropped
  };
  }
  ```

Handshake bodies, after a header whose `session` is 0: `ConnectRequest` carries the client salt (two 32-bit halves, low first) and the identity (`write_identity`); `ConnectAccept` carries the client salt then the server salt; `ConnectDeny` carries the reason (8 bits) and the server's version (8-bit length, bytes). `Disconnect` is a header alone, with the session. The session is the low 16 bits of `client_salt XOR server_salt`.

- [ ] **Step 1: Write the failing tests**

`sandbox/network_endpoint_tests.cpp`:

```cpp
// A server and its clients: the handshake, identity checks, timeouts,
// disconnects, and a whole session over a bad link (spec sections 5 and 6).

#include "Bits.hpp"
#include "Endpoint.hpp"
#include "GameIdentity.hpp"
#include "Packet.hpp"
#include "Socket.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using Catch::Approx;
using network::Address;
using network::ConnectionState;
using network::DenyReason;
using network::DisconnectReason;
using network::Endpoint;
using network::EndpointCallbacks;
using network::EndpointConfig;
using network::GameIdentity;
using network::PeerId;
using network::Role;

namespace {

struct Log {
    std::vector<PeerId> connected;
    std::vector<std::tuple<PeerId, DisconnectReason, DenyReason>> disconnected;
    std::vector<std::pair<PeerId, std::vector<std::uint8_t>>> messages;
    std::vector<std::tuple<PeerId, std::uint16_t, std::vector<std::uint8_t>>> payloads;
    std::vector<std::tuple<PeerId, std::uint16_t, std::uint32_t>> delivered;
    EndpointCallbacks callbacks() {
        EndpointCallbacks c;
        c.on_connected = [this](PeerId p) { connected.push_back(p); };
        c.on_disconnected = [this](PeerId p, DisconnectReason r, DenyReason d) { disconnected.emplace_back(p, r, d); };
        c.on_message = [this](PeerId p, const std::uint8_t* b, std::size_t n) { messages.emplace_back(p, std::vector<std::uint8_t>(b, b + n)); };
        c.on_payload = [this](PeerId p, std::uint16_t s, const std::uint8_t* b, std::size_t n) {
            payloads.emplace_back(p, s, std::vector<std::uint8_t>(b, b + n));
        };
        c.on_delivered = [this](PeerId p, std::uint16_t s, std::uint32_t t) { delivered.emplace_back(p, s, t); };
        return c;
    }
};

GameIdentity identity(const char* id = "0123456789abcdef0123456789abcdef", const char* version = "1.0.0", std::uint64_t hash = 42) {
    GameIdentity identity;
    network::parse_game_id(id, identity.game_id);
    identity.version = version;
    identity.resources_hash = hash;
    return identity;
}

// Addresses on the hub: the server is port 1, clients are 100 + n.
const Address kServer = Address::loopback(1);
Address client_address(int n) { return Address::loopback(static_cast<std::uint16_t>(100 + n)); }

struct Net {
    network::LinkHub hub;
    Log server_log;
    std::unique_ptr<Endpoint> server;
    std::deque<Log> client_logs;   // a deque: callbacks hold each Log's address
    std::vector<std::unique_ptr<Endpoint>> clients;

    explicit Net(std::uint8_t max_peers = 4, GameIdentity server_identity = identity()) {
        EndpointConfig config;
        config.role = Role::Server;
        config.max_peers = max_peers;
        config.identity = server_identity;
        server = std::make_unique<Endpoint>(hub.open(kServer), config, server_log.callbacks());
    }
    Endpoint& add_client(GameIdentity client_identity = identity()) {
        client_logs.emplace_back();
        EndpointConfig config;
        config.role = Role::Client;
        config.identity = client_identity;
        clients.push_back(std::make_unique<Endpoint>(hub.open(client_address(static_cast<int>(clients.size()))), config,
                                                     client_logs.back().callbacks()));
        return *clients.back();
    }
    // Everyone receives, then everyone sends, as the engine ticks.
    void tick(double now) {
        server->receive(now);
        for (auto& c : clients) c->receive(now);
        server->send(now);
        for (auto& c : clients) c->send(now);
    }
    double run(double from, double seconds) {
        const int ticks = static_cast<int>(seconds * 60.0);
        for (int i = 0; i <= ticks; ++i) tick(from + i / 60.0);
        return from + ticks / 60.0;
    }
};

// Sends a raw packet from an address on the hub.
void raw_send(network::LinkHub& hub, const Address& from, const Address& to, network::BitWriter& writer, std::uint8_t* buffer) {
    auto socket = hub.open(from);
    const std::size_t length = network::encode_packet(buffer, writer.bytes_written(), network::kMaxPacketBytes);
    socket->send(to, buffer, length);
}

}  // namespace

TEST_CASE("N60 a client connects, both sides talk, and a second request from the same address with another salt is ignored", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    REQUIRE(client.connect(kServer, 0.0));
    REQUIRE_FALSE(client.connect(kServer, 0.0));
    REQUIRE(client.state(0) == ConnectionState::Connecting);
    net.run(0.0, 0.1);
    REQUIRE(net.server_log.connected == std::vector<PeerId>{0});
    REQUIRE(net.client_logs[0].connected == std::vector<PeerId>{0});
    REQUIRE(client.state(0) == ConnectionState::Connected);
    REQUIRE(net.server->state(0) == ConnectionState::Connected);
    REQUIRE(net.server->peers() == std::vector<PeerId>{0});
    REQUIRE(client.session(0) == net.server->session(0));
    const std::uint8_t hello[2] = {'h', 'i'};
    REQUIRE(client.send_message(0, hello, 2));
    REQUIRE(net.server->send_message(0, hello, 2));
    const std::uint8_t state[3] = {1, 2, 3};
    const auto sequence = client.send_payload(0, state, 3, 9);
    REQUIRE(sequence.has_value());
    net.run(0.2, 0.1);
    REQUIRE(net.server_log.messages.size() == 1);
    REQUIRE(net.client_logs[0].messages.size() == 1);
    REQUIRE(net.server_log.payloads.size() == 1);
    REQUIRE(std::get<1>(net.server_log.payloads[0]) == *sequence);
    REQUIRE(net.client_logs[0].delivered.size() == 1);
    REQUIRE(std::get<2>(net.client_logs[0].delivered[0]) == 9);
    // Review Focus 1: another salt from the live client's address.
    std::uint8_t buffer[network::kMaxPacketBytes];
    network::BitWriter writer(buffer, sizeof(buffer));
    network::PacketHeader header;
    header.type = network::PacketType::ConnectRequest;
    network::write_header(writer, header);
    writer.bits(123, 32);
    writer.bits(0, 32);
    network::write_identity(writer, identity());
    raw_send(net.hub, client_address(0), kServer, writer, buffer);
    net.run(0.4, 0.1);
    REQUIRE(net.server_log.connected.size() == 1);
    REQUIRE(net.server->stats().dropped_unknown_peer == 1);
    REQUIRE(net.server->peers() == std::vector<PeerId>{0});
    REQUIRE(client.send_message(0, hello, 2));
    net.run(0.6, 0.1);
    REQUIRE(net.server_log.messages.size() == 2);
}

TEST_CASE("N61 a packet with a session that ended is dropped and counted", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    client.connect(kServer, 0.0);
    net.run(0.0, 0.1);
    const std::uint16_t old_session = client.session(0);
    client.disconnect(0, 0.2);
    net.run(0.2, 0.1);
    REQUIRE(net.server->peers().empty());
    client.connect(kServer, 0.4);
    double now = net.run(0.4, 0.1);
    REQUIRE(net.server->peers() == std::vector<PeerId>{0});
    REQUIRE(client.session(0) != old_session);
    std::uint8_t buffer[network::kMaxPacketBytes];
    network::BitWriter writer(buffer, sizeof(buffer));
    network::PacketHeader header;
    header.type = network::PacketType::Payload;
    header.session = old_session;
    header.sequence = 500;
    network::write_header(writer, header);
    writer.boolean(false);
    raw_send(net.hub, client_address(0), kServer, writer, buffer);
    net.run(now, 0.05);
    REQUIRE(net.server->stats().dropped_bad_session == 1);
    REQUIRE(net.server->peers() == std::vector<PeerId>{0});
}

TEST_CASE("N62 silence times a peer out after 5 s, and a clock that jumps a minute times everyone out cleanly", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    client.connect(kServer, 0.0);
    net.run(0.0, 0.1);
    // The client stops pumping.
    for (int i = 0; i < 60 * 6; ++i) {
        net.server->receive(0.2 + i / 60.0);
        net.server->send(0.2 + i / 60.0);
    }
    REQUIRE(net.server_log.disconnected.size() == 1);
    REQUIRE(std::get<1>(net.server_log.disconnected[0]) == DisconnectReason::Timeout);
    REQUIRE(net.server->peers().empty());
    // Review Focus 3: a jump forward.
    Net jump;
    Endpoint& c2 = jump.add_client();
    c2.connect(kServer, 0.0);
    jump.run(0.0, 0.1);
    jump.tick(60.0);
    jump.tick(60.02);
    REQUIRE(jump.server_log.disconnected.size() == 1);
    REQUIRE(jump.client_logs[0].disconnected.size() == 1);
    REQUIRE(c2.connect(kServer, 60.1));
    jump.run(60.1, 0.1);
    REQUIRE(jump.server_log.connected.size() == 2);
}

TEST_CASE("N63 the server denies another game, another version, other content, a full server, and drops another protocol", "[network][endpoint]") {
    Net net(1);
    Endpoint& wrong_game = net.add_client(identity("ffffffffffffffffffffffffffffffff"));
    wrong_game.connect(kServer, 0.0);
    net.run(0.0, 0.1);
    REQUIRE(net.client_logs[0].disconnected.size() == 1);
    REQUIRE(std::get<1>(net.client_logs[0].disconnected[0]) == DisconnectReason::Denied);
    REQUIRE(std::get<2>(net.client_logs[0].disconnected[0]) == DenyReason::WrongGame);
    Endpoint& wrong_version = net.add_client(identity("0123456789abcdef0123456789abcdef", "0.9.0"));
    wrong_version.connect(kServer, 0.2);
    net.run(0.2, 0.1);
    REQUIRE(std::get<2>(net.client_logs[1].disconnected[0]) == DenyReason::WrongVersion);
    REQUIRE(wrong_version.denied_version() == "1.0.0");
    Endpoint& wrong_content = net.add_client(identity("0123456789abcdef0123456789abcdef", "1.0.0", 43));
    wrong_content.connect(kServer, 0.4);
    net.run(0.4, 0.1);
    REQUIRE(std::get<2>(net.client_logs[2].disconnected[0]) == DenyReason::WrongContent);
    Endpoint& first = net.add_client();
    first.connect(kServer, 0.6);
    net.run(0.6, 0.1);
    REQUIRE(first.state(0) == ConnectionState::Connected);
    Endpoint& second = net.add_client();
    second.connect(kServer, 0.8);
    net.run(0.8, 0.1);
    REQUIRE(std::get<2>(net.client_logs[4].disconnected[0]) == DenyReason::Full);
    REQUIRE(net.server->peers().size() == 1);
    // Review Focus 4: another protocol id.
    std::uint8_t buffer[network::kMaxPacketBytes];
    network::BitWriter writer(buffer, sizeof(buffer));
    network::PacketHeader header;
    header.protocol = 0x1234;
    network::write_header(writer, header);
    raw_send(net.hub, client_address(7), kServer, writer, buffer);
    net.run(1.0, 0.05);
    REQUIRE(net.server->stats().dropped_bad_protocol == 1);
}

TEST_CASE("N64 a connect that nobody answers times out after 5 s", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    client.connect(Address::loopback(9), 0.0);
    for (int i = 0; i < 60 * 6; ++i) {
        client.receive(i / 60.0);
        client.send(i / 60.0);
    }
    REQUIRE(net.client_logs[0].disconnected.size() == 1);
    REQUIRE(std::get<1>(net.client_logs[0].disconnected[0]) == DisconnectReason::Timeout);
    REQUIRE(client.state(0) == ConnectionState::Disconnected);
}

TEST_CASE("N65 either side can disconnect and the other learns why", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    client.connect(kServer, 0.0);
    net.run(0.0, 0.1);
    client.disconnect(0, 0.2);
    REQUIRE(std::get<1>(net.client_logs[0].disconnected[0]) == DisconnectReason::Requested);
    net.run(0.2, 0.1);
    REQUIRE(std::get<1>(net.server_log.disconnected[0]) == DisconnectReason::Requested);
    client.connect(kServer, 0.4);
    net.run(0.4, 0.1);
    net.server->disconnect(0, 0.6);
    net.run(0.6, 0.1);
    REQUIRE(net.client_logs[0].disconnected.size() == 2);
    REQUIRE(std::get<1>(net.client_logs[0].disconnected[1]) == DisconnectReason::Requested);
    REQUIRE(client.state(0) == ConnectionState::Disconnected);
}

TEST_CASE("N66 a session over 150 ms, 20 ms jitter, and 2% loss each way delivers every message in order", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    for (Endpoint* e : {net.server.get(), &client}) {
        e->conditioner().latency_seconds = 0.150;
        e->conditioner().jitter_seconds = 0.020;
        e->conditioner().loss = 0.02f;
    }
    client.connect(kServer, 0.0);
    double now = net.run(0.0, 1.0);
    REQUIRE(client.state(0) == ConnectionState::Connected);
    int sent = 0;
    int delivered_reports = 0;
    for (int tick = 0; tick < 60 * 10; ++tick) {
        now += 1.0 / 60.0;
        if (sent < 300 && tick % 2 == 0) {
            const std::uint8_t byte = static_cast<std::uint8_t>(sent);
            if (client.send_message(0, &byte, 1)) ++sent;
        }
        const std::uint8_t state[4] = {1, 2, 3, 4};
        client.send_payload(0, state, 4, static_cast<std::uint32_t>(tick));
        net.tick(now);
    }
    REQUIRE(net.server_log.messages.size() == 300);
    for (int i = 0; i < 300; ++i) REQUIRE(net.server_log.messages[i].second[0] == static_cast<std::uint8_t>(i));
    delivered_reports = static_cast<int>(net.client_logs[0].delivered.size());
    REQUIRE(delivered_reports > 500);
    REQUIRE(client.stats(0, now).rtt == Approx(0.300).margin(0.06));
    REQUIRE(net.server->stats(0, now).loss < 0.1f);
}

TEST_CASE("N67 a stranger's payload is dropped and counted", "[network][endpoint]") {
    Net net;
    std::uint8_t buffer[network::kMaxPacketBytes];
    network::BitWriter writer(buffer, sizeof(buffer));
    network::PacketHeader header;
    network::write_header(writer, header);
    writer.boolean(false);
    raw_send(net.hub, client_address(3), kServer, writer, buffer);
    net.run(0.0, 0.05);
    REQUIRE(net.server->stats().dropped_unknown_peer == 1);
}

TEST_CASE("N68 a lost accept is answered again on the client's next request, with one connected event", "[network][endpoint]") {
    Net net;
    Endpoint& client = net.add_client();
    net.server->conditioner().loss = 1.0f;
    client.connect(kServer, 0.0);
    net.run(0.0, 0.3);
    REQUIRE(net.server_log.connected.size() == 1);
    REQUIRE(client.state(0) == ConnectionState::Connecting);
    net.server->conditioner().loss = 0.0f;
    net.run(0.3, 0.3);
    REQUIRE(client.state(0) == ConnectionState::Connected);
    REQUIRE(net.server_log.connected.size() == 1);
    REQUIRE(net.client_logs[0].connected.size() == 1);
}

namespace {

// A socket that stops working after a number of sends.
class FailingSocket : public network::ISocket {
public:
    FailingSocket(std::unique_ptr<network::ISocket> inner, int sends_before_failure) : inner_(std::move(inner)), left_(sends_before_failure) {}
    bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) override {
        if (left_-- <= 0) {
            failed_ = true;
            return false;
        }
        return inner_->send(to, bytes, length);
    }
    std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) override {
        return failed_ ? 0 : inner_->receive(from, buffer, capacity);
    }
    bool failed() const override { return failed_; }

private:
    std::unique_ptr<network::ISocket> inner_;
    int left_;
    bool failed_ = false;
};

}  // namespace

TEST_CASE("N69 a socket that fails drops every peer with reason Socket", "[network][endpoint]") {
    network::LinkHub hub;
    Log server_log;
    Log client_log;
    EndpointConfig server_config;
    server_config.identity = identity();
    Endpoint server(std::make_unique<FailingSocket>(hub.open(kServer), 20), server_config, server_log.callbacks());
    EndpointConfig client_config;
    client_config.role = Role::Client;
    client_config.identity = identity();
    Endpoint client(hub.open(client_address(0)), client_config, client_log.callbacks());
    client.connect(kServer, 0.0);
    for (int i = 0; i < 60 * 3; ++i) {
        const double now = i / 60.0;
        server.receive(now);
        client.receive(now);
        server.send(now);
        client.send(now);
    }
    REQUIRE(server.failed());
    REQUIRE(server_log.disconnected.size() == 1);
    REQUIRE(std::get<1>(server_log.disconnected[0]) == DisconnectReason::Socket);
    REQUIRE(server.peers().empty());
}
```

Add the test file to the sandbox list and `src/network/Endpoint.cpp` to `anarchy_network`.

- [ ] **Step 2: Build to see the failure**

Run: build. Expected: `Endpoint.hpp` cannot be opened.

- [ ] **Step 3: Write `Endpoint.hpp`**

```cpp
#pragma once

#include "Conditioner.hpp"
#include "Connection.hpp"
#include "GameIdentity.hpp"
#include "Packet.hpp"
#include "Socket.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace network {

// A peer's slot. A server has up to max_peers; a client has one, 0.
using PeerId = std::uint8_t;
constexpr PeerId kNoPeer = 0xFF;

enum class Role { Server, Client };
enum class ConnectionState { Disconnected, Connecting, Connected };
enum class DisconnectReason { None, Requested, Timeout, Denied, Socket };
enum class DenyReason : std::uint8_t { None, Full, WrongProtocol, WrongGame, WrongVersion, WrongContent };

struct EndpointConfig {
    Role role = Role::Server;
    std::uint8_t max_peers = 64;
    double bytes_per_second = 32000.0;   // 256 kbps
    double send_interval = 1.0 / 60.0;
    GameIdentity identity;
};

// Set once; the engine's tree may be touched from inside them, since the
// pump runs under its lock.
struct EndpointCallbacks {
    std::function<void(PeerId)> on_connected;
    std::function<void(PeerId, DisconnectReason, DenyReason)> on_disconnected;
    std::function<void(PeerId, const std::uint8_t*, std::size_t)> on_message;
    std::function<void(PeerId, std::uint16_t sequence, const std::uint8_t*, std::size_t)> on_payload;
    std::function<void(PeerId, std::uint16_t sequence, std::uint32_t tag)> on_delivered;
};

// Packets dropped before they reached a connection.
struct EndpointStats {
    std::uint64_t dropped_bad_protocol = 0;
    std::uint64_t dropped_bad_session = 0;
    std::uint64_t dropped_unknown_peer = 0;
    std::uint64_t dropped_malformed = 0;
};

// A server or a client: owns the socket, makes and ends connections, and
// pumps them. receive() at the start of an engine tick, send() at its end.
class Endpoint {
public:
    Endpoint(std::unique_ptr<ISocket> socket, EndpointConfig config, EndpointCallbacks callbacks);

    ConditionerSettings& conditioner() { return conditioner_.settings; }

    bool connect(const Address& server, double now);
    void disconnect(PeerId peer, double now);
    void receive(double now);
    void send(double now);

    ConnectionState state(PeerId peer) const;
    std::uint16_t session(PeerId peer) const;
    ConnectionStats stats(PeerId peer, double now) const;
    const EndpointStats& stats() const { return stats_; }
    bool send_message(PeerId peer, const std::uint8_t* bytes, std::size_t length);
    std::size_t payload_budget(PeerId peer, double now) const;
    std::optional<std::uint16_t> send_payload(PeerId peer, const std::uint8_t* bytes, std::size_t length, std::uint32_t tag);
    std::vector<PeerId> peers() const;
    const std::string& denied_version() const { return denied_version_; }
    bool failed() const { return failed_; }

private:
    struct Peer {
        bool used = false;
        Address address;
        std::uint64_t client_salt = 0;
        std::uint64_t server_salt = 0;
        ConnectionState state = ConnectionState::Disconnected;
        double state_time = 0.0;
        double last_request = -1.0;
        std::unique_ptr<Connection> connection;
    };

    Peer* find(const Address& address);
    const Peer* connected(PeerId peer) const;
    void receive_as_server(const Address& from, const PacketHeader& header, BitReader& reader, std::size_t length, double now);
    void receive_as_client(const Address& from, const PacketHeader& header, BitReader& reader, std::size_t length, double now);
    void open_connection(PeerId id, double now);
    void drop_peer(PeerId id, DisconnectReason reason, DenyReason deny);
    void fail_all();
    void send_request(Peer& peer);
    void send_accept(const Peer& peer);
    void send_deny(const Address& to, DenyReason reason);
    void send_disconnect(const Peer& peer);
    static std::uint16_t session_of(const Peer& peer) {
        return static_cast<std::uint16_t>((peer.client_salt ^ peer.server_salt) & 0xFFFF);
    }

    Conditioner conditioner_;
    EndpointConfig config_;
    EndpointCallbacks callbacks_;
    std::vector<Peer> peers_;
    Address server_address_;
    std::mt19937_64 random_;
    EndpointStats stats_;
    std::string denied_version_;
    bool failed_ = false;
    std::uint8_t buffer_[kMaxPacketBytes];
};

}  // namespace network
```

- [ ] **Step 4: Write `Endpoint.cpp`**

```cpp
#include "Endpoint.hpp"

#include <algorithm>
#include <chrono>

namespace network {

namespace {

constexpr double kRequestInterval = 0.1;
constexpr double kTimeout = 5.0;
constexpr int kDisconnectSends = 10;

void write_u64(BitWriter& writer, std::uint64_t value) {
    writer.bits(static_cast<std::uint32_t>(value & 0xFFFFFFFFu), 32);
    writer.bits(static_cast<std::uint32_t>(value >> 32), 32);
}

std::uint64_t read_u64(BitReader& reader) {
    const std::uint64_t low = reader.bits(32);
    const std::uint64_t high = reader.bits(32);
    return low | (high << 32);
}

}  // namespace

Endpoint::Endpoint(std::unique_ptr<ISocket> socket, EndpointConfig config, EndpointCallbacks callbacks)
    : conditioner_(std::move(socket)), config_(std::move(config)), callbacks_(std::move(callbacks)),
      peers_(config_.role == Role::Server ? config_.max_peers : 1),
      random_(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
              reinterpret_cast<std::uintptr_t>(this)) {}

Endpoint::Peer* Endpoint::find(const Address& address) {
    for (Peer& peer : peers_) {
        if (peer.used && peer.address == address) return &peer;
    }
    return nullptr;
}

const Endpoint::Peer* Endpoint::connected(PeerId id) const {
    if (id >= peers_.size()) return nullptr;
    const Peer& peer = peers_[id];
    return peer.used && peer.state == ConnectionState::Connected ? &peer : nullptr;
}

bool Endpoint::connect(const Address& server, double now) {
    if (config_.role != Role::Client || failed_ || peers_[0].used) return false;
    Peer& peer = peers_[0];
    peer.used = true;
    peer.address = server;
    peer.client_salt = random_();
    peer.server_salt = 0;
    peer.state = ConnectionState::Connecting;
    peer.state_time = now;
    peer.last_request = -1.0;
    peer.connection.reset();
    server_address_ = server;
    denied_version_.clear();
    return true;
}

void Endpoint::disconnect(PeerId id, double now) {
    (void)now;
    if (id >= peers_.size() || !peers_[id].used) return;
    if (peers_[id].state == ConnectionState::Connected) send_disconnect(peers_[id]);
    drop_peer(id, DisconnectReason::Requested, DenyReason::None);
}

void Endpoint::receive(double now) {
    if (failed_) return;
    conditioner_.advance(now);
    Address from;
    for (;;) {
        std::size_t length = conditioner_.receive(from, buffer_, kMaxPacketBytes);
        if (length == 0) break;
        length = decode_packet(buffer_, length);
        if (length == 0) continue;
        BitReader reader(buffer_, length);
        PacketHeader header;
        if (!read_header(reader, header)) {
            ++stats_.dropped_bad_protocol;
            continue;
        }
        if (config_.role == Role::Server) {
            receive_as_server(from, header, reader, length, now);
        } else {
            receive_as_client(from, header, reader, length, now);
        }
    }
    if (conditioner_.failed()) {
        fail_all();
        return;
    }
    // Silence. A connected peer's last sign of life is its last packet, or
    // the accept that made it, whichever is later.
    for (PeerId id = 0; id < peers_.size(); ++id) {
        Peer& peer = peers_[id];
        if (!peer.used) continue;
        double last = peer.state_time;
        if (peer.state == ConnectionState::Connected && peer.connection->last_receive() > last) last = peer.connection->last_receive();
        if (now - last > kTimeout) drop_peer(id, DisconnectReason::Timeout, DenyReason::None);
    }
}

void Endpoint::receive_as_server(const Address& from, const PacketHeader& header, BitReader& reader, std::size_t length, double now) {
    Peer* peer = find(from);
    switch (header.type) {
        case PacketType::ConnectRequest: {
            const std::uint64_t salt = read_u64(reader);
            GameIdentity identity;
            if (!read_identity(reader, identity)) {
                ++stats_.dropped_malformed;
                return;
            }
            if (peer != nullptr) {
                // The same client did not get our accept: answer again. A
                // different salt from a live address is someone else, or a
                // restart; it waits until this peer is gone.
                if (peer->client_salt == salt) send_accept(*peer);
                else ++stats_.dropped_unknown_peer;
                return;
            }
            DenyReason deny = DenyReason::None;
            if (identity.game_id != config_.identity.game_id) deny = DenyReason::WrongGame;
            else if (identity.version != config_.identity.version) deny = DenyReason::WrongVersion;
            else if (identity.resources_hash != config_.identity.resources_hash) deny = DenyReason::WrongContent;
            PeerId slot = kNoPeer;
            if (deny == DenyReason::None) {
                for (PeerId id = 0; id < peers_.size(); ++id) {
                    if (!peers_[id].used) {
                        slot = id;
                        break;
                    }
                }
                if (slot == kNoPeer) deny = DenyReason::Full;
            }
            if (deny != DenyReason::None) {
                send_deny(from, deny);
                return;
            }
            Peer& made = peers_[slot];
            made.used = true;
            made.address = from;
            made.client_salt = salt;
            made.server_salt = random_();
            made.last_request = -1.0;
            open_connection(slot, now);
            send_accept(made);
            if (callbacks_.on_connected) callbacks_.on_connected(slot);
            return;
        }
        case PacketType::ConnectAccept:
        case PacketType::ConnectDeny:
            return;
        default:
            break;
    }
    if (peer == nullptr || peer->state != ConnectionState::Connected) {
        ++stats_.dropped_unknown_peer;
        return;
    }
    if (header.session != peer->connection->session()) {
        ++stats_.dropped_bad_session;
        return;
    }
    const PeerId id = static_cast<PeerId>(peer - peers_.data());
    if (header.type == PacketType::Disconnect) {
        drop_peer(id, DisconnectReason::Requested, DenyReason::None);
        return;
    }
    peer->connection->read_packet(header, reader, buffer_, length, now);
}

void Endpoint::receive_as_client(const Address& from, const PacketHeader& header, BitReader& reader, std::size_t length, double now) {
    Peer& peer = peers_[0];
    if (!peer.used || from != server_address_) {
        ++stats_.dropped_unknown_peer;
        return;
    }
    switch (header.type) {
        case PacketType::ConnectAccept: {
            if (peer.state != ConnectionState::Connecting) return;
            const std::uint64_t client_salt = read_u64(reader);
            const std::uint64_t server_salt = read_u64(reader);
            if (reader.failed() || client_salt != peer.client_salt) {
                ++stats_.dropped_malformed;
                return;
            }
            peer.server_salt = server_salt;
            open_connection(0, now);
            if (callbacks_.on_connected) callbacks_.on_connected(0);
            return;
        }
        case PacketType::ConnectDeny: {
            if (peer.state != ConnectionState::Connecting) return;
            const std::uint32_t reason = reader.bits(8);
            const std::uint32_t version_length = reader.bits(8);
            if (reader.failed() || reason > static_cast<std::uint32_t>(DenyReason::WrongContent) || reader.bytes_left() < version_length) {
                ++stats_.dropped_malformed;
                return;
            }
            denied_version_.resize(version_length);
            reader.bytes(reinterpret_cast<std::uint8_t*>(denied_version_.data()), version_length);
            drop_peer(0, DisconnectReason::Denied, static_cast<DenyReason>(reason));
            return;
        }
        case PacketType::ConnectRequest:
            return;
        default:
            break;
    }
    if (peer.state != ConnectionState::Connected) {
        ++stats_.dropped_unknown_peer;
        return;
    }
    if (header.session != peer.connection->session()) {
        ++stats_.dropped_bad_session;
        return;
    }
    if (header.type == PacketType::Disconnect) {
        drop_peer(0, DisconnectReason::Requested, DenyReason::None);
        return;
    }
    peer.connection->read_packet(header, reader, buffer_, length, now);
}

void Endpoint::open_connection(PeerId id, double now) {
    Peer& peer = peers_[id];
    peer.state = ConnectionState::Connected;
    peer.state_time = now;
    peer.connection = std::make_unique<Connection>(config_.bytes_per_second, config_.send_interval);
    peer.connection->set_session(session_of(peer));
    Connection& connection = *peer.connection;
    connection.callbacks.on_message = [this, id](const std::uint8_t* bytes, std::size_t length) {
        if (callbacks_.on_message) callbacks_.on_message(id, bytes, length);
    };
    connection.callbacks.on_payload = [this, id](std::uint16_t sequence, const std::uint8_t* bytes, std::size_t length) {
        if (callbacks_.on_payload) callbacks_.on_payload(id, sequence, bytes, length);
    };
    connection.callbacks.on_delivered = [this, id](std::uint16_t sequence, std::uint32_t tag) {
        if (callbacks_.on_delivered) callbacks_.on_delivered(id, sequence, tag);
    };
}

void Endpoint::drop_peer(PeerId id, DisconnectReason reason, DenyReason deny) {
    Peer& peer = peers_[id];
    if (!peer.used) return;
    peer.used = false;
    peer.state = ConnectionState::Disconnected;
    peer.connection.reset();
    if (callbacks_.on_disconnected) callbacks_.on_disconnected(id, reason, deny);
}

void Endpoint::fail_all() {
    failed_ = true;
    for (PeerId id = 0; id < peers_.size(); ++id) drop_peer(id, DisconnectReason::Socket, DenyReason::None);
}

void Endpoint::send(double now) {
    if (failed_) return;
    conditioner_.advance(now);
    for (PeerId id = 0; id < peers_.size(); ++id) {
        Peer& peer = peers_[id];
        if (!peer.used) continue;
        if (peer.state == ConnectionState::Connecting) {
            if (config_.role == Role::Client && (peer.last_request < 0.0 || now - peer.last_request >= kRequestInterval)) {
                send_request(peer);
                peer.last_request = now;
            }
            continue;
        }
        if (peer.state == ConnectionState::Connected && peer.connection->wants_packet(now)) {
            const std::size_t length = peer.connection->write_packet(buffer_, kMaxPacketBytes, now);
            conditioner_.send(peer.address, buffer_, length);
        }
    }
    if (conditioner_.failed()) fail_all();
}

void Endpoint::send_request(Peer& peer) {
    BitWriter writer(buffer_, kMaxPacketBytes);
    PacketHeader header;
    header.type = PacketType::ConnectRequest;
    write_header(writer, header);
    write_u64(writer, peer.client_salt);
    write_identity(writer, config_.identity);
    conditioner_.send(peer.address, buffer_, encode_packet(buffer_, writer.bytes_written(), kMaxPacketBytes));
}

void Endpoint::send_accept(const Peer& peer) {
    BitWriter writer(buffer_, kMaxPacketBytes);
    PacketHeader header;
    header.type = PacketType::ConnectAccept;
    write_header(writer, header);
    write_u64(writer, peer.client_salt);
    write_u64(writer, peer.server_salt);
    conditioner_.send(peer.address, buffer_, encode_packet(buffer_, writer.bytes_written(), kMaxPacketBytes));
}

void Endpoint::send_deny(const Address& to, DenyReason reason) {
    BitWriter writer(buffer_, kMaxPacketBytes);
    PacketHeader header;
    header.type = PacketType::ConnectDeny;
    write_header(writer, header);
    writer.bits(static_cast<std::uint32_t>(reason), 8);
    const std::size_t length = std::min<std::size_t>(config_.identity.version.size(), 255);
    writer.bits(static_cast<std::uint32_t>(length), 8);
    writer.bytes(reinterpret_cast<const std::uint8_t*>(config_.identity.version.data()), length);
    conditioner_.send(to, buffer_, encode_packet(buffer_, writer.bytes_written(), kMaxPacketBytes));
}

void Endpoint::send_disconnect(const Peer& peer) {
    BitWriter writer(buffer_, kMaxPacketBytes);
    PacketHeader header;
    header.type = PacketType::Disconnect;
    header.session = peer.connection->session();
    write_header(writer, header);
    const std::size_t length = encode_packet(buffer_, writer.bytes_written(), kMaxPacketBytes);
    for (int i = 0; i < kDisconnectSends; ++i) conditioner_.send(peer.address, buffer_, length);
}

ConnectionState Endpoint::state(PeerId id) const {
    if (id >= peers_.size() || !peers_[id].used) return ConnectionState::Disconnected;
    return peers_[id].state;
}

std::uint16_t Endpoint::session(PeerId id) const {
    const Peer* peer = connected(id);
    return peer != nullptr ? peer->connection->session() : 0;
}

ConnectionStats Endpoint::stats(PeerId id, double now) const {
    const Peer* peer = connected(id);
    return peer != nullptr ? peer->connection->stats(now) : ConnectionStats{};
}

bool Endpoint::send_message(PeerId id, const std::uint8_t* bytes, std::size_t length) {
    const Peer* peer = connected(id);
    return peer != nullptr && peer->connection->send_message(bytes, length);
}

std::size_t Endpoint::payload_budget(PeerId id, double now) const {
    const Peer* peer = connected(id);
    return peer != nullptr ? peer->connection->payload_budget(now) : 0;
}

std::optional<std::uint16_t> Endpoint::send_payload(PeerId id, const std::uint8_t* bytes, std::size_t length, std::uint32_t tag) {
    const Peer* peer = connected(id);
    return peer != nullptr ? peer->connection->set_payload(bytes, length, tag) : std::nullopt;
}

std::vector<PeerId> Endpoint::peers() const {
    std::vector<PeerId> ids;
    for (PeerId id = 0; id < peers_.size(); ++id) {
        if (connected(id) != nullptr) ids.push_back(id);
    }
    return ids;
}

}  // namespace network
```

Note on N62's jump: at 60.0 the server sees its peer silent since about 0.2 and drops it; the client's `Disconnect` from the server was never sent (timeouts send nothing), so the client drops its side on its own timeout at the same tick. Both logs get one disconnect.

Note on the Disconnect packet's acks: its header's `ack` is 0xFFFF and `ack_bits` 0 (the defaults), so the receiver's ring processes nothing from it; it is dropped before `read_packet` anyway.

- [ ] **Step 5: Build and run**

Run: build, then `./build/Release/sandbox.exe "[network]"`. Expected: N60 to N69 pass. N66 takes about a second.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/network sandbox/network_endpoint_tests.cpp
git commit -m "Network: Endpoint connects, checks the game, pumps peers, and times them out

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 10: `project.json` gains `gameId` and `version`

**Files:**
- Modify: `src/engine_core/Project.hpp` (public accessors after `name()`, about line 160; members after `std::string name_;`, about line 293), `src/engine_core/Project.cpp` (`project_document` and `read_project_json`, about lines 335 to 393; `write_skeleton`, about line 1383; `read_into_game`, about line 1460; the other `read_project_json` callers, about lines 1533, 2183, 2995)
- Create: `sandbox/project_identity_tests.cpp`
- Modify: `CMakeLists.txt` (add the test file to the sandbox list)

**Interfaces:**
- Produces:
  ```cpp
  namespace engine_core {
  class Project {
      const std::string& game_id() const;   // 32 lowercase hex characters, never empty after create or load
      const std::string& version() const;   // "1.0.0" unless the file says otherwise
  };
  }
  ```
  and in `project.json`: `"gameId": "<32 hex>"`, `"version": "1.0.0"`.

This task does not link `anarchy_network`; sub-project 2 turns these two strings and the resources folder into a `GameIdentity`.

- [ ] **Step 1: Write the failing tests**

`sandbox/project_identity_tests.cpp`:

```cpp
// project.json's gameId and version, which the network handshake checks
// (docs/superpowers/specs/2026-10-12-network-transport-design.md, section 5).

#include "support.hpp"

#include "Project.hpp"
#include "PropertyBag.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using engine_core::Project;

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

bool is_hex32(const std::string& text) {
    if (text.size() != 32) return false;
    for (char c : text) {
        if (!std::isxdigit(static_cast<unsigned char>(c)) || std::isupper(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

engine_core::JsonValue project_json(const fs::path& root) {
    engine_core::JsonValue doc;
    std::string error;
    REQUIRE(engine_core::parse_json(read_text(root / "project.json"), doc, error));
    return doc;
}

const char* kOldProjectJson =
    "{\"format\": 1, \"name\": \"Old\", \"engine\": \"engine_core\", \"tree\": {\"src\": \"src\"}, \"resources\": {\"root\": \"resources\"}}";

}  // namespace

TEST_CASE("PJ1 create writes a gameId and version 1.0.0", "[project][network]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    Project project = Project::create(root);
    REQUIRE(is_hex32(project.game_id()));
    REQUIRE(project.version() == "1.0.0");
    const engine_core::JsonValue doc = project_json(root);
    REQUIRE(doc.find("gameId")->as_string() == project.game_id());
    REQUIRE(doc.find("version")->as_string() == "1.0.0");
    // Two projects never share an id.
    Project other = Project::create(dir.path / "Other");
    REQUIRE(other.game_id() != project.game_id());
}

TEST_CASE("PJ2 loading a project without them adds them once and keeps them after", "[project][network]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    { Project made = Project::create(root); }
    write_text(root / "project.json", kOldProjectJson);
    std::string first;
    {
        Project loaded = Project::load(root);
        first = loaded.game_id();
        REQUIRE(is_hex32(first));
        REQUIRE(loaded.version() == "1.0.0");
        REQUIRE(loaded.name() == "Old");
    }
    const engine_core::JsonValue doc = project_json(root);
    REQUIRE(doc.find("gameId")->as_string() == first);
    REQUIRE(doc.find("version")->as_string() == "1.0.0");
    REQUIRE(doc.find("name")->as_string() == "Old");
    REQUIRE(doc.find("tree")->find("src")->as_string() == "src");
    Project again = Project::load(root);
    REQUIRE(again.game_id() == first);
}

TEST_CASE("PJ3 Save As keeps the gameId and version", "[project][network]") {
    SimRole role;
    TempDir dir;
    Project project = Project::create(dir.path / "Place");
    const std::string id = project.game_id();
    project.save_as(dir.path / "Copy");
    REQUIRE(project.game_id() == id);
    const engine_core::JsonValue doc = project_json(dir.path / "Copy");
    REQUIRE(doc.find("gameId")->as_string() == id);
    REQUIRE(doc.find("version")->as_string() == "1.0.0");
}

TEST_CASE("PJ4 a version in the file is read, and a gameId that is not 32 hex characters is replaced", "[project][network]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    { Project made = Project::create(root); }
    write_text(root / "project.json",
               "{\"format\": 1, \"name\": \"Old\", \"engine\": \"engine_core\", \"tree\": {\"src\": \"src\"}, "
               "\"resources\": {\"root\": \"resources\"}, \"gameId\": \"not-an-id\", \"version\": \"2.1.0\"}");
    Project loaded = Project::load(root);
    REQUIRE(loaded.version() == "2.1.0");
    REQUIRE(is_hex32(loaded.game_id()));
    const engine_core::JsonValue doc = project_json(root);
    REQUIRE(doc.find("gameId")->as_string() == loaded.game_id());
    REQUIRE(doc.find("version")->as_string() == "2.1.0");
}
```

Add `sandbox/project_identity_tests.cpp` to the sandbox list.

- [ ] **Step 2: Build to see the failure**

Run: build. Expected: `game_id` is not a member of `Project`.

- [ ] **Step 3: The header**

In `Project.hpp`, after the `name()` accessor, add:

```cpp
    // project.json's gameId: 32 hex characters that name this game to the
    // network handshake. Made at create, added at load to a project without
    // one, kept by Save As.
    const std::string& game_id() const { return game_id_; }
    // project.json's version, "1.0.0" by default. The user will set it.
    const std::string& version() const { return version_; }
```

After `std::string name_;`, add:

```cpp
    std::string game_id_;
    std::string version_ = "1.0.0";
```

Change `void write_skeleton(const std::filesystem::path& root) const;` to drop the `const`.

- [ ] **Step 4: Reading and writing the document**

In `Project.cpp`, replace `project_document` and `read_project_json` (the anonymous-namespace functions after `struct Layout`) with:

```cpp
bool well_formed_game_id(const std::string& text) {
    if (text.size() != 32) return false;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

JsonValue project_document(const std::string& name, const Layout& layout, const std::string& game_id,
                           const std::string& version) {
    JsonValue doc = JsonValue::object();
    doc.set("format", JsonValue::number(kFormat));
    doc.set("name", JsonValue::string(name));
    doc.set("engine", JsonValue::string(kEngine));
    doc.set("gameId", JsonValue::string(game_id));
    doc.set("version", JsonValue::string(version));
    JsonValue tree = JsonValue::object();
    tree.set("src", JsonValue::string(layout.src));
    doc.set("tree", std::move(tree));
    JsonValue resources = JsonValue::object();
    resources.set("root", JsonValue::string(layout.resources));
    doc.set("resources", std::move(resources));
    return doc;
}

// What project.json says. game_id and version are empty when the file
// lacks them or they are malformed; the load fills them in and writes back.
struct ProjectJson {
    std::string name;
    Layout layout;
    std::string game_id;
    std::string version;
};

ProjectJson read_project_json(const fs::path& root) {
    const fs::path path = root / "project.json";
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        fail(utf8_path(path) + " is missing");
    }
    JsonValue doc;
    std::string message;
    if (!parse_json(read_file(path), doc, message)) {
        fail(utf8_path(path) + ": " + message);
    }
    if (!doc.is_object()) {
        fail(utf8_path(path) + ": expected an object");
    }
    const JsonValue* format = doc.find("format");
    if (format == nullptr || !format->is_number() || format->as_number() != kFormat) {
        fail(utf8_path(path) + ": format must be " + std::to_string(kFormat));
    }
    ProjectJson json;
    const JsonValue* label = doc.find("name");
    json.name = label != nullptr && label->is_string() ? label->as_string() : project_name_for(root);
    if (const JsonValue* tree = doc.find("tree")) {
        const JsonValue* src = tree->find("src");
        if (src == nullptr || !src->is_string() || !safe_relative(src->as_string())) {
            fail(utf8_path(path) + ": tree.src must be a relative directory");
        }
        json.layout.src = src->as_string();
    }
    if (const JsonValue* resources = doc.find("resources")) {
        const JsonValue* dir = resources->find("root");
        if (dir == nullptr || !dir->is_string() || !safe_relative(dir->as_string())) {
            fail(utf8_path(path) + ": resources.root must be a relative directory");
        }
        json.layout.resources = dir->as_string();
    }
    if (const JsonValue* id = doc.find("gameId"); id != nullptr && id->is_string() && well_formed_game_id(id->as_string())) {
        json.game_id = id->as_string();
    }
    if (const JsonValue* version = doc.find("version"); version != nullptr && version->is_string() && !version->as_string().empty()) {
        json.version = version->as_string();
    }
    return json;
}
```

- [ ] **Step 5: The callers**

`write_skeleton` (no longer const):

```cpp
void Project::write_skeleton(const fs::path& root) {
    const Layout layout;
    if (game_id_.empty()) game_id_ = make_guid() + make_guid();
    write_file(root / "project.json", write_json(project_document(name_, layout, game_id_, version_)));
```

(the rest of the function unchanged). `make_guid` is declared in `DataModel.hpp`, which `Project.hpp` includes.

`read_into_game`, replacing `Layout layout; read_project_json(root, name_, layout);`:

```cpp
    ProjectJson json = read_project_json(root);
    name_ = json.name;
    const Layout layout = json.layout;
    game_id_ = json.game_id;
    version_ = json.version;
    // A project from before gameId and version gets them now, once, the way
    // ignore_texture_cache adds .cache/ to .gitignore.
    if (game_id_.empty() || version_.empty()) {
        if (game_id_.empty()) game_id_ = make_guid() + make_guid();
        if (version_.empty()) version_ = "1.0.0";
        write_file(root / "project.json", write_json(project_document(name_, layout, game_id_, version_)));
    }
```

The three other callers (`save_as`, and the two near lines 2183 and 2995) each read `Layout` with an ignored name:

```cpp
    Layout old_layout;
    std::string ignored;
    std::error_code error;
    if (fs::is_regular_file(root_ / "project.json", error)) {
        read_project_json(root_, ignored, old_layout);
    }
```

becomes

```cpp
    Layout old_layout;
    std::error_code error;
    if (fs::is_regular_file(root_ / "project.json", error)) {
        old_layout = read_project_json(root_).layout;
    }
```

and likewise for the other two: `layout = read_project_json(root_).layout;`, dropping their `ignored` string. Grep for `read_project_json(` to be sure none is missed.

- [ ] **Step 6: Build and run**

Run: build, then `./build/Release/sandbox.exe "[project]"`. Expected: PJ1 to PJ4 pass and every existing `[project]` test still passes (P8 checks the skeleton's keys and does not mind two more).

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt src/engine_core/Project.hpp src/engine_core/Project.cpp sandbox/project_identity_tests.cpp
git commit -m "Project: project.json carries a gameId and a version

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 11: Soak, allocations, and the benchmark

**Files:**
- Create: `sandbox/network_soak_tests.cpp`
- Modify: `CMakeLists.txt` (add the test file)

**Interfaces:**
- Consumes: `Endpoint` and everything under it (Task 9); `alloc_counter` (`sandbox/alloc_counter.hpp`).

- [ ] **Step 1: Write the tests**

`sandbox/network_soak_tests.cpp`:

```cpp
// The spec's soak test, the no-allocation rule, and the pumping benchmark
// (spec, Testing).

#include "alloc_counter.hpp"

#include "Endpoint.hpp"
#include "GameIdentity.hpp"
#include "Packet.hpp"
#include "Socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using network::Address;
using network::ConnectionState;
using network::Endpoint;
using network::EndpointCallbacks;
using network::EndpointConfig;
using network::GameIdentity;
using network::PeerId;
using network::Role;

namespace {

GameIdentity identity() {
    GameIdentity identity;
    network::parse_game_id("0123456789abcdef0123456789abcdef", identity.game_id);
    identity.resources_hash = 7;
    return identity;
}

EndpointConfig config(Role role, std::uint8_t max_peers = 64) {
    EndpointConfig config;
    config.role = role;
    config.max_peers = max_peers;
    config.identity = identity();
    return config;
}

const Address kServer = Address::loopback(1);
Address client_address(int n) { return Address::loopback(static_cast<std::uint16_t>(100 + n)); }

// Two sockets joined by fixed rings: no heap, for the allocation test.
struct StaticLink {
    struct Datagram {
        std::uint16_t length;
        std::uint8_t bytes[network::kMaxPacketBytes];
    };
    struct Ring {
        Datagram slots[64];
        int head = 0;
        int count = 0;
    };
    Ring to_a;
    Ring to_b;

    class Socket : public network::ISocket {
    public:
        Socket(Ring& out, Ring& in, Address own, Address peer) : out_(out), in_(in), own_(own), peer_(peer) {}
        bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) override {
            if (to != peer_ || out_.count == 64) return false;
            Datagram& slot = out_.slots[(out_.head + out_.count) % 64];
            slot.length = static_cast<std::uint16_t>(length);
            std::memcpy(slot.bytes, bytes, length);
            ++out_.count;
            return true;
        }
        std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) override {
            if (in_.count == 0) return 0;
            const Datagram& slot = in_.slots[in_.head];
            in_.head = (in_.head + 1) % 64;
            --in_.count;
            if (slot.length > capacity) return 0;
            std::memcpy(buffer, slot.bytes, slot.length);
            from = peer_;
            return slot.length;
        }
        bool failed() const override { return false; }

    private:
        Ring& out_;
        Ring& in_;
        Address own_;
        Address peer_;
    };

    std::unique_ptr<network::ISocket> a() { return std::make_unique<Socket>(to_b, to_a, kServer, client_address(0)); }
    std::unique_ptr<network::ISocket> b() { return std::make_unique<Socket>(to_a, to_b, client_address(0), kServer); }
};

}  // namespace

TEST_CASE("N70 10,000 messages cross a 200 ms, 30 ms jitter, 5% loss link in order, exactly once", "[network][bench]") {
    network::LinkHub hub;
    int received = 0;
    bool in_order = true;
    EndpointCallbacks server_callbacks;
    server_callbacks.on_message = [&](PeerId, const std::uint8_t* bytes, std::size_t length) {
        std::uint32_t index;
        std::memcpy(&index, bytes, 4);
        if (length != 4 + index % 500 || index != static_cast<std::uint32_t>(received)) in_order = false;
        ++received;
    };
    Endpoint server(hub.open(kServer), config(Role::Server), server_callbacks);
    Endpoint client(hub.open(client_address(0)), config(Role::Client), EndpointCallbacks{});
    for (Endpoint* e : {&server, &client}) {
        e->conditioner().latency_seconds = 0.200;
        e->conditioner().jitter_seconds = 0.030;
        e->conditioner().loss = 0.05f;
    }
    client.connect(kServer, 0.0);
    std::vector<std::uint8_t> message;
    std::uint32_t sent = 0;
    double now = 0.0;
    for (int tick = 0; tick < 60 * 600 && received < 10000; ++tick) {
        now = tick / 60.0;
        server.receive(now);
        client.receive(now);
        if (client.state(0) == ConnectionState::Connected) {
            for (int i = 0; i < 8 && sent < 10000; ++i) {
                message.assign(4 + sent % 500, static_cast<std::uint8_t>(sent));
                std::memcpy(message.data(), &sent, 4);
                if (!client.send_message(0, message.data(), message.size())) break;
                ++sent;
            }
        }
        server.send(now);
        client.send(now);
    }
    REQUIRE(received == 10000);
    REQUIRE(in_order);
    REQUIRE(server.stats(0, now).rtt > 0.35);
}

TEST_CASE("N71 pumping a connected pair allocates nothing", "[network][bench]") {
    StaticLink link;
    int payloads = 0;
    int messages = 0;
    EndpointCallbacks callbacks;
    callbacks.on_payload = [&](PeerId, std::uint16_t, const std::uint8_t*, std::size_t) { ++payloads; };
    callbacks.on_message = [&](PeerId, const std::uint8_t*, std::size_t) { ++messages; };
    Endpoint server(link.a(), config(Role::Server, 1), callbacks);
    Endpoint client(link.b(), config(Role::Client), callbacks);
    client.connect(kServer, 0.0);
    const std::uint8_t state[64] = {};
    const std::uint8_t note[8] = {};
    auto tick = [&](double now) {
        server.receive(now);
        client.receive(now);
        if (client.state(0) == ConnectionState::Connected) {
            client.send_payload(0, state, sizeof(state), 1);
            server.send_payload(0, state, sizeof(state), 2);
            if (static_cast<int>(now * 60.0) % 10 == 0) {
                client.send_message(0, note, sizeof(note));
                server.send_message(0, note, sizeof(note));
            }
        }
        server.send(now);
        client.send(now);
    };
    for (int i = 0; i < 60; ++i) tick(i / 60.0);
    REQUIRE(client.state(0) == ConnectionState::Connected);
    alloc_counter::arm();
    for (int i = 60; i < 180; ++i) tick(i / 60.0);
    const std::size_t allocations = alloc_counter::count();
    alloc_counter::disarm();
    REQUIRE(payloads > 200);
    REQUIRE(messages > 10);
    REQUIRE(allocations == 0);
}

TEST_CASE("N72 a server pumping 64 full connections at 60 Hz spends under 1 ms a tick", "[network][bench]") {
    network::LinkHub hub;
    Endpoint server(hub.open(kServer), config(Role::Server, 64), EndpointCallbacks{});
    std::vector<std::unique_ptr<Endpoint>> clients;
    for (int i = 0; i < 64; ++i) {
        clients.push_back(std::make_unique<Endpoint>(hub.open(client_address(i)), config(Role::Client), EndpointCallbacks{}));
        clients.back()->connect(kServer, 0.0);
    }
    std::vector<std::uint8_t> full(network::kMaxPayloadBytes - 1, 3);
    auto tick = [&](double now, bool full_packets) {
        for (auto& c : clients) c->receive(now);
        if (full_packets) {
            for (auto& c : clients) c->send_payload(0, full.data(), full.size(), 0);
        }
        for (auto& c : clients) c->send(now);
        const auto start = std::chrono::steady_clock::now();
        server.receive(now);
        if (full_packets) {
            for (PeerId p = 0; p < 64; ++p) server.send_payload(p, full.data(), full.size(), 0);
        }
        server.send(now);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    for (int i = 0; i < 60; ++i) tick(i / 60.0, false);
    REQUIRE(server.peers().size() == 64);
    for (int i = 60; i < 120; ++i) tick(i / 60.0, true);   // warm up
    double total = 0.0;
    for (int i = 120; i < 420; ++i) total += tick(i / 60.0, true);
    const double mean = total / 300.0;
    WARN("server tick with 64 full connections: " << mean << " ms");
#ifdef NDEBUG
    REQUIRE(mean < 1.0);
#endif
}
```

Add `sandbox/network_soak_tests.cpp` to the sandbox list.

- [ ] **Step 2: Build and run**

Run: build, then `./build/Release/sandbox.exe "[network]"`. Expected: N70 to N72 pass; N72 prints the mean. If N71 reports allocations, find them with a breakpoint on `alloc_counter`'s counting `operator new` (in `sandbox/alloc_counter.cpp`) while armed; the usual suspects are a `std::function` rebuilt per packet, a `std::vector` in a callback, or `Conditioner::hold` with a non-zero setting. If N72 misses 1 ms, profile the server's `receive`: `LinkHub::find` is linear over 65 slots and is test scaffolding, not the library; `Endpoint::find` over 64 peers per packet is the library's and may need an address-keyed map, which is in scope.

- [ ] **Step 3: Run the whole sandbox once**

Run: `./build/Release/sandbox.exe`. Expected: every test passes (the memory `release-test-flakes` lists the known Windows flakes to recognize).

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt sandbox/network_soak_tests.cpp
git commit -m "Network: soak, allocation, and pumping benchmark tests

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Done when

- `./build/Release/sandbox.exe "[network]"` and `"[project]"` pass in Release, and the full sandbox passes.
- `src/network/` has no `#include` of anything under `src/engine_core`.
- The spec's wire constants are the ones in `Packet.hpp`, `PacketRing.hpp`, and `ReliableChannel.hpp`.
- `docs/superpowers/specs/2026-10-12-networking-roadmap-design.md` still names this spec for sub-project 1; nothing else in it changes.
