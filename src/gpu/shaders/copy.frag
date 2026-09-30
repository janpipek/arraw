#version 440

// Copies its input sample for sample: the pass that proves a render round
// trips bit for bit before any photography is asked of one.

layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform sampler2D source;

void main() {
    // gl_FragCoord's row 0 is the texture's first row on every backend: the
    // origin QRhi's backends differ on is the framebuffer's, and each one
    // stores its first row where its own gl_FragCoord.y is smallest.
    fragColor = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
}
