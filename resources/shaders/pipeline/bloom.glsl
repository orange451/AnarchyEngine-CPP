// Bloom's soft threshold: BloomMath's BrightWeight, line for line, which the
// sandbox tests pin. No #version: Renderer puts it in after the main file's.

// How much of a color whose brightest channel is brightness goes into the
// bloom, 0 to 1: nothing below threshold minus a knee of half the threshold,
// a quadratic fade across the knee, then the light over the threshold.
float brightWeight(float brightness, float threshold) {
    if (brightness <= 0.0) {
        return 0.0;
    }
    if (threshold <= 0.0) {
        return 1.0;
    }
    float knee = 0.5 * threshold;
    float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    return max(soft, brightness - threshold) / brightness;
}

vec3 brightPart(vec3 color, float threshold) {
    return color * brightWeight(max(color.r, max(color.g, color.b)), threshold);
}
