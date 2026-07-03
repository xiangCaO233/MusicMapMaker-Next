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

class AudioWaveformView : public IUIView
{
public:
    AudioWaveformView(const std::string& name);
    ~AudioWaveformView() override;

    void update(UIManager* sourceManager) override;

private:
    /// @brief 按当前视野刷新波形包络采样缓存。
    /// @param visualTime 当前全局视觉时间，单位为秒。
    /// @param duration 音频总时长，单位为秒。
    /// @param speed 播放速度，预留给后续采样策略。
    /// @param waveformVisualOffset 波形内容使用的最终视觉偏移，单位为秒。
    void updateEnvelopes(double visualTime, double duration, double speed,
                         float waveformVisualOffset);
    void syncEQ();
    void fullRecalculate();  // 新增：全局重计算

    std::shared_ptr<ice::GraphicEqualizer> m_previewEQ;
    std::unique_ptr<ice::AudioBuffer>      m_processBuffer;
    std::unique_ptr<ice::AudioBuffer>      m_rawBuffer;

    // 全局缓存数据 (分辨率固定，例如每秒 100 个点)
    std::vector<float> m_cachedMinL, m_cachedMaxL;
    std::vector<float> m_cachedMinR, m_cachedMaxR;
    double             m_cachePointsPerSecond{ 100.0 };
    bool               m_isCalculating{ false };

    // 当前视图渲染数据
    std::vector<double> m_times;
    std::vector<double> m_viewMinL, m_maxEnvelopeL;  // 保持变量名兼容或重命名
    std::vector<double> m_viewMinR, m_maxEnvelopeR;

    float m_zoom{ 1.0f };
    int   m_samplePoints{ 2000 };
};

}  // namespace MMM::UI
