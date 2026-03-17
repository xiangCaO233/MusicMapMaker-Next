#version 450

// 接收来自顶点缓冲的数据
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(binding = 0) uniform UBO
{
    float time;
}ubo;

// RGB 转 HSV 的快捷函数
vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0 / 3.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

// HSV 转 RGB 的快捷函数
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

layout(location = 0) out vec4 fragColor;

void main()
{
    gl_Position = vec4(inPosition.xy, 0.0, 1.0);
    // 1. 将插值后的颜色转为 HSV
    vec3 hsv = rgb2hsv(inColor.rgb);

    // 2. 让色相随时间旋转 (0.5 是速度)
    hsv.x = fract(hsv.x + ubo.time * 0.2);
    
    // 3. 转回 RGB
    vec3 rgb = hsv2rgb(hsv);

    // 4. Gamma 纠正（针对 UNORM 交换链）
    fragColor = vec4(pow(rgb, vec3(1.0/2.2)), inColor.a);
}
