// Luminance through the block's row of source-channel weights, shared by the
// Denoise and Presence passes. Included, never compiled on its own.
//
// Reads `plan.lumaRow`: include it after a uniform block named `plan` that
// has a `vec4 lumaRow` member (the Denoise and Presence blocks both do).

float lumaOf(vec3 colour) {
    return plan.lumaRow.x * colour.r + plan.lumaRow.y * colour.g + plan.lumaRow.z * colour.b;
}
