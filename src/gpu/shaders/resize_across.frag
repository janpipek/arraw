#version 440
#extension GL_GOOGLE_include_directive : require

// The horizontal half of the resize: Resample.cpp's horizontalPass.
//
// Mirrors it, and must change with it. The weights are not computed here: the
// CPU's axisWeights (ResampleWeights.cpp) evaluates the kernel, the centre
// mapping, the widening when shrinking and the normalisation in double, and
// packResizeWeights (GpuPlan.h) uploads the result, so the two cannot differ on
// a weight, and no source position is ever held in one float. What is left to
// this shader is the sum: premultiplied alpha, the rule against ringing below
// black, and the data the vertical pass needs.
//
// The vertical pass needs more than this pass's pixels, and a render has one
// output, so the pass is run once per plane (GpuResizeBlock::plane, ResizePlane
// in GpuPlan.h) over the same inputs, each run doing the whole loop and keeping
// what its plane holds. Three runs instead of one with multiple render targets:
// simpler, at the price of computing the window three times.
//
//   plane 0, sums:  the Pass::pixels of the CPU: premultiplied RGBA.
//   plane 1, low:   rgb is Pass::low, and a is Pass::translucent as 0 or 1.
//   plane 2, high:  rgb is Pass::high; a is unused.
//
// The CPU accumulates in double and this shader in float, so the sums, and with
// them the quotients the vertical pass takes, differ by float rounding; the
// tolerances the tests hold are measured on Vulkan (lavapipe) only.

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

    vec4 sum = vec4(0.0);
    vec4 negative = vec4(0.0);
    vec3 low = vec3(floatMax);
    vec3 high = vec3(-floatMax);
    float translucent = 0.0;
    for (int k = 0; k < count; ++k) {
        const float weight = tapWeight(pixel.x, k);
        // The image extends its edge pixel: clampIndex().
        const int index = clamp(first + k, 0, last);
        const vec4 colour = texelFetch(source, ivec2(index, pixel.y) + ivec2(plan.offset), 0);
        // The CPU's row[]: premultiplied, in float.
        const vec4 premultiplied = vec4(colour.rgb * colour.a, colour.a);
        sum += weight * premultiplied;
        negative = max(negative, vec4(lessThan(premultiplied, vec4(0.0))));
        if (colour.a < 1.0) {
            translucent = 1.0;
        }
        if (colour.a >= transparentBelow) {
            // The visible colour is the unpremultiplied one, as it was read.
            low = min(low, colour.rgb);
            high = max(high, colour.rgb);
        }
    }

    if (plan.plane == 0u) {
        fragColor = vec4(store(sum.r, negative.r), store(sum.g, negative.g),
                         store(sum.b, negative.b), store(sum.a, negative.a));
    } else if (plan.plane == 1u) {
        fragColor = vec4(low, translucent);
    } else {
        fragColor = vec4(high, 0.0);
    }
}
