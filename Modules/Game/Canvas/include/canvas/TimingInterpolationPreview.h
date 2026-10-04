#pragma once

#include "mmm/timing/TimingInterpolation.h"
#include <cstddef>
#include <optional>
#include <vector>

struct ImVec2;

namespace MMM::Canvas
{
/// @brief 预览顶点保留真实横轴比例与参数，不保存投影后的屏幕坐标。
struct TimingPreviewPoint {
    /// @brief 相对于整段的时间或自变量比例，放大后仍可追溯原位置。
    double m_progress{};
    /// @brief 原函数在此位置的绝对参数，不从稀疏折线反推。
    double m_value{};
};

/// @brief 插值编辑图的像素级预览缓存，密集区保留峰谷而非隔点抽取。
/// @details 原函数和真实输出样本合并后再压缩，黄色样本与蓝线使用同一真值。
/// @note 横轴缩放只改变预览，不修改时长、采样密度或任何谱面事件。
class TimingInterpolationPreview
{
public:
    /// @brief 仅输入、像素宽度或可见区间变化时重建，稳定帧直接复用。
    /// @param variableDomain 真时用于手绘参考图，以拍数或秒数均分自变量。
    /// @return 本次是否重建，用于验证稳定帧不会重复函数求值。
    /// @warning 每帧入口只比较缓存键；变化时执行有界采样，不持锁或遍历 ECS。
    bool update(const TimingInterpolation& curve, double startValue,
                float width, bool variableDomain = false);
    /// @brief 按光标横轴比例缩放可见区间，并保持光标所指时间不动。
    /// @warning 仅真实缩放输入调用，常量时间修改局部视图状态。
    void zoomAt(double position, double factor);
    /// @brief 横向拖动平移，位移以当前视野宽度为单位。
    /// @warning 仅真实拖动输入调用，不延迟本地视觉更新。
    void pan(double delta);
    /// @brief 双击恢复完整区间，不改模型或输出数量。
    void resetView();
    /// @brief 当前可见横轴区间，单位为整段比例。
    double viewStart() const { return m_viewStart; }
    /// @brief 当前可见横轴终点，至少比起点大一个视图最小跨度。
    double viewEnd() const { return m_viewEnd; }
    /// @brief 当前曲线的像素代表点，每列保留首点、峰、谷与末点。
    const std::vector<TimingPreviewPoint>& points() const { return m_points; }
    /// @brief 输出代表点均来自真实采样索引，不用均分区间伪造位置。
    const std::vector<TimingPreviewPoint>& samples() const { return m_samples; }
    /// @brief 本视野包含的真实输出样本数，用于解释密集点的压缩显示。
    std::size_t visibleSampleCount() const { return m_visibleSampleCount; }

private:
    /// @brief 重建时捕获不可变定义，稳定帧只借用以比较缓存键。
    /// @warning 仅输入变化复制共享所有权，保证旧缓存身份不会因地址复用误命中；
    /// 单纯观察指针无法保证函数或拍轴在下一次缓存比较时仍然存活。
    std::optional<TimingInterpolation> m_source;
    /// @brief 最近缓存起值，预设曲线修改起值后需要重新采样。
    double m_startValue{};
    /// @brief 像素列数有硬上限，超宽或异常窗口不能无限申请内存。
    int m_columns{};
    /// @brief 自变量参考图与秒域输出图具有不同映射，不能共用缓存键。
    bool m_variableDomain{};
    /// @brief 本地视野归一范围，独立于段落起止时间。
    double m_viewStart{}, m_viewEnd{ 1.0 };
    /// @brief 缓存建立时的视野，缩放和平移后下一帧重新生成局部预览。
    double m_cachedStart{ -1.0 }, m_cachedEnd{ -1.0 };
    /// @brief 蓝线代表点按横轴递增，无需在稳定绘制帧排序。
    std::vector<TimingPreviewPoint> m_points;
    /// @brief 黄色点保留输出索引对应的函数值，容量只与图宽有关。
    std::vector<TimingPreviewPoint> m_samples;
    /// @brief 压缩前真实落点数，区间外样本不计入显示提示。
    std::size_t m_visibleSampleCount{};
};

/// @brief 绘制已准备的缓存，纵轴上下界和颜色由两种编辑图分别提供。
/// @param sampleColor 为零时仅画参考曲线，不画输出样本。
/// @warning 每帧仅投影像素代表点；不求值、重建缓存、排序或复制共享指针。
void drawTimingInterpolationPreview(const TimingInterpolationPreview& preview,
                                    const ImVec2& origin, const ImVec2& size,
                                    double minimum, double maximum,
                                    unsigned int lineColor,
                                    unsigned int sampleColor = 0);
}  // namespace MMM::Canvas
