#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

layout(push_constant) uniform PushConstants {
    vec4 blurDirectionAndPasses;
    vec4 pad1;
    vec4 pad2;
    vec4 pad3;
} pcs;

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0f - 1.0f, 0.0f, 1.0f);
    fragUV = pos;
    fragColor = vec4(1.0);
}
