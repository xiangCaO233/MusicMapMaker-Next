#version 450
layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    vec4 blurDirectionAndPasses; // x, y = direction, z = unused, w = intensity
    vec4 pad1;
    vec4 pad2;
    vec4 pad3;
} pcs;

void main() {
    vec4 color = vec4(0.0);
    vec2 blurDirection = pcs.blurDirectionAndPasses.xy;
    float intensity = pcs.blurDirectionAndPasses.w;
    
    // 如果方向不为0，说明是模糊Pass
    if (blurDirection.x != 0.0 || blurDirection.y != 0.0) {
        // 9-tap separable Gaussian blur
        float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
        
        color += texture(texSampler, fragUV) * weights[0];
        for(int i = 1; i < 5; ++i) {
            color += texture(texSampler, fragUV + blurDirection * float(i)) * weights[i];
            color += texture(texSampler, fragUV - blurDirection * float(i)) * weights[i];
        }
    } else {
        // 如果方向为0，说明是合成Pass，直接采样并应用强度
        color = texture(texSampler, fragUV);
        if (intensity > 0.0) {
            color *= intensity;
        }
    }
    
    outColor = color;
}
