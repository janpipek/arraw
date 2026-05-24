#version 330 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

uniform vec4  uTransform;  // (scaleX, scaleY, panX, panY)
uniform float uRotation;   // degrees

out vec2 vUV;

void main() {
    vec2 p = aPos * uTransform.xy + uTransform.zw;
    gl_Position = vec4(p, 0.0, 1.0);

    // Rotate UV around image centre (0.5, 0.5)
    float rad = uRotation * 3.14159265 / 180.0;
    float c   = cos(rad);
    float s   = sin(rad);
    vec2  uv  = aUV - 0.5;
    vUV = vec2(c * uv.x - s * uv.y, s * uv.x + c * uv.y) + 0.5;
}
