#pragma once
#include "ui/workspace/AudioPlotTouch.h"

#include "ui/IUIView.h"
#include <memory>
#include <vector>

namespace MMM::UI
{
class WaveformAnalysis;

/// @brief 展示活动音轨双声道波形包络和均衡器预览的 UI 视图。
/// @details 只在 UI 线程保管当前视野与已接收的包络，后台任务拥有独立的
/// PCM 缓冲和 EQ 历史。
class AudioWaveformView : public IUIView
{
public:
    /// @brief 创建具名波形视图。
    /// @param name UIManager 注册和查找视图时使用的名称。
    AudioWaveformView(const std::string& name);
    /// @brief 请求取消后台任务并释放当前视野。
    ~AudioWaveformView() override;

    /// @brief 绘制活动音轨的当前视野波形。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning UI 热路径：窗口可见时每帧调用，重计算必须由缓存状态约束。
    void update(UIManager* sourceManager) override;

    /// @brief 关闭窗口时立即结束触控定位，不留待再次打开才提交。
    void setOpen(bool open) override;

private:
    /// @brief 两个声道共用固定大小的触控导航状态，不持有快照。
    AudioPlotTouch m_touchNavigation;

    /// @brief 按当前视野刷新波形包络采样缓存。
    /// @param visualTime 当前全局视觉时间，单位为秒。
    /// @param duration 音频总时长，单位为秒。
    /// @param speed 播放速度，预留给后续采样策略。
    /// @param waveformVisualOffset 波形采样内容使用的专用偏移，单位为秒。
    void updateEnvelopes(double visualTime, double duration, double speed,
                         float waveformVisualOffset);
    /// @brief 独占任务的观察状态与已接收包络，后台任务不访问该对象。
    std::unique_ptr<WaveformAnalysis> m_analysis;

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
