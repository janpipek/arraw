#version 440
#extension GL_GOOGLE_include_directive : require

// The vertical half of the resize: Resample.cpp's verticalPass, which also
// unpremultiplies each pixel into the result.
//
// Mirrors it, and must change with it; see resize_across.frag for where the
// weights come from, why the horizontal pass is split into planes, and the
// precision. Here the three planes it wrote are read back in, the weights are
// those of the vertical axis, and each pixel is one output pixel.
//
// Lanczos weights are signed, so premultiplied colour and alpha are different
// signed sums and their quotient is unbounded near transparency. Alpha is
// clamped to [0, 1], pixels below transparentBelow become transparent, and
// where the window holds any pixel that is not opaque, colour is clamped to the
// range of the visible pixels in it. An opaque window is left to the filter and
// the rule against ringing below zero alone.

layout(location = 0) out vec4 fragColor;

// Planes of resize_across.frag, in the order of ResizePlane.
layout(binding = 0) uniform sampler2D sums;
layout(binding = 3) uniform sampler2D lows;
layout(binding = 4) uniform sampler2D highs;

// The Resize block, the weights of the resized axis, and tapWeight and store.
#include "common/resize.glsl"

void main() {
    // gl_FragCoord is (x + 0.5, y + 0.5): output column x of row y.
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const vec4 header = texelFetch(weights, ivec2(0, pixel.y), 0);
    const int first = int(header.x);
    const int count = int(header.y);
    const int last = int(plan.inputLength) - 1;

    vec4 sum = vec4(0.0);
    vec4 negative = vec4(0.0);
    vec3 low = vec3(floatMax);
    vec3 high = vec3(-floatMax);
    float translucent = 0.0;
    for (int k = 0; k < count; ++k) {
        const float weight = tapWeight(pixel.y, k);
        const int row = clamp(first + k, 0, last);
        const ivec2 at = ivec2(pixel.x, row);
        const vec4 across = texelFetch(sums, at, 0);
        const vec4 lowAcross = texelFetch(lows, at, 0);
        const vec4 highAcross = texelFetch(highs, at, 0);
        sum += weight * across;
        // The flags come from the stored horizontal sums, as on the CPU, not
        // from the horizontal pass's own.
        negative = max(negative, vec4(lessThan(across, vec4(0.0))));
        low = min(low, lowAcross.rgb);
        high = max(high, highAcross.rgb);
        if (lowAcross.a != 0.0) {
            translucent = 1.0;
        }
    }

    const vec3 stored = vec3(store(sum.r, negative.r), store(sum.g, negative.g),
                             store(sum.b, negative.b));
    if (translucent == 0.0) {
        // An opaque window gives exactly opaque, as on the CPU, where the
        // weights sum to 1 in double. The float sums can fall short by an ulp,
        // and a pixel that is not exactly 1 is refused by the JPEG export.
        fragColor = vec4(stored, 1.0);
        return;
    }
    const float alpha = clamp(store(sum.a, negative.a), 0.0, 1.0);
    if (alpha < transparentBelow) {
        fragColor = vec4(0.0);
        return;
    }
    // A window with no visible colour has low above high: colour 0.
    const vec3 quotient = stored / alpha;
    const vec3 colour = vec3(low.r <= high.r ? clamp(quotient.r, low.r, high.r) : 0.0,
                             low.g <= high.g ? clamp(quotient.g, low.g, high.g) : 0.0,
                             low.b <= high.b ? clamp(quotient.b, low.b, high.b) : 0.0);
    fragColor = vec4(colour, alpha);
}
