#version 440
// Spatial global adjustment context pass: extract source luminance at reduced
// resolution. The shared blur passes then turn this into the context sampled by
// Texture/Clarity/Dehaze in image.frag (docs/adr/0011).

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D srcTex;

const vec3 kLuma = vec3(0.2627, 0.6780, 0.0593); // Rec.2020

void main() {
    float y = dot(texture(srcTex, vUV).rgb, kLuma);
    fragColor = vec4(vec3(y), 1.0);
}
