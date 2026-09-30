#version 440

// The geometry resample: GeometryPlan.cpp's applyGeometry and sample.
//
// Mirrors them, and must change with them. The CPU composes crop, matrix and
// centring in double per pixel; here the CPU has already composed them into
// one affine map (GpuGeometryBlock in GpuPlan.h), so a pixel costs one
// multiply-add per axis. Only texelFetch reads the source: filtering is done
// by hand, in premultiplied alpha, exactly as sample() does it.

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D source;

// The contract with GpuGeometryBlock: keep the members in step with it.
layout(std140, binding = 1) uniform Geometry {
    vec2 origin;
    vec2 columnStep;
    vec2 rowStep;
    uvec2 sourceSize;
    uvec2 outputSize;
} plan;

// Float epsilon: the spacing of floats above 1 is this, so the spacing near
// a coordinate of magnitude len is about len * this.
const float floatEpsilon = 1.1920929e-7;

// Turns a source edge position along one axis into a texel-centre coordinate.
//
// The CPU snaps to the nearest integer when within 32 double epsilons of the
// source length, to absorb the rounding of its own composition. Here the
// composition is done in double before packing, so what is left is the
// rounding of the float origin and steps and of one multiply-add: about an
// ulp of the length. One float epsilon of the length is that tolerance. No
// more is spent because every snapped distance is an error against the CPU
// (4 epsilons measured a 4.9e-4 pixel worst on a 1000 pixel ramp against
// 2.4e-4 with one). Exact copies need no snap: quarter-turns have integral
// steps and half-integral origins, which a float holds exactly.
float centreCoordinate(float edge, uint length) {
    const float last = float(length) - 1.0;
    float value = clamp(edge - 0.5, 0.0, last);
    const float nearest = round(value);
    if (abs(value - nearest) <= floatEpsilon * float(length)) {
        value = nearest;
    }
    return value;
}

void main() {
    // gl_FragCoord is (x + 0.5, y + 0.5): the output pixel's centre.
    const vec2 edge = plan.origin + gl_FragCoord.x * plan.columnStep +
                      gl_FragCoord.y * plan.rowStep;
    const float x = centreCoordinate(edge.x, plan.sourceSize.x);
    const float y = centreCoordinate(edge.y, plan.sourceSize.y);
    const int x0 = int(floor(x));
    const int y0 = int(floor(y));
    const int x1 = min(x0 + 1, int(plan.sourceSize.x) - 1);
    const int y1 = min(y0 + 1, int(plan.sourceSize.y) - 1);
    const float dx = x - float(x0);
    const float dy = y - float(y0);

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
