#version 440
#extension GL_GOOGLE_include_directive : require

// One step of one base of the Presence context: Presence.cpp's reduction to
// the grid of log2 mean luminance, its opening by an octagon and the opening's
// reconstruction, and its Gaussian blur, kept above the opening, one step per
// render.
//
// Every function below mirrors the C++ of the same name in src/core/Presence.cpp,
// with the same arithmetic in the same order, and must change with it. The
// Presence block is the contract with GpuPresenceBlock in GpuPlan.h: same
// members, same order. The spatial weights come from the host; only log2 is
// the device's own.
//
// Parity with the CPU, and the tolerance the tests hold, are measured on
// Vulkan (lavapipe) only.

layout(location = 0) out vec4 fragColor;

// The source, or the step before's grid in r.
layout(binding = 0) uniform sampler2D source;

// The grid a step bounds its result by, in r: the opened grid a floor's last
// blur keeps above, or the cells a reconstruction step keeps below; the source
// stands in for every other step, and is not read then.
layout(binding = 2) uniform sampler2D limit;

layout(std140, binding = 1) uniform Presence {
    uint step;
    uint radius;
    uint reduction;
    uint window;
    vec4 lumaRow;
    uvec2 sourceSize;
    uvec2 gridSize;
    vec4 weights[17];
} plan;

// PresenceStep in GpuPlan.h.
const uint stepReduce = 0u;
const uint stepBlurAcross = 1u;
const uint stepBlurDown = 2u;
const uint stepMinimumAcross = 3u;
const uint stepMinimumDown = 4u;
const uint stepMaximumAcross = 5u;
const uint stepMaximumDown = 6u;
const uint stepBlurDownAboveOpening = 7u;
const uint stepReconstruct = 8u;
const uint stepMinimumDiagonal = 9u;
const uint stepMinimumAntidiagonal = 10u;
const uint stepMaximumDiagonal = 11u;
const uint stepMaximumAntidiagonal = 12u;

float weightAt(uint tap) {
    return plan.weights[tap / 4u][tap % 4u];
}

// presenceLuminanceFloor, presenceLuminanceCeiling and boundedLuminance.
#include "common/presence_bounds.glsl"

#include "common/luma_row.glsl"

// reducedGrid(): the log2 of the mean luminance of the block a cell covers.
float reduce(ivec2 cell) {
    const int reduction = int(plan.reduction);
    const ivec2 size = ivec2(plan.sourceSize);
    float sum = 0.0;
    float count = 0.0;
    for (int dy = 0; dy < reduction; ++dy) {
        const int y = cell.y * reduction + dy;
        if (y >= size.y) {
            break;
        }
        for (int dx = 0; dx < reduction; ++dx) {
            const int x = cell.x * reduction + dx;
            if (x >= size.x) {
                break;
            }
            sum += boundedLuminance(lumaOf(texelFetch(source, ivec2(x, y), 0).rgb));
            count += 1.0;
        }
    }
    return log2(sum / count);
}

// The grid at a cell, the coordinates clamped to it, as clampedCell().
float clampedAt(ivec2 at) {
    const ivec2 last = ivec2(plan.gridSize) - ivec2(1);
    return texelFetch(source, clamp(at, ivec2(0), last), 0).r;
}

// blurPass(): a normalised Gaussian along one axis, edges clamped.
float blur(ivec2 at, ivec2 axis) {
    float sum = clampedAt(at);
    float total = 1.0;
    for (uint tap = 1u; tap <= plan.radius; ++tap) {
        const float weight = weightAt(tap);
        const ivec2 offset = axis * int(tap);
        const float after = clampedAt(at + offset);
        const float before = clampedAt(at - offset);
        sum += weight * after + weight * before;
        total += 2.0 * weight;
    }
    return sum / total;
}

// extremumPass(): the window's minimum, or maximum, along one direction, each
// coordinate clamped to the grid; plan.window is this pass's half-width.
float extremum(ivec2 at, ivec2 axis, bool maximum) {
    float extreme = clampedAt(at);
    for (uint tap = 1u; tap <= plan.window; ++tap) {
        const ivec2 offset = axis * int(tap);
        const float after = clampedAt(at + offset);
        const float before = clampedAt(at - offset);
        extreme = maximum ? max(extreme, max(after, before)) : min(extreme, min(after, before));
    }
    return extreme;
}

// reconstructionPass(): the maximum over the 3x3 neighbourhood, edges clamped,
// never above the cell unopened.
float reconstruct(ivec2 at) {
    float extreme = clampedAt(at);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            extreme = max(extreme, clampedAt(at + ivec2(dx, dy)));
        }
    }
    return min(extreme, texelFetch(limit, at, 0).r);
}

void main() {
    // gl_FragCoord is the pixel's centre; its floor is the column and row.
    const ivec2 at = ivec2(floor(gl_FragCoord.xy));
    float value;
    if (plan.step == stepReduce) {
        value = reduce(at);
    } else if (plan.step == stepBlurAcross) {
        value = blur(at, ivec2(1, 0));
    } else if (plan.step == stepBlurDown) {
        value = blur(at, ivec2(0, 1));
    } else if (plan.step == stepMinimumAcross) {
        value = extremum(at, ivec2(1, 0), false);
    } else if (plan.step == stepMinimumDown) {
        value = extremum(at, ivec2(0, 1), false);
    } else if (plan.step == stepMaximumAcross) {
        value = extremum(at, ivec2(1, 0), true);
    } else if (plan.step == stepMaximumDown) {
        value = extremum(at, ivec2(0, 1), true);
    } else if (plan.step == stepMinimumDiagonal) {
        value = extremum(at, ivec2(1, 1), false);
    } else if (plan.step == stepMinimumAntidiagonal) {
        value = extremum(at, ivec2(1, -1), false);
    } else if (plan.step == stepMaximumDiagonal) {
        value = extremum(at, ivec2(1, 1), true);
    } else if (plan.step == stepMaximumAntidiagonal) {
        value = extremum(at, ivec2(1, -1), true);
    } else if (plan.step == stepReconstruct) {
        value = reconstruct(at);
    } else {
        // baseOf(): the blur never lowers a floor below its opening.
        value = max(blur(at, ivec2(0, 1)), texelFetch(limit, at, 0).r);
    }
    // Into a one-channel target where the device has one: only r is kept.
    fragColor = vec4(value, 0.0, 0.0, 1.0);
}
