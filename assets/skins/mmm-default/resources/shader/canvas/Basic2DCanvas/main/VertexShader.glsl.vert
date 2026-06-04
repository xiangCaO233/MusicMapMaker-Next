#version 450

// 接收来自顶点缓冲的数据
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

// 推送常量，接收正交投影矩阵
layout(push_constant) uniform PushConstants {
    mat4 orthoProjection;
} pcs;

void main() {
    gl_Position = 
        pcs.orthoProjection * 
        vec4(inPosition, 1.0);
    fragUV = inUV;
    fragColor = inColor;
}
