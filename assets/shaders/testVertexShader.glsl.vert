#version 450

// 接收来自顶点缓冲的数据
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(binding = 0) uniform UBO
{
    float time;
}ubo;



layout(location = 0) out vec4 fragColor;

void main()
{
    gl_Position = vec4(inPosition.xy, 0.0, 1.0);
    float brightness = (sin(ubo.time) + 1.0) * 0.5;
    fragColor = vec4(inColor.rgb * brightness, inColor.a);
}
