#version 330 core
// FXAA 3.11 (Timothy Lottes), PC quality preset 12, on the tone-mapped
// image: finds each edge's direction and length and blends across it.
// Pixels whose local contrast is under the thresholds return their own color
// after five reads, so flat areas, and the empty pane, come through exactly.
// Offsets follow the original's: S is +y, N is -y. Only consistency matters.
in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uScene;
// One texel of uScene.
uniform vec2 uTexel;

const float kSubpix = 0.75;
const float kEdgeThreshold = 0.166;
const float kEdgeThresholdMin = 0.0833;
const int kSteps = 5;
const float kStep[5] = float[5](1.0, 1.5, 2.0, 4.0, 12.0);

float luma(vec3 color) {
    return dot(color, vec3(0.299, 0.587, 0.114));
}

float lumaAt(vec2 uv) {
    return luma(textureLod(uScene, uv, 0.0).rgb);
}

float lumaOff(vec2 uv, float x, float y) {
    return lumaAt(uv + vec2(x, y) * uTexel);
}

void main() {
    vec2 posM = vUv;
    vec3 rgbM = textureLod(uScene, posM, 0.0).rgb;
    float lumaM = luma(rgbM);
    float lumaS = lumaOff(posM, 0.0, 1.0);
    float lumaE = lumaOff(posM, 1.0, 0.0);
    float lumaN = lumaOff(posM, 0.0, -1.0);
    float lumaW = lumaOff(posM, -1.0, 0.0);
    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    if (range < max(kEdgeThresholdMin, rangeMax * kEdgeThreshold)) {
        fragColor = vec4(rgbM, 1.0);
        return;
    }

    float lumaNW = lumaOff(posM, -1.0, -1.0);
    float lumaSE = lumaOff(posM, 1.0, 1.0);
    float lumaNE = lumaOff(posM, 1.0, -1.0);
    float lumaSW = lumaOff(posM, -1.0, 1.0);

    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS;
    float edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE;
    float edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
    float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW;
    float edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4;
    float edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = uTexel.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) {
        lumaN = lumaW;
        lumaS = lumaE;
    } else {
        lengthSign = uTexel.y;
    }
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM;
    float gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM;
    float lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) {
        lengthSign = -lengthSign;
    }
    float subpixC = clamp(abs(subpixB) * subpixRcpRange, 0.0, 1.0);

    vec2 posB = posM;
    vec2 offNP = horzSpan ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
    if (!horzSpan) {
        posB.x += lengthSign * 0.5;
    } else {
        posB.y += lengthSign * 0.5;
    }
    vec2 posN = posB - offNP * kStep[0];
    vec2 posP = posB + offNP * kStep[0];
    float subpixD = -2.0 * subpixC + 3.0;
    float subpixE = subpixC * subpixC;
    if (!pairN) {
        lumaNN = lumaSS;
    }
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    float lumaEndN = lumaAt(posN) - lumaNN * 0.5;
    float lumaEndP = lumaAt(posP) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    for (int i = 1; i < kSteps && !(doneN && doneP); ++i) {
        if (!doneN) {
            posN -= offNP * kStep[i];
            lumaEndN = lumaAt(posN) - lumaNN * 0.5;
            doneN = abs(lumaEndN) >= gradientScaled;
        }
        if (!doneP) {
            posP += offNP * kStep[i];
            lumaEndP = lumaAt(posP) - lumaNN * 0.5;
            doneP = abs(lumaEndP) >= gradientScaled;
        }
    }

    float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
    float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLength = dstP + dstN;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = dst * (-1.0 / spanLength) + 0.5;
    float subpixH = subpixG * kSubpix;
    float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan) {
        posM.x += pixelOffsetSubpix * lengthSign;
    } else {
        posM.y += pixelOffsetSubpix * lengthSign;
    }
    fragColor = vec4(textureLod(uScene, posM, 0.0).rgb, 1.0);
}
