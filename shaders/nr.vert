#version 440
// Passthrough vertex shader shared by the Colour Noise Reduction pre-passes
// (docs/adr/0032). Full-screen quad; clipCorr maps GL-style NDC to the backend's
// so every NR render target keeps the same orientation as the main pipeline's
// image texture (image.vert uses the same clipCorr), letting denoisedTex drop in
// for the raw slot. nr.vert is linked into every NR program (both blur passes and
// recombine), so its `nrbuf` block must list every member any of those fragment
// shaders declares — including recombine's trailing `strength` — or the program
// fails to link with a duplicate-block-type error. std140 keeps offsets stable.

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

layout(location = 0) out vec2 vUV;

layout(std140, binding = 0) uniform nrbuf {
    mat4  clipCorr;    // QRhi::clipSpaceCorrMatrix()
    vec2  invChroma;   // 1 / quarter-res chroma size, for blur tap offsets
    float sigma;       // Gaussian sigma in the quarter-res chroma's pixels
    int   radius;      // tap radius (capped)
    int   flipV;       // 1 when isYUpInFramebuffer(): see below
    float strength;    // unused here; declared so the block matches nr_recombine (std140)
} u;

void main() {
    gl_Position = u.clipCorr * vec4(aPos, 0.0, 1.0);
    // The NR targets are textures we render into and immediately re-sample. On a
    // Y-up framebuffer (OpenGL) texel row 0 sits at the bottom, the opposite of
    // the top-row-first uploaded image texture, so each pass would store its
    // result V-flipped and denoisedTex would not be a drop-in for the raw slot.
    // Mirroring the sampled V here makes every pass an identity sample again, so
    // the chain stays aligned with the uploaded orientation on all backends.
    vUV = vec2(aUV.x, u.flipV != 0 ? 1.0 - aUV.y : aUV.y);
}
