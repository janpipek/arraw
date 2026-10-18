#version 440

// The lens pass: Lens.cpp's applyLens, with the Catmull-Rom kernel (ADR 050, section 9).
//
// Mirrors it, and must change with it. Each output pixel is mapped to its sensor position (the
// fill scale, then the distortion table), red and blue move from there by their own tables at the
// sensor radius, each channel is read with the 4x4 Catmull-Rom taps (edge pixels repeat beyond
// the image), and the vignetting gain at the sensor radius multiplies the colour.
//
// The CPU holds absolute positions in double. A float cannot, at photo sizes, without losing a
// few thousandths of a pixel, so nothing here is an absolute position: the output pixel's offset
// from the optical centre is exact (the centre comes as a whole and a fractional part), the
// displacement of the sample is that offset times a small factor, and the taps are the output
// pixel plus the whole part of the displacement, weighted by its fractional part. The tables hold
// the scales less one for the same reason (packLensTables in GpuPlan.h).
//
// Parity with the CPU, and the tolerance the tests hold, are measured on Vulkan (lavapipe) only.

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D source;

// The radial tables: node i of (distortion - 1, red - 1, blue - 1, vignetting gain).
layout(binding = 2) uniform sampler2D tables;

// The contract with GpuLensBlock: keep the members in step with it.
layout(std140, binding = 1) uniform Lens {
    vec2 centerWhole;
    vec2 centerFraction;
    vec2 radiusStep;
    uint distorts;
    uint colorSplit;
    uint dims;
    float fillGain;
    float inverseFill;
    float lastNode;
} plan;

// tableValue in LensCorrection.cpp, for all four tables at once.
vec4 tableAt(float radius) {
    const float position = clamp(radius, 0.0, 1.0) * plan.lastNode;
    const int index = min(int(position), int(plan.lastNode) - 1);
    const float fraction = position - float(index);
    const vec4 lower = texelFetch(tables, ivec2(index, 0), 0);
    const vec4 upper = texelFetch(tables, ivec2(index + 1, 0), 0);
    return lower + (upper - lower) * fraction;
}

// Kernel<CatmullRom>::weights.
vec4 catmullRom(float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5 * vec4(-t3 + 2.0 * t2 - t, 3.0 * t3 - 5.0 * t2 + 2.0, -3.0 * t3 + 4.0 * t2 + t,
                      t3 - t2);
}

// Reads all four channels at the position `base + t`, base being the pixel the taps start from
// (the kernel's first tap is one before it). A position on a whole pixel on an axis reads that
// line or column alone, as the CPU's read does. A channel that was non-negative in every tap and
// comes out below zero is the kernel's ringing, and is clamped at zero.
vec4 readAt(ivec2 base, vec2 t) {
    const ivec2 last = textureSize(source, 0) - 1;
    const bool wholeAcross = t.x == 0.0;
    const bool wholeDown = t.y == 0.0;
    if (wholeAcross && wholeDown) {
        return texelFetch(source, clamp(base, ivec2(0), last), 0);
    }
    const vec4 across = catmullRom(t.x);
    const vec4 down = catmullRom(t.y);
    const int columns = wholeAcross ? 1 : 4;
    const int rows = wholeDown ? 1 : 4;
    vec4 total = vec4(0.0);
    vec4 lowest = vec4(0.0);
    bool first = true;
    for (int row = 0; row < rows; ++row) {
        const int line = clamp(wholeDown ? base.y : base.y - 1 + row, 0, last.y);
        const float rowWeight = wholeDown ? 1.0 : down[row];
        vec4 sum = vec4(0.0);
        for (int tap = 0; tap < columns; ++tap) {
            const int column = clamp(wholeAcross ? base.x : base.x - 1 + tap, 0, last.x);
            const float weight = wholeAcross ? 1.0 : across[tap];
            const vec4 pixel = texelFetch(source, ivec2(column, line), 0);
            lowest = first ? pixel : min(lowest, pixel);
            first = false;
            sum += weight * pixel;
        }
        total += rowWeight * sum;
    }
    for (int channel = 0; channel < 4; ++channel) {
        if (total[channel] < 0.0 && lowest[channel] >= 0.0) {
            total[channel] = 0.0;
        }
    }
    return total;
}

void main() {
    // The output pixel's offset from the optical centre, in pixels; exact.
    const vec2 offset = (gl_FragCoord.xy - plan.centerWhole) - plan.centerFraction;
    const ivec2 pixel = ivec2(floor(gl_FragCoord.xy));

    // The displacement of the green sample from the output pixel: the sensor offset
    // `offset / fill * scale` less the offset itself, as `offset * k`.
    float k = 0.0;
    if (plan.distorts != 0u) {
        const float ideal = length(offset * plan.radiusStep) * plan.inverseFill;
        k = tableAt(ideal).x * plan.inverseFill + plan.fillGain;
    }
    const vec2 displacement = offset * k;
    // The sensor position's offset from the centre, and its radius.
    const vec2 sensor = offset * (1.0 + k);
    const float sensorRadius = length(sensor * plan.radiusStep);

    const vec2 whole = floor(displacement);
    vec4 colour = readAt(pixel + ivec2(whole), displacement - whole);
    if (plan.colorSplit != 0u) {
        const vec4 scales = tableAt(sensorRadius);
        const vec2 redShift = displacement + sensor * scales.y;
        const vec2 blueShift = displacement + sensor * scales.z;
        const vec2 redWhole = floor(redShift);
        const vec2 blueWhole = floor(blueShift);
        colour.r = readAt(pixel + ivec2(redWhole), redShift - redWhole).r;
        colour.b = readAt(pixel + ivec2(blueWhole), blueShift - blueWhole).b;
    }
    if (plan.dims != 0u) {
        colour.rgb *= tableAt(sensorRadius).w;
    }
    fragColor = colour;
}
