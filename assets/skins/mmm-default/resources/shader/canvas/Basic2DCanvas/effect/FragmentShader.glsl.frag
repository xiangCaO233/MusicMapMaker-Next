#version 450
layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

// 64 字节块与其他画布管线共用布局；当前仅 xy 和 w 有效。
layout(push_constant) uniform PushConstants {
    vec4 blurDirectionAndPasses; // x, y = direction, z = unused, w = intensity
    vec4 pad1;
    vec4 pad2;
    vec4 pad3;
} pcs;

void main() {
    // 所有分支最终写入同一累加颜色，避免未初始化片元输出。
    vec4 color = vec4(0.0);
    vec2 blurDirection = pcs.blurDirectionAndPasses.xy;
    float intensity = pcs.blurDirectionAndPasses.w;
    
    // 如果方向不为0，说明是模糊Pass
    if (blurDirection.x != 0.0 || blurDirection.y != 0.0) {
        // 五个对称权重构成中心一点加两侧各四点的 9-tap 可分离高斯核。
        float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
        
        // 中心纹理只采样一次，两侧偏移共享相同权重。
        color += texture(texSampler, fragUV) * weights[0];
        for(int i = 1; i < 5; ++i) {
            // blurDirection 已由 CPU 按当前纹理像素尺寸归一化。
            color += texture(texSampler, fragUV + blurDirection * float(i)) * weights[i];
            color += texture(texSampler, fragUV - blurDirection * float(i)) * weights[i];
        }
    } else {
        // 如果方向为0，说明是合成Pass，直接采样并应用强度
        // 合成阶段不重复模糊，只调整已生成效果纹理的亮度。
        color = texture(texSampler, fragUV);
        if (intensity > 0.0) {
            // 非正强度保留原采样，避免零值意外隐藏效果层。
            color *= intensity;
        }
    }
    
    // 颜色值保持浮点范围，最终钳制和混合由目标附件格式处理。
    outColor = color;
}

// 改变采样次数或权重时需重新评估两阶段模糊的 GPU 带宽成本。
// push constant 的四个 vec4 保持 16 字节对齐，不能单独压缩字段。
// 该 Shader 不执行纹理边界判断，采样器地址模式必须由管线正确配置。
