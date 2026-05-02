#pragma once

#include "audio/AudioManager.h"
#include "ui/IUIView.h"
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

    const std::string& getTrackId() const { return m_trackId; }

private:
    void renderVolumeSection(float& volume, bool& muted, bool& changed);
    void renderSpeedAndPitchSection(float& speed, float& pitch, bool& changed);
    void renderEQSection(bool& changed);
    void renderEffectPreviewSection();

private:
    std::string m_trackId;
    std::string m_trackName;
    TrackType   m_type;

    Audio::EQPreset m_currentPreset{ Audio::EQPreset::None };
};

}  // namespace MMM::UI
