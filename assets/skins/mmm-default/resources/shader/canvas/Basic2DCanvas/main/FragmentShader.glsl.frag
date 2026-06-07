#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

// 绑定的纹理 (如果是纯色，CPU 端应绑定一张 1x1 的纯白纹理)
layout(binding = 0) uniform sampler2D texSampler;

void main() {
    // 顶点颜色与纹理采样颜色相乘
    outColor = fragColor * texture(texSampler, fragUV);
}
