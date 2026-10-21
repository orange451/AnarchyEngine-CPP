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

std::string ShaderLog(GLuint shader) {
    char log[4096] = {};
    glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    return log;
}

// The compiled stage, or 0 with its log in log.
GLuint CompileQuiet(GLenum type, const std::string& source, std::string& log) {
    GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        log = ShaderLog(shader);
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

std::string LoadShader(const char* filename, std::initializer_list<const char*> libraries) {
    const std::string main = LoadShader(filename);
    if (main.empty()) {
        return {};
    }
    const std::size_t lineEnd = main.find('\n');
    std::string out = main.substr(0, lineEnd == std::string::npos ? main.size() : lineEnd + 1);
    for (const char* library : libraries) {
        const std::string source = LoadShader(library);
        if (source.empty()) {
            return {};
        }
        out += source;
        out += '\n';
    }
    if (lineEnd != std::string::npos) {
        out += main.substr(lineEnd + 1);
    }
    return out;
}

bool CanDraw(GLuint program) {
    glValidateProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_VALIDATE_STATUS, &status);
    return status == GL_TRUE;
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

GLuint LinkProgramLog(const std::string& vertexSource, const std::string& fragmentSource, std::string& log,
                      LinkStage& failed) {
    log.clear();
    failed = LinkStage::Link;
    if (vertexSource.empty() || fragmentSource.empty()) {
        log = "a shader file of the engine's could not be read";
        failed = vertexSource.empty() ? LinkStage::Vertex : LinkStage::Fragment;
        return 0;
    }
    GLuint vertex = CompileQuiet(GL_VERTEX_SHADER, vertexSource, log);
    if (vertex == 0) {
        failed = LinkStage::Vertex;
        return 0;
    }
    GLuint fragment = CompileQuiet(GL_FRAGMENT_SHADER, fragmentSource, log);
    if (fragment == 0) {
        failed = LinkStage::Fragment;
        glDeleteShader(vertex);
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
        char text[4096] = {};
        glGetProgramInfoLog(program, sizeof text, nullptr, text);
        log = text;
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

}  // namespace runner
