#include "ShaderFile.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

fs::path ExecutableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path();
#elif defined(__APPLE__)
    std::string buffer(256, '\0');
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.assign(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return {};
        }
    }
    const std::size_t terminator = buffer.find('\0');
    if (terminator == std::string::npos) {
        return {};
    }
    buffer.resize(terminator);
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(buffer, error);
    return (error ? fs::path(buffer) : canonical).parent_path();
#else
    std::string buffer(256, '\0');
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return {};
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length));
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path();
#endif
}

bool IsFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return {};
    }

    std::string contents(static_cast<std::size_t>(size), '\0');
    file.seekg(0, std::ios::beg);
    if (size == 0) {
        return contents;
    }

    file.read(contents.data(), size);
    if (file.gcount() != size) {
        return {};
    }
    return contents;
}

void PrintShaderLog(GLuint shader, const char* stage) {
    char log[2048];
    glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    std::fprintf(stderr, "%s shader failed to compile:\n%s\n", stage, log);
}

void PrintProgramLog(GLuint program) {
    char log[2048];
    glGetProgramInfoLog(program, sizeof log, nullptr, log);
    std::fprintf(stderr, "Program failed to link:\n%s\n", log);
}

GLuint Compile(GLenum type, const char* source, const char* stage) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        PrintShaderLog(shader, stage);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

std::string LoadShader(const char* filename) {
    const fs::path exeDir = ExecutableDirectory();
    fs::path candidates[4];
    std::size_t count = 0;
    if (!exeDir.empty()) {
        candidates[count++] = exeDir / ".." / "Resources" / "shaders" / filename;
    }
    candidates[count++] = fs::path("shaders") / filename;
    if (!exeDir.empty()) {
        candidates[count++] = exeDir / "shaders" / filename;
        candidates[count++] = exeDir / ".." / "shaders" / filename;
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (!IsFile(candidates[i])) {
            continue;
        }
        std::string source = ReadTextFile(candidates[i]);
        if (source.empty()) {
            std::fprintf(stderr, "Could not read shader file %s\n", candidates[i].string().c_str());
            return {};
        }
        return source;
    }

    std::fprintf(stderr, "Could not find shader file \"%s\". Looked for:\n", filename);
    for (std::size_t i = 0; i < count; ++i) {
        std::fprintf(stderr, "  %s\n", candidates[i].string().c_str());
    }
    return {};
}

GLuint LinkProgram(const std::string& vertexSource, const std::string& fragmentSource, const char* name) {
    if (vertexSource.empty() || fragmentSource.empty()) {
        return 0;
    }

    const std::string vertexStage = std::string(name) + " vertex";
    const std::string fragmentStage = std::string(name) + " fragment";
    GLuint vertex = Compile(GL_VERTEX_SHADER, vertexSource.c_str(), vertexStage.c_str());
    GLuint fragment = Compile(GL_FRAGMENT_SHADER, fragmentSource.c_str(), fragmentStage.c_str());
    if (vertex == 0 || fragment == 0) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        PrintProgramLog(program);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}
