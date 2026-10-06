#include "EnvironmentMap.hpp"

#include "ShaderFile.hpp"
#include "gl.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace runner {
namespace {

// The unit each program reads its source from.
constexpr int kUnitSource = 0;

unsigned MakeCube(int size, bool mipmapped) {
    unsigned texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, texture);
    for (int face = 0; face < 6; ++face) {
        int levelSize = size;
        for (int level = 0; levelSize >= 1 && (level == 0 || mipmapped); ++level, levelSize /= 2) {
            glTexImage2D(RT_GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(face), level,
                         static_cast<GLint>(RT_GL_RGBA16F), levelSize, levelSize, 0, GL_RGBA, RT_GL_HALF_FLOAT,
                         nullptr);
        }
    }
    glTexParameteri(RT_GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                    static_cast<GLint>(mipmapped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR));
    glTexParameteri(RT_GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    for (const GLenum wrap : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, RT_GL_TEXTURE_WRAP_R}) {
        glTexParameteri(RT_GL_TEXTURE_CUBE_MAP, wrap, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
    }
    return texture;
}

// Mips past the last roughness are never drawn or read.
unsigned MakePrefiltered(int size) {
    const unsigned texture = MakeCube(size, true);
    glTexParameteri(RT_GL_TEXTURE_CUBE_MAP, RT_GL_TEXTURE_MAX_LEVEL, EnvironmentMap::kPrefilteredLevels - 1);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, 0);
    return texture;
}

}  // namespace

bool EnvironmentMap::buildProgram(Program& program, const char* name, const char* fragment) {
    program = Program{};
    program.id = LinkProgram(LoadShader("pipeline/fullscreen.vert"),
                             LoadShader(fragment, {"pipeline/environment.glsl"}), name);
    if (program.id == 0) {
        return false;
    }
    const unsigned id = program.id;
    program.face = glGetUniformLocation(id, "uFace");
    program.roughness = glGetUniformLocation(id, "uRoughness");
    program.environmentSize = glGetUniformLocation(id, "uEnvironmentSize");
    program.samples = glGetUniformLocation(id, "uSamples");
    glUseProgram(id);
    for (const char* sampler : {"uSky", "uEnvironment"}) {
        const int location = glGetUniformLocation(id, sampler);
        if (location >= 0) {
            glUniform1i(location, kUnitSource);
        }
    }
    glUseProgram(0);
    return true;
}

bool EnvironmentMap::initialize() {
    const bool built = buildProgram(equirect_, "Sky to cube", "pipeline/equirect.frag") &&
                       buildProgram(irradianceProgram_, "Sky irradiance", "pipeline/irradiance.frag") &&
                       buildProgram(prefilter_, "Sky reflections", "pipeline/prefilter.frag") &&
                       buildProgram(brdfProgram_, "Reflection table", "pipeline/brdf.frag");
    if (!built) {
        shutdown();
    }
    return built;
}

void EnvironmentMap::shutdown() {
    for (Program* program : {&equirect_, &irradianceProgram_, &prefilter_, &brdfProgram_}) {
        if (program->id != 0) {
            glDeleteProgram(program->id);
        }
        *program = Program{};
    }
    if (framebuffer_ != 0) {
        glDeleteFramebuffers(1, &framebuffer_);
        framebuffer_ = 0;
    }
    for (unsigned* texture :
         {&environment_, &irradiance_, &prefiltered_, &brdf_, &spareIrradiance_, &sparePrefiltered_}) {
        DeleteTexture(*texture);
    }
    slice_ = -1;
    brdfDrawn_ = false;
    imageRevision_ = 0;
    environmentSize_ = 0;
    prefilteredSize_ = 0;
    procedural_ = false;
    refused_ = false;
}

