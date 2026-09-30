#version 440

// The geometry resample: GeometryPlan.cpp's applyGeometry and sample.
//
// Mirrors them, and must change with them. The CPU composes crop, matrix and
// centring in double per pixel; here the CPU has already composed them into
// one affine map (GpuGeometryBlock in GpuPlan.h), split so that no absolute
// source position is ever held in one float, which at photo sizes would cost
// tens of 16-bit codes at a sharp edge. Only texelFetch reads the source:
// filtering is done by hand, in premultiplied alpha, exactly as sample() does
// it.
//
// Parity with the CPU, and the tolerances the tests hold, are measured on
// Vulkan (lavapipe) only.

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D source;

// The contract with GpuGeometryBlock: keep the members in step with it.
layout(std140, binding = 1) uniform Geometry {
    ivec2 originWhole;
    vec2 originFraction;
    vec2 columnStepHigh;
    vec2 rowStepHigh;
    vec2 columnStepLow;
    vec2 rowStepLow;
    uvec2 sourceSize;
    uvec2 outputSize;
} plan;

// Turns a source edge position along one axis, given as whole + fraction with
// fraction in [0, 1), into the texel below the centre coordinate and the
// distance above it, clamped as sample() clamps: the centre coordinate
// edge - 0.5 lies in [0, length - 1], and is exactly the last texel beyond it.
//
// The CPU also snaps to the nearest integer within 32 double epsilons of a
// texel centre. That is far below anything a float sees, and no snap is done
// here: a snap would turn a blend with a transparent neighbour into a copy of
// its colour. Exact copies need none, since quarter-turns land on a fraction
// of exactly one half.
void centreCoordinate(int whole, float fraction, uint length, out int below, out float above) {
    const float shifted = fraction - 0.5;
    const float shiftedFloor = floor(shifted);
    below = whole + int(shiftedFloor);
    above = shifted - shiftedFloor;
    const int last = int(length) - 1;
    if (below < 0) {
        below = 0;
        above = 0.0;
    } else if (below >= last) {
        below = last;
        above = 0.0;
    }
}

void main() {
    // gl_FragCoord is (x + 0.5, y + 0.5): the output pixel's centre. With the
    // steps' high parts (9 significant bits) the products are exact in float,
    // so they split into whole and fractional parts without error; see
    // GpuGeometryBlock for the bound.
    const vec2 columnHigh = gl_FragCoord.x * plan.columnStepHigh;
    const vec2 rowHigh = gl_FragCoord.y * plan.rowStepHigh;
    const vec2 columnWhole = floor(columnHigh);
    const vec2 rowWhole = floor(rowHigh);
    const ivec2 whole = plan.originWhole + ivec2(columnWhole) + ivec2(rowWhole);
    // Everything left is small: fractions and the low parts' products.
    vec2 fraction = plan.originFraction + (columnHigh - columnWhole) + (rowHigh - rowWhole) +
                    (gl_FragCoord.x * plan.columnStepLow + gl_FragCoord.y * plan.rowStepLow);
    const vec2 carry = floor(fraction);
    fraction -= carry;
    const ivec2 edgeWhole = whole + ivec2(carry);

    int x0;
    int y0;
    float dx;
    float dy;
    centreCoordinate(edgeWhole.x, fraction.x, plan.sourceSize.x, x0, dx);
    centreCoordinate(edgeWhole.y, fraction.y, plan.sourceSize.y, y0, dy);
    const int x1 = min(x0 + 1, int(plan.sourceSize.x) - 1);
    const int y1 = min(y0 + 1, int(plan.sourceSize.y) - 1);

    // On a texel centre the texel passes unchanged, colour of a transparent
    // pixel included, which the premultiplied blend below would zero.
    if (dx == 0.0 && dy == 0.0) {
        fragColor = texelFetch(source, ivec2(x0, y0), 0);
        return;
    }

    const vec4 neighbours[4] = vec4[4](
        texelFetch(source, ivec2(x0, y0), 0), texelFetch(source, ivec2(x1, y0), 0),
        texelFetch(source, ivec2(x0, y1), 0), texelFetch(source, ivec2(x1, y1), 0));
    const float weights[4] = float[4]((1.0 - dx) * (1.0 - dy), dx * (1.0 - dy),
                                      (1.0 - dx) * dy, dx * dy);
    vec3 colour = vec3(0.0);
    float alpha = 0.0;
    for (int index = 0; index < 4; ++index) {
        const float weight = weights[index] * neighbours[index].a;
        alpha += weight;
        colour += weight * neighbours[index].rgb;
    }
    fragColor = alpha <= 0.0 ? vec4(0.0) : vec4(colour / alpha, alpha);
}
