#pragma once

#include "audio/AudioManager.h"
#include "ui/IUIView.h"
#include "ui/layout/box/CLayBox.h"
#include <deque>
#include <string>

namespace MMM::UI
{

/**
 * @brief 音轨控制器 UI，绑定一个音频音轨（BGM 或 SFX 池）
 */
class AudioTrackControllerUI : virtual public IUIView
{
public:
    enum class TrackType { Main, Effect };

    /// @param trackId 音轨标识符
    /// @param trackName 显示名称
    /// @param type 音轨类型
    AudioTrackControllerUI(const std::string& trackId,
                           const std::string& trackName, TrackType type);

    ~AudioTrackControllerUI() override = default;

    void update(UIManager* sourceManager) override;

    void* getActualInstance() override { return this; }

    const std::string& getTrackId() const { return m_trackId; }

private:
    /// @brief 构建音量区域的 Clay 布局
    void buildVolumeSection(CLayVBox& parent, size_t& rowIndex,
                            float labelWidth, float& volume, bool& muted,
                            bool& changed);
    /// @brief 构建速度和音高区域的 Clay 布局
    void buildSpeedAndPitchSection(CLayVBox& parent, size_t& rowIndex,
                                   float labelWidth, float availWidgetW,
                                   float& speed, float& pitch, bool& changed);
    /// @brief 渲染 EQ 区域（保持原有 ImGui 直接绘制）
    void renderEQSection(bool& changed);
    /// @brief 构建音效预览区域的 Clay 布局
    void buildEffectPreviewSection(CLayVBox& parent, size_t& rowIndex,
                                   float labelWidth);
    /// @brief 构建波形/频谱按钮的 Clay 布局
    void buildAnalysisButtons(CLayVBox& parent, size_t& rowIndex,
                              UIManager* sourceManager);

    /// @brief 获取或创建一个行布局
    CLayHBox& getRow(size_t index);
    /// @brief 获取或创建一个段落容器布局
    CLayVBox& getSection(size_t index);
    /// @brief 测量标签文本的像素宽度
    float measureLabelWidth(const char* label);
    /// @brief 添加一个设置项行（标签 + 控件）
    void addSettingItem(CLayVBox& parent, size_t& rowIndex, const char* label,
                        float labelWidth, CLayBox::DrawFunc widget,
                        float heightOverride = 0.0f);

private:
    std::string m_trackId;
    std::string m_trackName;
    TrackType   m_type;

    Audio::EQPreset m_currentPreset{ Audio::EQPreset::None };

    /// @brief Clay 布局容器池
    CLayVBox             m_contentVBox;
    std::deque<CLayHBox> m_rows;
    std::deque<CLayVBox> m_sections;
};

}  // namespace MMM::UI
