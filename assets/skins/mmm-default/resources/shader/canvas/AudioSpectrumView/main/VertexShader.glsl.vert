#version 450

// 输入布局与音频频谱画布生成的顶点格式保持一致。
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

// 正交投影矩阵由当前频谱视口在每次命令录制时提供。
layout(push_constant) uniform PushConstants {
    mat4 orthoProjection;
} pcs;

void main() {
    // 频谱几何已位于画布坐标，仅需投影到 Vulkan 裁剪空间。
    gl_Position = pcs.orthoProjection * vec4(inPosition, 1.0);
    // UV 和顶点颜色不做插值前变换，交由片元阶段组合纹理。
    fragUV = inUV;
    fragColor = inColor;
}

// location 编号属于 CPU 管线与片元着色器共同遵守的接口契约。
// push constant 布局变化时必须同步创建图形管线的范围大小。
