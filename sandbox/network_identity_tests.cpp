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

TEST_CASE("N53b nested siblings and non-ASCII names hash stably; an empty folder is not a missing one", "[network][identity]") {
    TempDir a;
    write(a.path / "a" / "x.bin", "x");
    write(a.path / "a" / "b" / "y.bin", "y");
    write(a.path / "ab" / "z.bin", "z");
    TempDir b;
    write(b.path / "ab" / "z.bin", "z");
    write(b.path / "a" / "b" / "y.bin", "y");
    write(b.path / "a" / "x.bin", "x");
    REQUIRE(network::hash_resources(a.path) != 0);
    REQUIRE(network::hash_resources(a.path) == network::hash_resources(b.path));

    const std::string accented = "texture\xC3\xA9.png";
    TempDir c;
    write(c.path / fs::u8path(accented), "t");
    TempDir d;
    write(d.path / fs::u8path(accented), "t");
    TempDir e;
    write(e.path / "texture.png", "t");
    REQUIRE(network::hash_resources(c.path) != 0);
    REQUIRE(network::hash_resources(c.path) == network::hash_resources(d.path));
    REQUIRE(network::hash_resources(c.path) != network::hash_resources(e.path));

    TempDir empty;
    fs::create_directories(empty.path);
    REQUIRE(network::hash_resources(empty.path) != 0);
    REQUIRE(network::hash_resources(empty.path / "missing") == 0);
}

TEST_CASE("N85 the resources hash skips what a packed game leaves out: names starting with '.'", "[network][identity]") {
    TempDir plain;
    write(plain.path / "textures" / "a.png", "a");
    TempDir dotted;
    write(dotted.path / "textures" / "a.png", "a");
    write(dotted.path / "textures" / ".gitkeep", "");
    write(dotted.path / ".DS_Store", "finder");
    write(dotted.path / ".git" / "objects" / "x", "x");
    REQUIRE(network::hash_resources(dotted.path) != 0);
    REQUIRE(network::hash_resources(dotted.path) == network::hash_resources(plain.path));

    // A new project's resources hold only .gitkeep files: it hashes as an empty folder.
    TempDir kept;
    write(kept.path / "meshes" / ".gitkeep", "");
    TempDir empty;
    fs::create_directories(empty.path);
    REQUIRE(network::hash_resources(kept.path) == network::hash_resources(empty.path));
}
