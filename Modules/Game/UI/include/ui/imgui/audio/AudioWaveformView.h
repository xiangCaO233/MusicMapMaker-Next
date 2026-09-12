#pragma once

#include "ui/IUIView.h"
#include <memory>
#include <vector>

namespace ice
{
class GraphicEqualizer;
class AudioBuffer;
}  // namespace ice

namespace MMM::UI
{

/// @brief 展示活动音轨双声道波形包络和均衡器预览的 UI 视图。
/// @details 视图持有降采样后的全局缓存与当前视野切片；音频缓冲由该对象独占，
/// GraphicEqualizer 与音频管理器共享生命周期。
class AudioWaveformView : public IUIView
{
public:
    /// @brief 创建具名波形视图。
    /// @param name UIManager 注册和查找视图时使用的名称。
    AudioWaveformView(const std::string& name);
    /// @brief 在实现文件中释放需要完整类型的音频缓冲。
    ~AudioWaveformView() override;

    /// @brief 绘制活动音轨的当前视野波形。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning UI 热路径：窗口可见时每帧调用，重计算必须由缓存状态约束。
    void update(UIManager* sourceManager) override;

private:
    /// @brief 按当前视野刷新波形包络采样缓存。
    /// @param visualTime 当前全局视觉时间，单位为秒。
    /// @param duration 音频总时长，单位为秒。
    /// @param speed 播放速度，预留给后续采样策略。
    /// @param waveformVisualOffset 波形采样内容使用的专用偏移，单位为秒。
    void updateEnvelopes(double visualTime, double duration, double speed,
                         float waveformVisualOffset);
    /// @brief 将预览均衡器状态同步到波形处理链。
    void syncEQ();
    /// @brief 低频重建整段音频的固定分辨率包络缓存。
    /// @warning 可能遍历完整音频，仅允许在音轨或均衡器状态变化后调用。
    void fullRecalculate();

    /// @brief 与音频预览共享的均衡器实例，用于生成处理后波形。
    std::shared_ptr<ice::GraphicEqualizer> m_previewEQ;
    /// @brief 独占的均衡器处理结果缓冲。
    std::unique_ptr<ice::AudioBuffer> m_processBuffer;
    /// @brief 独占的原始音频采样缓冲。
    std::unique_ptr<ice::AudioBuffer> m_rawBuffer;

    /// @brief 固定时间分辨率的左声道最小值和最大值缓存。
    std::vector<float> m_cachedMinL, m_cachedMaxL;
    /// @brief 固定时间分辨率的右声道最小值和最大值缓存。
    std::vector<float> m_cachedMinR, m_cachedMaxR;
    /// @brief 全局包络缓存每秒保存的采样点数。
    double m_cachePointsPerSecond{ 100.0 };
    /// @brief 防止同一缓存重计算流程重入的状态标记。
    bool m_isCalculating{ false };

    /// @brief 当前视野各包络点对应的时间坐标。
    std::vector<double> m_times;
    /// @brief 当前视野的左声道最小值与最大值。
    std::vector<double> m_viewMinL, m_maxEnvelopeL;
    /// @brief 当前视野的右声道最小值与最大值。
    std::vector<double> m_viewMinR, m_maxEnvelopeR;

    /// @brief 波形时间轴缩放倍率。
    float m_zoom{ 1.0f };
    /// @brief 当前视野期望生成的包络点上限。
    int m_samplePoints{ 2000 };
};

}  // namespace MMM::UI
