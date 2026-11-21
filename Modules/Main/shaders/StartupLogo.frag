#version 450

// 内嵌 SVG 的 alpha 保留母版曲线，RGB 储存两笔各自的中性灰。
// 遮罩只上传一次，稳定帧只需一次纹理采样。
layout(set = 0, binding = 0) uniform sampler2D logoMask;
layout(location = 0) in vec2 uv;
// 输出直通 alpha，在当前主题底色上合成。
layout(location = 0) out vec4 color;
layout(push_constant) uniform Style {
    float time;      // 归一化时间，直到稳定态的第一帧已经绘制才放行资源同步。
    float gray;      // 深浅主题分别选择特效灰度，独立于 SVG 笔画。
} style;

// SVG 视口保留透明边界。钳制采样保证扩散效果不会从矩形另一侧绕回。
vec4 sampleLogo(vec2 point)
{
    return texture(logoMask, clamp(point, 0.0, 1.0));
}

void main()
{
    // 时间由界面按所绘制的帧推进，GPU 不单独读取系统时钟。
    float t = style.time;
    // 扩大绘制范围，给扫描轮廓和扩散环留白。中心点始终与 SVG 视口吻合。
    vec2 p = uv - 0.5;
    vec2 source = p * (630.0 / 512.0) + 0.5;
    // 完整采样同时给出当前笔画的灰度和轮廓覆盖率。
    vec4 logo = sampleLogo(source);
    float fill = logo.a;
    // 静止后直接绘制母版遮罩，所有临时高光、条纹和扩散环都归零。
    if (t >= 1.0) {
        color = logo;
        return;
    }

    // 斜向扫光经过后才显影，提前捕获轮廓外的带状光效。
    // 屏幕坐标的 y 轴朝下，负分量让扫光沿斜上方移动。
    float scan = dot(p, vec2(0.85, -0.52)) - mix(-0.48, 0.48, t);
    float solid = fill * (1.0 - smoothstep(-0.017, 0.045, scan));
    // 边缘能量取自相邻蒙版像素之差，因此光效沿母版轮廓分布。
    // 采样偏移以归一化纹理坐标表示，同一效果可随窗口 DPI 缩放。
    const float spread = 0.023;
    float nearby = max(max(sampleLogo(source + vec2(spread, 0)).a, sampleLogo(source - vec2(spread, 0)).a),
                       max(sampleLogo(source + vec2(0, spread)).a, sampleLogo(source - vec2(0, spread)).a));
    float edge = (nearby - fill) * exp(-abs(scan) * 23.0);
    // 两道扩散环以不同半径展开，末段统一衰出，避开稳定帧突变。
    float radius = length(p);
    float ring = exp(-abs(radius - mix(0.02, 0.49, t)) * 165.0);
    float echo = exp(-abs(radius - mix(0.01, 0.40, t)) * 210.0);
    // 衰出先于动画终点，确保最后一帧没有隐约残留的环或条带。
    float envelope = smoothstep(0.0, 0.14, t) * (1.0 - smoothstep(0.55, 0.96, t));
    // 扫描带内使用细条纹短暂显影，包络外恢复无纹理的实色。
    float stripe = 0.5 + 0.5 * cos(p.y * 390.0);
    float bloom = exp(-abs(scan) * 48.0) * fill * stripe;
    // 合并装饰和实色遮罩。上限由输出钳制，合成不会放大主题色值。
    float alpha = solid + envelope * (edge * 0.65 + ring * 0.22 + echo * 0.09 + bloom * 0.28);
    // 图形内保留两笔不同的明度，图形外沿与波纹使用统一的特效灰度。
    float shade = mix(style.gray, logo.r, fill);
    color = vec4(vec3(shade), clamp(alpha, 0.0, 1.0));
}
