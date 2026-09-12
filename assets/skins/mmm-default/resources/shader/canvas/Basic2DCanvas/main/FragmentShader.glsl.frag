#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

// 绑定的纹理 (如果是纯色，CPU 端应绑定一张 1x1 的纯白纹理)
// 描述符始终有效可避免在纯色批次中引入 Shader 分支。
layout(binding = 0) uniform sampler2D texSampler;

void main() {
    // 顶点颜色与纹理采样颜色相乘
    // 顶点 Alpha 和纹理 Alpha 共同决定现有混合管线的覆盖率。
    outColor = fragColor * texture(texSampler, fragUV);
}
