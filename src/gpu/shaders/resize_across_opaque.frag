#version 440
#extension GL_GOOGLE_include_directive : require

// The horizontal half of the resize of an opaque image: Resample.cpp's
// horizontalPass<true>.
//
// Mirrors it, and must change with it; see resize_across.frag for where the
// weights come from and the precision. With every alpha exactly one, the
// premultiplied sum of a window is its filtered colour, no range of colours or
// translucency has to be kept for the vertical pass, and so one render writes
// the whole intermediate: the sums with the rule against ringing below black,
// and alpha one.

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D source;

// The Resize block, the weights of the resized axis, and tapWeight and store.
#include "common/resize.glsl"

void main() {
    // gl_FragCoord is (x + 0.5, y + 0.5): output column x of row y.
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const vec4 header = texelFetch(weights, ivec2(0, pixel.x), 0);
    const int first = int(header.x);
    const int count = int(header.y);
    const int last = int(plan.inputLength) - 1;

    vec3 sum = vec3(0.0);
    vec3 negative = vec3(0.0);
    for (int k = 0; k < count; ++k) {
        const float weight = tapWeight(pixel.x, k);
        const int index = clamp(first + k, 0, last);
        const vec3 colour = texelFetch(source, ivec2(index, pixel.y) + ivec2(plan.offset), 0).rgb;
        sum += weight * colour;
        negative = max(negative, vec3(lessThan(colour, vec3(0.0))));
    }
    fragColor = vec4(store(sum.r, negative.r), store(sum.g, negative.g),
                     store(sum.b, negative.b), 1.0);
}
