#include "EditorFont.hpp"

#include "IdeTheme.hpp"

#include "jadefx/scene/text/Font.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>

namespace ide {

namespace {

std::string& Choice() {
    static std::string choice;
    return choice;
}

bool ReadAt(std::ifstream& in, std::uint64_t offset, unsigned char* out, std::size_t size) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(in.gcount()) == size;
}

std::uint32_t U16(const unsigned char* at) { return (static_cast<std::uint32_t>(at[0]) << 8) | at[1]; }

std::uint32_t U32(const unsigned char* at) {
    return (static_cast<std::uint32_t>(at[0]) << 24) | (static_cast<std::uint32_t>(at[1]) << 16) |
           (static_cast<std::uint32_t>(at[2]) << 8) | at[3];
}

void AppendUtf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char unit) { return static_cast<char>(std::tolower(unit)); });
    return text;
}

bool ReadNames(const std::string& path, std::string& family, std::string& style) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) {
        return false;
    }
    unsigned char head[12];
    if (!ReadAt(in, 0, head, sizeof head)) {
        return false;
    }
    std::uint64_t base = 0;
    if (U32(head) == 0x74746366u) {
        unsigned char first[4];
        if (!ReadAt(in, 12, first, sizeof first)) {
            return false;
        }
        base = U32(first);
        if (!ReadAt(in, base, head, sizeof head)) {
            return false;
        }
    }
    const std::uint32_t tables = U16(head + 4);
    std::uint64_t name_offset = 0;
    for (std::uint32_t index = 0; index < tables && index < 512; ++index) {
        unsigned char record[16];
        if (!ReadAt(in, base + 12 + static_cast<std::uint64_t>(index) * 16, record, sizeof record)) {
            return false;
        }
        if (U32(record) == 0x6E616D65u) {
            name_offset = U32(record + 8);
            break;
        }
    }
    if (name_offset == 0) {
        return false;
    }
    unsigned char table[6];
    if (!ReadAt(in, name_offset, table, sizeof table)) {
        return false;
    }
    const std::uint32_t count = U16(table + 2);
    const std::uint64_t strings = name_offset + U16(table + 4);
    std::string found[4];
    int rank[4] = {-1, -1, -1, -1};
    for (std::uint32_t index = 0; index < count && index < 2048; ++index) {
        unsigned char record[12];
        if (!ReadAt(in, name_offset + 6 + static_cast<std::uint64_t>(index) * 12, record, sizeof record)) {
            break;
        }
        const std::uint32_t platform = U16(record);
        const std::uint32_t language = U16(record + 4);
        const std::uint32_t id = U16(record + 6);
        const std::uint32_t length = U16(record + 8);
        const std::uint32_t offset = U16(record + 10);
        int slot = -1;
        if (id == 1) {
            slot = 0;
        } else if (id == 2) {
            slot = 1;
        } else if (id == 16) {
            slot = 2;
        } else if (id == 17) {
            slot = 3;
        }
        if (slot < 0 || length == 0 || length > 512 || (platform != 3 && platform != 1)) {
            continue;
        }
        const int score = platform == 3 ? (language == 0x409 ? 3 : 2) : (language == 0 ? 1 : 0);
        if (score <= rank[slot]) {
            continue;
        }
        std::string bytes(length, '\0');
        if (!ReadAt(in, strings + offset, reinterpret_cast<unsigned char*>(&bytes[0]), length)) {
            continue;
        }
        std::string text;
        if (platform == 3) {
            for (std::size_t at = 0; at + 1 < bytes.size(); at += 2) {
                AppendUtf8(text, (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 8) |
                                     static_cast<unsigned char>(bytes[at + 1]));
            }
        } else {
            for (const char unit : bytes) {
                if (static_cast<unsigned char>(unit) < 0x80) {
                    text.push_back(unit);
                }
            }
        }
        found[slot] = std::move(text);
        rank[slot] = score;
    }
    family = !found[2].empty() ? found[2] : found[0];
    style = !found[2].empty() && !found[3].empty() ? found[3] : found[1];
    return !family.empty();
}