bool EnvironmentMap::ensureTextures(int environmentSize, int prefilteredSize) {
    if (framebuffer_ != 0 && environmentSize == environmentSize_ && prefilteredSize == prefilteredSize_) {
        return true;
    }
    if (refused_) {
        return false;
    }
    // Made again at the new sizes: whatever they held is gone.
    for (unsigned* texture : {&environment_, &irradiance_, &prefiltered_, &spareIrradiance_, &sparePrefiltered_}) {
        DeleteTexture(*texture);
    }
    slice_ = -1;
    imageRevision_ = 0;
    procedural_ = false;
    environmentSize_ = 0;
    prefilteredSize_ = 0;
    glActiveTexture(GL_TEXTURE0 + kUnitSource);
    environment_ = MakeCube(environmentSize, true);
    irradiance_ = MakeCube(kIrradianceSize, false);
    prefiltered_ = MakePrefiltered(prefilteredSize);

    if (brdf_ == 0) {
        glGenTextures(1, &brdf_);
        glBindTexture(GL_TEXTURE_2D, brdf_);
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(RT_GL_RGBA16F), kBrdfSize, kBrdfSize, 0, GL_RGBA,
                     RT_GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
        glBindTexture(GL_TEXTURE_2D, 0);
        brdfDrawn_ = false;
    }
    if (framebuffer_ == 0) {
        glGenFramebuffers(1, &framebuffer_);
        glBindFramebuffer(RT_GL_FRAMEBUFFER, framebuffer_);
        const GLenum buffer = RT_GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &buffer);
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, framebuffer_);
    bool complete = true;
    for (const unsigned cube : {environment_, irradiance_, prefiltered_}) {
        glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0, RT_GL_TEXTURE_CUBE_MAP_POSITIVE_X, cube, 0);
        complete = complete && glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) == RT_GL_FRAMEBUFFER_COMPLETE;
    }
    glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, brdf_, 0);
    complete = complete && glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) == RT_GL_FRAMEBUFFER_COMPLETE;
    if (!complete) {
        std::fprintf(stderr, "The sky's environment cubes are not supported; the sky is not drawn.\n");
        // Keeps the programs, but no textures: a later call does not try again.
        for (unsigned* texture : {&environment_, &irradiance_, &prefiltered_, &brdf_}) {
            DeleteTexture(*texture);
        }
        glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer_);
        framebuffer_ = 0;
        brdfDrawn_ = false;
        refused_ = true;
        return false;
    }
    environmentSize_ = environmentSize;
    prefilteredSize_ = prefilteredSize;
    return true;
}

bool EnvironmentMap::drawFaces(const Program& program, unsigned cube, int level, int size, unsigned emptyVao) {
    glBindFramebuffer(RT_GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, size, size);
    glUseProgram(program.id);
    glBindVertexArray(emptyVao);
    for (int face = 0; face < 6; ++face) {
        glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0,
                               RT_GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(face), cube, level);
        if (face == 0 && !CanDraw(program.id)) {
            return false;
        }
        glUniform1i(program.face, face);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    return true;
}

bool EnvironmentMap::drawEnvironment(unsigned image, unsigned emptyVao) {
    glActiveTexture(GL_TEXTURE0 + kUnitSource);
    // Not bound while it is drawn into.
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, 0);
    glBindTexture(GL_TEXTURE_2D, image);
    glUseProgram(equirect_.id);
    glUniform1f(equirect_.environmentSize, static_cast<float>(environmentSize_));
    if (!drawFaces(equirect_, environment_, 0, environmentSize_, emptyVao)) {
        return false;
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, environment_);
    glGenerateMipmap(RT_GL_TEXTURE_CUBE_MAP);
    return true;
}

