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

TEST_CASE("PJ5 the migration keeps keys it does not know", "[project][network]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    { Project made = Project::create(root); }
    write_text(root / "project.json",
               "{\"format\": 1, \"name\": \"Old\", \"engine\": \"engine_core\", \"tree\": {\"src\": \"src\"}, "
               "\"resources\": {\"root\": \"resources\"}, \"custom\": {\"a\": 1}}");
    std::string id;
    {
        Project loaded = Project::load(root);
        id = loaded.game_id();
    }
    const engine_core::JsonValue doc = project_json(root);
    REQUIRE(doc.find("custom") != nullptr);
    REQUIRE(doc.find("custom")->find("a")->as_number() == 1);
    REQUIRE(doc.find("gameId")->as_string() == id);
    REQUIRE(doc.find("version")->as_string() == "1.0.0");
}

TEST_CASE("PJ6 an old project whose project.json is read-only still loads", "[project][network]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    { Project made = Project::create(root); }
    write_text(root / "project.json", kOldProjectJson);
    const fs::path file = root / "project.json";
    fs::permissions(file, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read, fs::perm_options::replace);
    std::string id;
    bool threw = false;
    try {
        Project loaded = Project::load(root);
        id = loaded.game_id();
    } catch (...) {
        threw = true;
    }
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    REQUIRE_FALSE(threw);
    REQUIRE(is_hex32(id));
}
