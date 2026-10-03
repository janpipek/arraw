#version 440

// One triangle that covers the whole viewport, from the vertex index alone:
// (-1, -1), (3, -1) and (-1, 3) in clip space. No vertex buffer, and no
// diagonal seam through the image as two triangles would leave. Every pass
// reads its own pixel from gl_FragCoord, so nothing is interpolated.
void main() {
    const vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
