#version 450

// 一个三角形覆盖动态视口，启动窗口不需要专用顶点缓冲。
layout(location = 0) out vec2 uv;

void main()
{
    // 顶点在右侧和下侧越界，裁剪后覆盖整块 Logo 区域且没有对角拼缝。
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // Vulkan 正向视口的左上角对应 (-1, -1)。
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