bool EnvironmentMap::ensureBrdf(unsigned emptyVao) {
    if (brdfDrawn_) {
        return true;
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, brdf_, 0);
    glViewport(0, 0, kBrdfSize, kBrdfSize);
    glUseProgram(brdfProgram_.id);
    glBindVertexArray(emptyVao);
    if (!CanDraw(brdfProgram_.id)) {
        return false;
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    brdfDrawn_ = true;
    return true;
}

bool EnvironmentMap::filter(unsigned emptyVao) {
    if (!filterIrradiance(irradiance_, kIrradianceSamples, emptyVao) ||
        !filterLevels(prefiltered_, (1u << kPrefilteredLevels) - 1u, false, emptyVao)) {
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

bool EnvironmentMap::filterIrradiance(unsigned irradiance, int samples, unsigned emptyVao) {
    // The diffuse light.
    glActiveTexture(GL_TEXTURE0 + kUnitSource);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, environment_);
    glUseProgram(irradianceProgram_.id);
    glUniform1f(irradianceProgram_.environmentSize, static_cast<float>(environmentSize_));
    glUniform1i(irradianceProgram_.samples, samples);
    if (!drawFaces(irradianceProgram_, irradiance, 0, kIrradianceSize, emptyVao)) {
        return false;
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, 0);
    return true;
}

bool EnvironmentMap::filterLevels(unsigned prefiltered, unsigned levels, bool procedural, unsigned emptyVao) {
    // The reflections, from the same environment cube.
    glActiveTexture(GL_TEXTURE0 + kUnitSource);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, environment_);
    glUseProgram(prefilter_.id);
    glUniform1f(prefilter_.environmentSize, static_cast<float>(environmentSize_));
    for (int level = 0; level < kPrefilteredLevels; ++level) {
        if (((levels >> level) & 1u) == 0) {
            continue;
        }
        const int samples =
            !procedural ? kPrefilterSamples : level < 3 ? kProceduralPrefilterSamples : kProceduralRoughSamples;
        glUseProgram(prefilter_.id);
        glUniform1f(prefilter_.roughness, static_cast<float>(level) / static_cast<float>(kPrefilteredLevels - 1));
        glUniform1i(prefilter_.samples, samples);
        if (!drawFaces(prefilter_, prefiltered, level, std::max(prefilteredSize_ >> level, 1), emptyVao)) {
            return false;
        }
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, 0);
    return true;
}

namespace {

void BeginDrawing() {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(RT_GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    // Filtering reads across the edges of faces, so the cubes have no seams.
    glEnable(RT_GL_TEXTURE_CUBE_MAP_SEAMLESS);
}

}  // namespace

bool EnvironmentMap::update(unsigned image, std::uint64_t imageRevision, unsigned emptyVao) {
    if (image == 0 || equirect_.id == 0) {
        return false;
    }
    if (brdfDrawn_ && !procedural_ && imageRevision == imageRevision_ && environmentSize_ == kEnvironmentSize &&
        prefilteredSize_ == kPrefilteredSize) {
        return true;
    }
    if (!ensureTextures(kEnvironmentSize, kPrefilteredSize)) {
        return false;
    }
    BeginDrawing();
    // Whatever was made before is not the sky now, whether or not this finishes.
    imageRevision_ = 0;
    procedural_ = false;
    slice_ = -1;
    if (!ensureBrdf(emptyVao) || !drawEnvironment(image, emptyVao) || !filter(emptyVao)) {
        return false;
    }
    static_assert((kPrefilteredSize >> (kPrefilteredLevels - 1)) >= 1, "the prefiltered cube's mips run out at 1 by 1");
    imageRevision_ = imageRevision;
    return true;
}

bool EnvironmentMap::startProcedural(int environmentSize, int prefilteredSize) {
    slice_ = -1;
    if (equirect_.id == 0 || (prefilteredSize >> (kPrefilteredLevels - 1)) < 1 ||
        !ensureTextures(environmentSize, prefilteredSize)) {
        return false;
    }
    if (spareIrradiance_ == 0) {
        // The same formats and sizes as the cubes in use, which the driver renders into.
        glActiveTexture(GL_TEXTURE0 + kUnitSource);
        spareIrradiance_ = MakeCube(kIrradianceSize, false);
        sparePrefiltered_ = MakePrefiltered(prefilteredSize_);
    }
    slice_ = 0;
    return true;
}

bool EnvironmentMap::continueProcedural(unsigned emptyVao, const std::function<bool(int face)>& drawFace) {
    if (slice_ < 0) {
        return false;
    }
    BeginDrawing();
    if (!ensureBrdf(emptyVao)) {
        return false;
    }
    // With no procedural sky in use to light the frames between, all at once.
    const int last = procedural_ ? slice_ : kLightingSlices - 1;
    for (; slice_ <= last; ++slice_) {
        if (!drawSlice(LightingSliceAt(slice_), emptyVao, drawFace)) {
            return false;
        }
    }
    if (slice_ < kLightingSlices) {
        return false;
    }
    std::swap(irradiance_, spareIrradiance_);
    std::swap(prefiltered_, sparePrefiltered_);
    slice_ = -1;
    imageRevision_ = 0;
    procedural_ = true;
    return true;
}

bool EnvironmentMap::drawSlice(const LightingSlice& slice, unsigned emptyVao,
                               const std::function<bool(int face)>& drawFace) {
    if (slice.faceBegin < slice.faceEnd) {
        glActiveTexture(GL_TEXTURE0 + kUnitSource);
        // Not bound while it is drawn into.
        glBindTexture(RT_GL_TEXTURE_CUBE_MAP, 0);
        glBindFramebuffer(RT_GL_FRAMEBUFFER, framebuffer_);
        glViewport(0, 0, environmentSize_, environmentSize_);
        glBindVertexArray(emptyVao);
        for (int face = slice.faceBegin; face < slice.faceEnd; ++face) {
            glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0,
                                   RT_GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(face), environment_, 0);
            if (!drawFace(face)) {
                return false;
            }
        }
        glBindFramebuffer(RT_GL_FRAMEBUFFER, 0);
    }
    if (slice.diffuse) {
        glActiveTexture(GL_TEXTURE0 + kUnitSource);
        glBindTexture(RT_GL_TEXTURE_CUBE_MAP, environment_);
        glGenerateMipmap(RT_GL_TEXTURE_CUBE_MAP);
        if (!filterIrradiance(spareIrradiance_, kProceduralIrradianceSamples, emptyVao)) {
            return false;
        }
    }
    if (slice.levels != 0u && !filterLevels(sparePrefiltered_, slice.levels, true, emptyVao)) {
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

}  // namespace runner
