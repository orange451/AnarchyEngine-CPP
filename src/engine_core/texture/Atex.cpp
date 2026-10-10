#include "texture/Atex.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

namespace engine_core::texture {

namespace {

constexpr char kMagic[4] = {'A', 'T', 'E', 'X'};

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
}

std::uint32_t get_u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}

std::uint64_t get_u64(const std::uint8_t* p) {
    return std::uint64_t(get_u32(p)) | (std::uint64_t(get_u32(p + 4)) << 32);
}

std::size_t header_bytes(std::size_t levels, std::size_t planes) {
    return 24 + (planes + 3) / 4 * 4 + levels * planes * 16;
}

int level_side(int side, int level) { return std::max(1, side >> level); }

bool known_format(std::uint8_t value) { return value <= std::uint8_t(PixelFormat::BC5); }

}  // namespace

bool write_atex(const std::filesystem::path& path, const BakedTexture& baked, std::string& error) {
    const std::size_t planes = baked.planes.size();
    const std::size_t levels = std::size_t(baked.level_count());
    if (planes == 0 || levels == 0 || baked.formats.size() != planes) {
        error = "nothing to write";
        return false;
    }
    for (const auto& plane : baked.planes) {
        if (plane.size() != levels) {
            error = "planes have different level counts";
            return false;
        }
    }

    std::vector<std::uint8_t> head;
    head.insert(head.end(), kMagic, kMagic + 4);
    put_u32(head, kAtexVersion);
    put_u32(head, std::uint32_t(baked.width));
    put_u32(head, std::uint32_t(baked.height));
    put_u32(head, std::uint32_t(levels));
    put_u32(head, std::uint32_t(planes));
    for (PixelFormat format : baked.formats) head.push_back(std::uint8_t(format));
    while (head.size() % 4 != 0) head.push_back(0);

    // Smallest level first: offsets grow as level falls.
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> ranges(
        levels, std::vector<std::pair<std::uint64_t, std::uint64_t>>(planes));
    std::uint64_t offset = header_bytes(levels, planes);
    for (std::size_t l = levels; l-- > 0;) {
        for (std::size_t p = 0; p < planes; ++p) {
            ranges[l][p] = {offset, baked.planes[p][l].size()};
            offset += baked.planes[p][l].size();
        }
    }
    for (std::size_t l = 0; l < levels; ++l) {
        for (std::size_t p = 0; p < planes; ++p) {
            put_u64(head, ranges[l][p].first);
            put_u64(head, ranges[l][p].second);
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + temporary.u8string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(head.data()), std::streamsize(head.size()));
        for (std::size_t l = levels; l-- > 0;) {
            for (std::size_t p = 0; p < planes; ++p) {
                const auto& bytes = baked.planes[p][l];
                out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            }
        }
        if (!out) {
            error = "cannot write " + temporary.u8string();
            out.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        error = "cannot write " + path.u8string() + ": " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

std::optional<AtexHeader> read_atex_header(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::error_code ec;
    const std::uint64_t file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size < 24) return std::nullopt;

    std::uint8_t fixed[24];
    if (!in.read(reinterpret_cast<char*>(fixed), 24)) return std::nullopt;
    if (std::memcmp(fixed, kMagic, 4) != 0 || get_u32(fixed + 4) != kAtexVersion) return std::nullopt;
    AtexHeader header;
    header.width = int(get_u32(fixed + 8));
    header.height = int(get_u32(fixed + 12));
    header.levels = int(get_u32(fixed + 16));
    const std::uint32_t planes = get_u32(fixed + 20);
    if (header.width <= 0 || header.height <= 0 || header.levels <= 0 || header.levels > 32 || planes == 0 ||
        planes > 8) {
        return std::nullopt;
    }
    const std::size_t total = header_bytes(std::size_t(header.levels), planes);
    if (file_size < total) return std::nullopt;
    std::vector<std::uint8_t> rest(total - 24);
    if (!in.read(reinterpret_cast<char*>(rest.data()), std::streamsize(rest.size()))) return std::nullopt;

    for (std::uint32_t p = 0; p < planes; ++p) {
        if (!known_format(rest[p])) return std::nullopt;
        header.formats.push_back(PixelFormat(rest[p]));
    }
    const std::uint8_t* entry = rest.data() + (planes + 3) / 4 * 4;
    header.ranges.resize(std::size_t(header.levels));
    for (int l = 0; l < header.levels; ++l) {
        for (std::uint32_t p = 0; p < planes; ++p, entry += 16) {
            const std::uint64_t offset = get_u64(entry), length = get_u64(entry + 8);
            const std::size_t expected = level_bytes(header.formats[p], level_side(header.width, l),
                                                     level_side(header.height, l));
            if (length != expected || offset < total || offset > file_size || length > file_size - offset) {
                return std::nullopt;
            }
            header.ranges[std::size_t(l)].push_back({offset, length});
        }
    }
    return header;
}

std::optional<std::vector<std::vector<std::uint8_t>>> read_atex_level(const std::filesystem::path& path,
                                                                       const AtexHeader& header, int level) {
    if (level < 0 || level >= header.levels) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<std::vector<std::uint8_t>> out;
    for (const auto& range : header.ranges[std::size_t(level)]) {
        std::vector<std::uint8_t> bytes(range.second);
        in.seekg(std::streamoff(range.first));
        if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return std::nullopt;
        out.push_back(std::move(bytes));
    }
    return out;
}

}  // namespace engine_core::texture
