#version 330 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

uniform vec4  uTransform;  // (scaleX, scaleY, panX, panY)
uniform vec4  uCropRect;   // UV bounds: (left, top, right, bottom)
uniform float uRotation;   // degrees
uniform float uAspect;     // crop width / height in pixels (for isotropic rotation)

out vec2 vUV;

void main() {
    vec2 p = aPos * uTransform.xy + uTransform.zw;
    gl_Position = vec4(p, 0.0, 1.0);

    // Map quad UV to the crop region, rotate around image centre (fixed pivot)
    vec2 uv = mix(uCropRect.xy, uCropRect.zw, aUV);
    vec2 center = vec2(0.5, 0.5);
    vec2 d = uv - center;
    d.x *= uAspect;
    float rad = uRotation * 3.14159265 / 180.0;
    float c   = cos(rad);
    float s   = sin(rad);
    vec2 r = vec2(c * d.x - s * d.y, s * d.x + c * d.y);
    r.x /= uAspect;
    vUV = r + center;
}
