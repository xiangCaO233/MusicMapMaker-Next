#version 450

// 接收来自顶点缓冲的数据
// 三个 location 必须与 Brush 顶点属性描述保持相同顺序和格式。
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

// 推送常量，接收正交投影矩阵
// 该矩阵把画布像素坐标直接映射到 Vulkan 裁剪空间。
layout(push_constant) uniform PushConstants {
    mat4 orthoProjection;
} pcs;

void main() {
    // 位置只应用一次正交投影，不在 Shader 内修改层深度。
    gl_Position = 
        pcs.orthoProjection * 
        vec4(inPosition, 1.0);
    // 颜色和 UV 原样传递，批处理器已完成每个顶点的属性展开。
    fragUV = inUV;
    fragColor = inColor;
}

// 修改接口 location 或 push constant 时必须同步 CPU 管线布局。
