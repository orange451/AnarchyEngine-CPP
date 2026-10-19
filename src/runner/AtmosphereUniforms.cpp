#include "AtmosphereUniforms.hpp"

#include "gl.hpp"

namespace runner {

AtmosphereLocations FindAtmosphere(unsigned program) {
    const auto at = [program](const char* uniform) { return glGetUniformLocation(program, uniform); };
    AtmosphereLocations where;
    where.enabled = at("uAtmosphereEnabled");
    where.sigma0 = at("uAtmosphereSigma0");
    where.falloff = at("uAtmosphereFalloff");
    where.height = at("uAtmosphereHeight");
    where.up = at("uAtmosphereUp");
    where.color = at("uAtmosphereColor");
    where.aerialPerspective = at("uAtmosphereAerial");
    where.skyHaze = at("uAtmosphereSkyHaze");
    where.skyDistance = at("uAtmosphereSkyDistance");
    where.lightDirection = at("uAtmosphereLightDirection");
    where.lightColor = at("uAtmosphereLightColor");
    where.skyReady = at("uAtmosphereSkyReady");
    where.viewToSky = at("uAtmosphereViewToSky");
    where.skyColor = at("uAtmosphereSkyColor");
    where.skyMaxLod = at("uAtmosphereSkyMaxLod");
    where.sky = at("uAtmosphereSky");
    return where;
}

void UploadAtmosphere(const AtmosphereLocations& where, const AtmosphereFrame& frame, int skyUnit) {
    glUniform1f(where.enabled, frame.enabled ? 1.f : 0.f);
    // The sampler points somewhere either way, so it never reads a unit holding a 2D texture.
    glUniform1i(where.sky, skyUnit);
    if (frame.skyCube != 0) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(skyUnit));
        glBindTexture(RT_GL_TEXTURE_CUBE_MAP, frame.skyCube);
        glActiveTexture(GL_TEXTURE0);
    }
    if (!frame.enabled) {
        return;
    }
    glUniform1f(where.sigma0, frame.sigma0);
    glUniform1f(where.falloff, frame.falloff);
    glUniform1f(where.height, frame.height);
    glUniform3fv(where.up, 1, frame.up);
    glUniform3fv(where.color, 1, frame.color);
    glUniform1f(where.aerialPerspective, frame.aerialPerspective);
    glUniform1f(where.skyHaze, frame.skyHaze);
    glUniform1f(where.skyDistance, frame.skyDistance);
    glUniform3fv(where.lightDirection, 1, frame.lightDirection);
    glUniform3fv(where.lightColor, 1, frame.lightColor);
    glUniform1f(where.skyReady, frame.skyReady ? 1.f : 0.f);
    glUniformMatrix3fv(where.viewToSky, 1, GL_FALSE, frame.viewToSky);
    glUniform3fv(where.skyColor, 1, frame.skyColor);
    glUniform1f(where.skyMaxLod, frame.skyMaxLod);
}

}  // namespace runner
