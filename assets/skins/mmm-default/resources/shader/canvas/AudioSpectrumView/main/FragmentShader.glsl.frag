#version 450

// 插值输入必须与配套顶点着色器的 location 和类型完全一致。
layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

// binding 0 由频谱画布描述符集绑定当前纹理。
layout(binding = 0) uniform sampler2D texSampler;

void main() {
    // 顶点颜色同时承担色调与透明度，纹理提供局部采样强度。
    outColor = fragColor * texture(texSampler, fragUV);
}

// 纯色频谱图元仍需绑定兼容采样器，不能留下未定义描述符。
