#include "ShaderAssembly.hpp"

#include "ide/IdeResources.hpp"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <sstream>

namespace runner {
namespace {

// A shader file under resources/shaders, as LoadShader finds it; empty when it cannot be read.
std::string LoadShader(const char* filename) {
    const std::filesystem::path path = ide::find_resource(std::string("shaders/") + filename);
    std::string source;
    std::string error;
    if (path.empty() || !ide::read_file(path, source, error)) {
        return {};
    }
    return source;
}

int count_lines(const std::string& text) { return static_cast<int>(std::count(text.begin(), text.end(), '\n')); }

std::string trim(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

}  // namespace

AssembledSource AssembleSurface(const char* main_file, std::initializer_list<const char*> libraries,
                                const std::string& user_glsl, bool fragment) {
    AssembledSource out;
    const std::string main = LoadShader(main_file);
    if (main.empty()) {
        return out;
    }
    const std::size_t line_end = main.find('\n');
    std::string text = main.substr(0, line_end == std::string::npos ? main.size() : line_end + 1);
    if (text.back() != '\n') {
        text += '\n';
    }
    text += "#define SURFACE_SHADER\n";
    for (const char* library : libraries) {
        const std::string source = LoadShader(library);
        if (source.empty()) {
            return out;
        }
        text += source;
        text += '\n';
    }
    if (fragment) {
        const std::string hook = LoadShader("pipeline/surface_shader.glsl");
        if (hook.empty()) {
            return out;
        }
        text += hook;
        text += '\n';
        out.user_first_line = count_lines(text) + 1;
        std::string user = user_glsl;
        if (!user.empty() && user.back() != '\n') {
            user += '\n';
        }
        out.user_lines = count_lines(user);
        text += user;
    }
    if (line_end != std::string::npos) {
        text += main.substr(line_end + 1);
    }
    out.text = std::move(text);
    return out;
}

AssembledSource AssemblePost(const std::string& user_glsl) {
    AssembledSource out;
    const std::string main = LoadShader("pipeline/post.frag");
    if (main.empty()) {
        return out;
    }
    const std::size_t line_end = main.find('\n');
    std::string text = main.substr(0, line_end == std::string::npos ? main.size() : line_end + 1);
    if (text.back() != '\n') {
        text += '\n';
    }
    text += "#define POST_SHADER\n";
    for (const char* library : {"pipeline/gbuffer.glsl", "pipeline/view_position.glsl", "pipeline/post_shader.glsl"}) {
        const std::string source = LoadShader(library);
        if (source.empty()) {
            return out;
        }
        text += source;
        text += '\n';
    }
    out.user_first_line = count_lines(text) + 1;
    std::string user = user_glsl;
    if (!user.empty() && user.back() != '\n') {
        user += '\n';
    }
    out.user_lines = count_lines(user);
    text += user;
    if (line_end != std::string::npos) {
        text += main.substr(line_end + 1);
    }
    out.text = std::move(text);
    return out;
}

AssembledSource AssembleVertex(const char* main_file, const std::string& user_glsl) {
    AssembledSource out;
    const std::string main = LoadShader(main_file);
    const std::string hook = LoadShader("pipeline/vertex_shader.glsl");
    if (main.empty() || hook.empty()) {
        return out;
    }
    const std::size_t line_end = main.find('\n');
    std::string text = main.substr(0, line_end == std::string::npos ? main.size() : line_end + 1);
    if (text.back() != '\n') {
        text += '\n';
    }
    text += "#define VERTEX_SHADER\n";
    text += hook;
    text += '\n';
    out.user_first_line = count_lines(text) + 1;
    std::string user = user_glsl;
    if (!user.empty() && user.back() != '\n') {
        user += '\n';
    }
    out.user_lines = count_lines(user);
    text += user;
    if (line_end != std::string::npos) {
        text += main.substr(line_end + 1);
    }
    out.text = std::move(text);
    return out;
}

std::string MapShaderError(const std::string& log, const AssembledSource& source) {
    static const std::regex nvidia(R"(^\s*\d+\((\d+)\)\s*:\s*error\b[^:]*:\s*(.*)$)");
    static const std::regex mesa(R"(^\s*\d+:(\d+)\(\d+\)\s*:\s*error\s*:\s*(.*)$)");
    static const std::regex apple(R"(^\s*ERROR:\s*\d+:(\d+):\s*(.*)$)");
    std::istringstream lines(log);
    std::string line;
    std::string first;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (first.empty()) {
            first = trim(line);
        }
        std::smatch match;
        if (std::regex_match(line, match, nvidia) || std::regex_match(line, match, mesa) ||
            std::regex_match(line, match, apple)) {
            const int at = std::stoi(match[1].str());
            const int last = std::max(1, source.user_lines);
            int user = at - source.user_first_line + 1;
            user = user < 1 ? 1 : std::min(user, last);
            return std::to_string(user) + ": " + trim(match[2].str());
        }
    }
    return first.empty() ? std::string() : "1: " + first;
}

}  // namespace runner
