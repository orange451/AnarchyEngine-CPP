#include "amesh.hpp"
#include "runner/InstanceBuffer.hpp"
#include "runner/gl.hpp"

// Only GLFW's window calls: the GL names come from runner/gl.hpp.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

// By hand, not ctest (it needs a GL 3.3 context): amesh-gl-check uploads a
// skinned AMESH quad through GpuMesh, draws it with a shader that reads every
// attribute location, and checks the pixels and glGetError.
namespace {

using namespace anarchy::amesh;
using runner::GLenum;
using runner::GLint;
using runner::GLuint;

int gFailures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        ++gFailures;
    }
}

void ExpectNoGlError(const char* where) {
    const GLenum error = glGetError();
    Expect(error == runner::GL_NO_ERROR, std::string(where) + ": glGetError " + std::to_string(error));
}

// Red is the color attribute, green the weight on bone 1 (found through the
// integer bone attribute), blue the normal's z. The quad fills clip space.
const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aTangent;
layout(location = 4) in vec4 aColor;
layout(location = 5) in uvec4 aBone;
layout(location = 6) in vec4 aWeight;
out vec3 vColor;
void main() {
    float onTip = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (aBone[k] == 1u) {
            onTip += aWeight[k];
        }
    }
    // The UV and tangent must reach the shader too: (0..1, 1) here.
    float check = aUv.x * 0.0 + aTangent.x * aTangent.w;
    vColor = vec3(aColor.r, onTip, aNormal.z * check);
    gl_Position = vec4(aPosition.xy * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec3 vColor;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor, 1.0);
}
)";

// Instanced: each instance's model places the quad, its normal matrix's z
// column scales its tint, and the tint is the color.
const char* kInstancedVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 7) in mat4 aModel;
layout(location = 11) in mat3 aNormalMatrix;
layout(location = 14) in vec3 aTint;
out vec3 vColor;
void main() {
    vColor = aTint * (aNormalMatrix * vec3(0.0, 0.0, 1.0)).z;
    gl_Position = aModel * vec4(aPosition, 1.0);
}
)";

GLuint Compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, runner::GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader: %s\n", log);
    }
    return shader;
}

// A unit quad facing +Z; the bottom rides bone 0, the top 3/4 on bone 1.
Data SkinnedQuad() {
    Data data;
    const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.p[0] = corners[i][0], v.p[1] = corners[i][1];
        v.n[2] = 1;
        v.uv[0] = corners[i][0], v.uv[1] = corners[i][1];
        v.rgba[0] = 128;
        v.bone[0] = 0, v.bone[1] = 1;
        v.weight[0] = i < 2 ? 1.0f : 0.25f;
        v.weight[1] = i < 2 ? 0.0f : 0.75f;
        data.vertices.push_back(v);
    }
    data.indices = {0, 1, 2, 0, 2, 3};
    compute_tangents(data);
    Bone root;
    Bone tip;
    tip.parent = 0;
    data.bones = {root, tip};
    Subset lower;
    lower.tri_begin = 0, lower.tri_count = 1, lower.vert_begin = 0, lower.vert_count = 3;
    data.subsets = {lower};
    // Through the file format, as the engine will load it.
    return read(write(data));
}

struct Pixel {
    int r, g, b;
};

Pixel ReadPixel(int x, int y) {
    unsigned char rgba[4] = {};
    glReadPixels(x, y, 1, 1, runner::GL_RGBA, runner::GL_UNSIGNED_BYTE, rgba);
    return {rgba[0], rgba[1], rgba[2]};
}

bool Near(int value, int expected, int tolerance = 3) {
    return std::abs(value - expected) <= tolerance;
}

}  // namespace

