#version 450
// 注意：这是一个用于全屏特效（如模糊、后处理）的顶点着色器。
// 它不使用外部传入的顶点数据，而是通过 gl_VertexIndex 自动生成一个覆盖全屏的三角形。

layout(location = 0) out vec2 fragUV;

// 与片元着色器共享同一 push constant 块，保持管线布局兼容。
layout(push_constant) uniform PushConstants {
    vec4 blurDirectionAndPasses;
    vec4 pad1;
    vec4 pad2;
    vec4 pad3;
} pcs;

void main() {
    // 生成覆盖全屏的三角形
    // 三个 gl_VertexIndex 通过位运算产生超出裁剪区的无接缝大三角形。
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // UV 使用同一坐标构造，光栅化后覆盖标准零到一采样范围。
    gl_Position = vec4(pos * 2.0f - 1.0f, 0.0f, 1.0f);
    fragUV = pos;
}
