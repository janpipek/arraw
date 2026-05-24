#version 330 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

uniform vec4  uTransform;  // (scaleX, scaleY, panX, panY)
uniform vec4  uCropRect;   // UV bounds: (left, top, right, bottom)
uniform float uRotation;   // degrees

out vec2 vUV;

void main() {
    vec2 p = aPos * uTransform.xy + uTransform.zw;
    gl_Position = vec4(p, 0.0, 1.0);

    // Map quad UV to the crop region, then rotate around crop centre
    vec2 uv = mix(uCropRect.xy, uCropRect.zw, aUV);
    vec2 center = (uCropRect.xy + uCropRect.zw) * 0.5;
    float rad = uRotation * 3.14159265 / 180.0;
    float c   = cos(rad);
    float s   = sin(rad);
    vec2 r    = uv - center;
    vUV = vec2(c * r.x - s * r.y, s * r.x + c * r.y) + center;
}