int main() {
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    constexpr int kSize = 64;
    GLFWwindow* window = glfwCreateWindow(kSize, kSize, "amesh-gl-check", nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "no GL 3.3 core context\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    if (!runner::LoadGl([](const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); })) {
        return 1;
    }

    const GLuint program = glCreateProgram();
    const GLuint vertex = Compile(runner::GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = Compile(runner::GL_FRAGMENT_SHADER, kFragmentShader);
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, runner::GL_LINK_STATUS, &linked);
    Expect(linked != 0, "the check shader links");
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    const GLuint instanced = glCreateProgram();
    const GLuint instancedVertex = Compile(runner::GL_VERTEX_SHADER, kInstancedVertexShader);
    const GLuint instancedFragment = Compile(runner::GL_FRAGMENT_SHADER, kFragmentShader);
    glAttachShader(instanced, instancedVertex);
    glAttachShader(instanced, instancedFragment);
    glLinkProgram(instanced);
    GLint instancedLinked = 0;
    glGetProgramiv(instanced, runner::GL_LINK_STATUS, &instancedLinked);
    Expect(instancedLinked != 0, "the instanced check shader links");
    glDeleteShader(instancedVertex);
    glDeleteShader(instancedFragment);

    {
        const Data data = SkinnedQuad();
        GpuMesh mesh;
        Expect(!mesh.valid(), "a new GpuMesh is not valid");
        mesh.upload(data);
        ExpectNoGlError("upload");
        Expect(mesh.valid() && mesh.lod_count() == 1 && mesh.subset_count() == 1, "upload makes one LOD and one subset");

        glViewport(0, 0, kSize, kSize);
        glClearColor(0, 0, 0, 1);
        glClear(runner::GL_COLOR_BUFFER_BIT);
        glUseProgram(program);
        mesh.bind();
        mesh.draw(0);
        ExpectNoGlError("draw");
        const Pixel bottom = ReadPixel(kSize / 2, 0);
        const Pixel top = ReadPixel(kSize / 2, kSize - 1);
        Expect(Near(bottom.r, 128) && Near(bottom.g, 0) && Near(bottom.b, 255),
               "the bottom edge is color 128, bone 1 weight 0, normal z 1 (got " + std::to_string(bottom.r) + "," +
                   std::to_string(bottom.g) + "," + std::to_string(bottom.b) + ")");
        Expect(Near(top.g, 191, 6), "the top edge carries bone 1's 0.75 weight (got " + std::to_string(top.g) + ")");

        // Re-uploading keeps the same objects; a static mesh gets no influences.
        Data flat = data;
        flat.bones.clear();
        flat.subsets.clear();
        mesh.upload(flat, true);
        ExpectNoGlError("re-upload");
        glClear(runner::GL_COLOR_BUFFER_BIT);
        mesh.bind();
        mesh.draw();
        Expect(Near(ReadPixel(kSize / 2, kSize - 1).g, 0), "a static mesh's bone attribute is 0xFFFF");
        Expect(mesh.subset_count() == 0, "re-upload replaces the subsets");

        // The subset is the lower-right triangle only.
        mesh.upload(data);
        glClear(runner::GL_COLOR_BUFFER_BIT);
        mesh.bind();
        mesh.draw_subset(0);
        ExpectNoGlError("draw_subset");
        Expect(ReadPixel(kSize - 4, 4).r > 0 && ReadPixel(4, kSize - 4).r == 0, "draw_subset draws its triangle only");

        bool threw = false;
        try {
            mesh.draw(1);
        } catch (const std::out_of_range&) {
            threw = true;
        }
        Expect(threw, "drawing a missing LOD throws");

        // Three instances of the 0..1 quad, each moved into a quadrant of clip
        // space (a quadrant is 1 wide, as the quad is): bottom left red, bottom
        // right green, top left blue. Identity normal matrices pass the tint on.
        mesh.upload(data);
        runner::InstanceData rows[3] = {};
        const float corners[3][2] = {{-1.f, -1.f}, {0.f, -1.f}, {-1.f, 0.f}};
        for (int i = 0; i < 3; ++i) {
            rows[i].model[0] = rows[i].model[5] = rows[i].model[10] = rows[i].model[15] = 1.f;
            rows[i].model[12] = corners[i][0];
            rows[i].model[13] = corners[i][1];
            rows[i].normal[0] = rows[i].normal[4] = rows[i].normal[8] = 1.f;
            rows[i].tint[i] = 1.f;
        }
        runner::InstanceBuffer instances;
        instances.upload(rows, 3);
        glUseProgram(instanced);
        glClear(runner::GL_COLOR_BUFFER_BIT);
        mesh.bind();
        instances.attach(0);
        mesh.draw_instanced(0, 3);
        ExpectNoGlError("draw_instanced");
        const int q = kSize / 4;
        const Pixel bottomLeft = ReadPixel(q, q);
        const Pixel bottomRight = ReadPixel(3 * q, q);
        const Pixel topLeft = ReadPixel(q, 3 * q);
        const Pixel topRight = ReadPixel(3 * q, 3 * q);
        Expect(bottomLeft.r > 200 && bottomLeft.g < 50, "instance 0 draws red bottom left");
        Expect(bottomRight.g > 200 && bottomRight.r < 50, "instance 1 draws green bottom right");
        Expect(topLeft.b > 200 && topLeft.r < 50, "instance 2 draws blue top left");
        Expect(topRight.r == 0 && topRight.g == 0 && topRight.b == 0, "nothing draws top right");

        // attach(1) starts at the second row: two instances, green and blue.
        glClear(runner::GL_COLOR_BUFFER_BIT);
        instances.attach(1);
        mesh.draw_instanced(0, 2);
        Expect(ReadPixel(q, q).r == 0 && ReadPixel(3 * q, q).g > 200 && ReadPixel(q, 3 * q).b > 200,
               "attach(first) starts at row first");
        ExpectNoGlError("attach(1)");

        // A plain draw of the same mesh still works after an instanced one.
        glUseProgram(program);
        glClear(runner::GL_COLOR_BUFFER_BIT);
        mesh.bind();
        mesh.draw(0);
        Expect(Near(ReadPixel(kSize / 2, 0).r, 128), "a plain draw after an instanced one still draws");
        ExpectNoGlError("plain after instanced");

        bool instancedThrew = false;
        try {
            mesh.draw_instanced(1, 1);
        } catch (const std::out_of_range&) {
            instancedThrew = true;
        }
        Expect(instancedThrew, "draw_instanced with a missing LOD throws");
        mesh.draw_instanced(0, 0);
        ExpectNoGlError("draw_instanced of 0");
        instances.destroy();
        ExpectNoGlError("InstanceBuffer::destroy");

        GpuMesh moved = std::move(mesh);
        Expect(moved.valid() && !mesh.valid(), "moving a GpuMesh moves its objects");
        moved.destroy();
        Expect(!moved.valid(), "destroy releases the objects");
        ExpectNoGlError("destroy");

        GpuMesh scoped;
        scoped.upload(data);
    }
    ExpectNoGlError("destructor");

    glDeleteProgram(program);
    glDeleteProgram(instanced);
    glfwDestroyWindow(window);
    glfwTerminate();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d GpuMesh check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("GpuMesh checks passed\n");
    return 0;
}
