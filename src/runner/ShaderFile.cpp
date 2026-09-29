#include "ShaderFile.hpp"

#include "ide/IdeResources.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace runner {
namespace {

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
    const std::filesystem::path path = ide::find_resource(std::string("shaders/") + filename);
    if (path.empty()) {
        return {};
    }
    std::string source;
    std::string error;
    if (!ide::read_file(path, source, error) || source.empty()) {
        std::fprintf(stderr, "Could not read shader file %s\n", path.string().c_str());
        return {};
    }
    return source;
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

}  // namespace runner