std::vector<std::filesystem::path> FontFolders() {
    std::vector<std::filesystem::path> out;
#if defined(_WIN32)
    const char* windows = std::getenv("WINDIR");
    out.emplace_back(std::string(windows != nullptr ? windows : "C:\\Windows") + "\\Fonts");
    if (const char* local = std::getenv("LOCALAPPDATA")) {
        out.emplace_back(std::string(local) + "\\Microsoft\\Windows\\Fonts");
    }
#elif defined(__APPLE__)
    out.emplace_back("/System/Library/Fonts");
    out.emplace_back("/Library/Fonts");
    if (const char* home = std::getenv("HOME")) {
        out.emplace_back(std::string(home) + "/Library/Fonts");
    }
#else
    out.emplace_back("/usr/share/fonts");
    out.emplace_back("/usr/local/share/fonts");
    if (const char* home = std::getenv("HOME")) {
        out.emplace_back(std::string(home) + "/.local/share/fonts");
        out.emplace_back(std::string(home) + "/.fonts");
    }
#endif
    return out;
}

bool IsRegular(const std::string& style) {
    const std::string lower = Lower(style);
    return lower.empty() || lower == "regular" || lower == "book" || lower == "normal" || lower == "roman";
}

}  // namespace

const std::vector<SystemFont>& system_fonts() {
    static const std::vector<SystemFont> fonts = [] {
        std::vector<SystemFont> out;
        std::set<std::string> seen;
        for (const std::filesystem::path& folder : FontFolders()) {
            std::error_code error;
            if (!std::filesystem::is_directory(folder, error)) {
                continue;
            }
            for (std::filesystem::recursive_directory_iterator it(folder, error), end; !error && it != end;
                 it.increment(error)) {
                if (!it->is_regular_file(error)) {
                    continue;
                }
                const std::string extension = Lower(it->path().extension().string());
                if (extension != ".ttf" && extension != ".otf" && extension != ".ttc") {
                    continue;
                }
                const auto utf8 = it->path().u8string();
                const std::string path(reinterpret_cast<const char*>(utf8.data()), utf8.size());
                std::string family;
                std::string style;
                if (!ReadNames(path, family, style) || !IsRegular(style) || !seen.insert(Lower(family)).second) {
                    continue;
                }
                out.push_back(SystemFont{family, path});
            }
        }
        std::sort(out.begin(), out.end(),
                  [](const SystemFont& left, const SystemFont& right) { return Lower(left.family) < Lower(right.family); });
        return out;
    }();
    return fonts;
}

void set_editor_font_choice(const std::string& family) { Choice() = family; }

std::string editor_font_wanted() {
    if (!Choice().empty()) {
        return Choice();
    }
    std::string themed = current_theme().value("--ide-editor-font");
    while (!themed.empty() && (themed.front() == '"' || themed.front() == '\'' || themed.front() == ' ')) {
        themed.erase(themed.begin());
    }
    while (!themed.empty() && (themed.back() == '"' || themed.back() == '\'' || themed.back() == ' ')) {
        themed.pop_back();
    }
    return themed;
}

std::string editor_font_family() {
    const std::string wanted = editor_font_wanted();
    if (wanted.empty()) {
        return editor_mono_family();
    }
    static std::unordered_map<std::string, std::string> loaded;
    const std::string key = Lower(wanted);
    const auto cached = loaded.find(key);
    if (cached != loaded.end()) {
        return cached->second.empty() ? editor_mono_family() : cached->second;
    }
    std::string registered;
    for (const SystemFont& font : system_fonts()) {
        if (Lower(font.family) == key) {
            const std::string name = "Editor Font " + font.family;
            if (jadefx::Font::loadFile(name, font.path)) {
                registered = name;
            }
            break;
        }
    }
    loaded[key] = registered;
    return registered.empty() ? editor_mono_family() : registered;
}

}  // namespace ide
