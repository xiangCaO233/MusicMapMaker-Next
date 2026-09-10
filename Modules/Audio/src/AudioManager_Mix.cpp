#include "audio/AudioManager.h"
#include "audio/AudioTimelineMixerNode.h"
#include "audio/SoundEffectPool.h"
#include "config/AppConfig.h"

#include <algorithm>

#include <ice/core/MixBus.hpp>
#include <ice/core/SourceNode.hpp>

namespace MMM::Audio
{
namespace
{
/// @brief 将项目声道模式转换为 IonCachyEngine 声道模式。
/// @param mode UI 与配置层使用的枚举值。
/// @return 含义对应的底层 MixBus 模式，未知值回退到 Stereo。
ice::MixBusChannelMode toIceChannelMode(MixerChannelMode mode)
{
    switch ( mode ) {
    case MixerChannelMode::MuteLeft: return ice::MixBusChannelMode::MuteLeft;
    case MixerChannelMode::MuteRight: return ice::MixBusChannelMode::MuteRight;
    case MixerChannelMode::CopyLeftToRight:
        return ice::MixBusChannelMode::CopyLeftToRight;
    case MixerChannelMode::CopyRightToLeft:
        return ice::MixBusChannelMode::CopyRightToLeft;
    case MixerChannelMode::Stereo: return ice::MixBusChannelMode::Stereo;
    }
    return ice::MixBusChannelMode::Stereo;
}

/// @brief 将 IonCachyEngine 声道模式转换为项目声道模式。
/// @param mode 当前底层 MixBus 声道模式。
/// @return 可由业务层展示和持久化的模式，未知值回退到 Stereo。
MixerChannelMode fromIceChannelMode(ice::MixBusChannelMode mode)
{
    switch ( mode ) {
    case ice::MixBusChannelMode::MuteLeft: return MixerChannelMode::MuteLeft;
    case ice::MixBusChannelMode::MuteRight: return MixerChannelMode::MuteRight;
    case ice::MixBusChannelMode::CopyLeftToRight:
        return MixerChannelMode::CopyLeftToRight;
    case ice::MixBusChannelMode::CopyRightToLeft:
        return MixerChannelMode::CopyRightToLeft;
    case ice::MixBusChannelMode::Stereo: return MixerChannelMode::Stereo;
    }
    return MixerChannelMode::Stereo;
}
}  // namespace

// 音量控制分为主时间线、试听、SFX 与交互 SFX 多层。所有 setter 先把公开输入
// 限制到 [0, 1]，再更新配置和对应运行节点；静音作为独立开关，不破坏用户保存
// 的原始音量。有效增益统一由各 refresh 函数组合，避免不同入口遗漏某一层。
//
// 各层组合关系如下：
//
// - 主时间线 = mainTrackVolume * globalVolume * bgmGain；
// - 独立试听 = auditionTrackVolume * globalVolume * bgmGain；
// - 普通音效 = 池音量 * globalVolume * sfxGain；
// - 交互音效在普通音效公式上额外乘 interactionSfxGain；
// - 任一对应静音开关启用时，有效增益直接为零；
// - 主 MixBus 声道模式只改变最终左右映射，不修改各来源增益。

/// @brief 设置复合时间线主增益并立即应用。
/// @param volume 目标音量。
void AudioManager::setMainTrackVolume(float volume)
{
    m_mainTrackVolume = std::clamp(volume, 0.0f, 1.0f);
    refreshAudioTimelineVolume();
}

/// @brief 获取复合时间线主增益。
/// @return 主音轨音量。
float AudioManager::getMainTrackVolume() const
{
    return m_mainTrackVolume;
}

/// @brief 设置复合时间线主静音状态并立即应用。
/// @param muted 是否静音。
void AudioManager::setMainTrackMute(bool muted)
{
    m_mainTrackMuted = muted;
    refreshAudioTimelineVolume();
}

/// @brief 获取复合时间线主静音状态。
/// @return 静音时返回 true。
bool AudioManager::isMainTrackMuted() const
{
    return m_mainTrackMuted;
}

/// @brief 按试听轨道配置、全局音量和 BGM 总线增益刷新独立试听源音量。
///
/// 试听源是资源浏览使用的独立 SourceNode，不经过复合时间线主增益，但仍属于
/// BGM 分类并服从应用全局音量。节点未创建时保留管理器状态，加载后再应用。
void AudioManager::refreshAuditionTrackVolume()
{
    if ( !m_auditionSource ) {
        return;
    }

    // 试听音轨属于 BGM 区域，因此同时受试听、全局和 BGM 三层增益控制。
    float effectiveVolume = m_auditionTrackVolume * m_globalVolume * m_bgmGain;
    if ( m_auditionTrackMuted || m_globalMuted || m_bgmGainMuted ) {
        effectiveVolume = 0.0f;
    }
    m_auditionSource->setvolume(effectiveVolume);
}

/// @brief 按主增益、全局音量和 BGM 总线状态刷新复合时间线音量。
///
/// 只向 MixerNode 发布一个最终线性增益，避免音频回调重复读取多个 UI 状态。
/// 时间线未加载时不做节点操作，成员值仍会在后续加载流程中使用。
void AudioManager::refreshAudioTimelineVolume()
{
    if ( !m_audioTimelineNode ) {
        return;
    }

    // 静音优先于乘积，但保留三层原始值供解除静音时恢复。
    float effectiveVolume = m_mainTrackVolume * m_globalVolume * m_bgmGain;
    if ( m_mainTrackMuted || m_globalMuted || m_bgmGainMuted ) {
        effectiveVolume = 0.0F;
    }
    m_audioTimelineNode->setMasterGain(effectiveVolume);
}

/// @brief 设置全局音量、保存配置并刷新所有轨道有效音量。
/// @param volume 目标全局音量。
///
/// 这是跨 BGM、试听和 SFX 的低频控制入口，包含配置磁盘写入，禁止从逐帧
/// 渲染或音频回调调用。传入异常范围会钳位到公开配置允许的区间。
void AudioManager::setGlobalVolume(float volume)
{
    m_globalVolume = std::clamp(volume, 0.0f, 1.0f);

    // 全局音量是用户偏好，修改后立即持久化并传播到全部播放区域。
    auto& settings        = Config::AppConfig::instance().getEditorSettings();
    settings.globalVolume = m_globalVolume;
    Config::AppConfig::instance().save();

    // 三类节点分别缓存或读取有效增益，需要在同一控制操作中全部刷新。
    refreshAudioTimelineVolume();
    refreshAuditionTrackVolume();
    refreshSFXEffectiveVolumes();
}

/// @brief 设置全局静音状态并刷新所有轨道有效音量。
/// @param muted 是否静音。
///
/// 复用 setGlobalVolume 传播现有音量，使所有分类走相同刷新顺序；静音字段先
/// 写入配置对象，因此 setGlobalVolume 保存的是一份包含两者的新快照。
void AudioManager::setGlobalMute(bool muted)
{
    m_globalMuted = muted;

    // 静音与音量分开保存，解除静音时仍恢复原来的比例。
    auto& settings       = Config::AppConfig::instance().getEditorSettings();
    settings.globalMuted = m_globalMuted;
    Config::AppConfig::instance().save();

    // 复用全局音量入口统一刷新三类节点，并再次保存一致配置快照。
    setGlobalVolume(m_globalVolume);
}

/// @brief 获取全局是否静音。
/// @return 静音时返回 true。
bool AudioManager::isGlobalMuted() const
{
    return m_globalMuted;
}

/// @brief 获取主混音输出左声道电平。
/// @return 左声道电平。
float AudioManager::getOutputLevelL() const
{
    if ( m_mainMixer ) return m_mainMixer->get_left_level();
    return 0.0f;
}

/// @brief 获取主混音输出右声道电平。
/// @return 右声道电平。
float AudioManager::getOutputLevelR() const
{
    if ( m_mainMixer ) return m_mainMixer->get_right_level();
    return 0.0f;
}

/// @brief 获取主音轨左声道实时电平。
/// @return 左声道电平。
float AudioManager::getMainTrackLevelL() const
{
    if ( m_audioTimelineNode ) return m_audioTimelineNode->leftLevel();
    return 0.0f;
}

/// @brief 获取主音轨右声道实时电平。
/// @return 右声道电平。
float AudioManager::getMainTrackLevelR() const
{
    if ( m_audioTimelineNode ) return m_audioTimelineNode->rightLevel();
    return 0.0f;
}

/// @brief 获取指定音效池左声道实时电平。
/// @param key 音效池标识。
/// @return 左声道电平。
float AudioManager::getSFXPoolLevelL(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        if ( auto mixer = it->second->getMixer() ) {
            return mixer->get_left_level();
        }
    }
    return 0.0f;
}

/// @brief 获取指定音效池右声道实时电平。
/// @param key 音效池标识。
/// @return 右声道电平。
float AudioManager::getSFXPoolLevelR(const std::string& key) const
{
    auto it = m_sfxPools.find(key);
    if ( it != m_sfxPools.end() ) {
        if ( auto mixer = it->second->getMixer() ) {
            return mixer->get_right_level();
        }
    }
    return 0.0f;
}

/// @brief 获取全局音量。
/// @return 当前全局音量。
float AudioManager::getGlobalVolume() const
{
    return m_globalVolume;
}

/// @brief 刷新所有音效池当前播放节点的有效音量。
///
/// getSFXEffectiveGain 负责区分普通与交互池，getSFXPoolMute 合并各层静音状态。
/// 遍历只在用户修改控制项时发生，不进入音频或逻辑逐帧热路径。
void AudioManager::refreshSFXEffectiveVolumes()
{
    // 低频控制路径遍历池；实时播放只读取池内已发布的有效音量。
    for ( auto& [key, pool] : m_sfxPools ) {
        pool->updateEffectiveVolume(getSFXEffectiveGain(key),
                                    getSFXPoolMute(key));
    }
}

/// @brief 设置主混音器左声道静音。
/// @param muted 是否静音。
void AudioManager::setMainMixerLeftMute(bool muted)
{
    if ( m_mainMixer ) {
        m_mainMixer->set_mute_left(muted);
    }
}

/// @brief 获取主混音器左声道是否静音。
/// @return 静音时返回 true。
bool AudioManager::isMainMixerLeftMuted() const
{
    if ( m_mainMixer ) {
        return m_mainMixer->is_mute_left();
    }
    return false;
}

/// @brief 设置主混音器右声道静音。
/// @param muted 是否静音。
void AudioManager::setMainMixerRightMute(bool muted)
{
    if ( m_mainMixer ) {
        m_mainMixer->set_mute_right(muted);
    }
}

/// @brief 获取主混音器右声道是否静音。
/// @return 静音时返回 true。
bool AudioManager::isMainMixerRightMuted() const
{
    if ( m_mainMixer ) {
        return m_mainMixer->is_mute_right();
    }
    return false;
}

/// @brief 设置主混音器双声道输出模式。
/// @param mode 目标静音或声道复制策略。
void AudioManager::setMainMixerChannelMode(MixerChannelMode mode)
{
    if ( m_mainMixer ) {
        m_mainMixer->set_channel_mode(toIceChannelMode(mode));
    }
}

/// @brief 获取主混音器双声道输出模式。
/// @return 混音器未初始化时返回 Stereo。
MixerChannelMode AudioManager::getMainMixerChannelMode() const
{
    if ( m_mainMixer ) {
        return fromIceChannelMode(m_mainMixer->get_channel_mode());
    }
    return MixerChannelMode::Stereo;
}

/// @brief 设置 BGM 全局增益、保存配置并刷新主音轨音量。
/// @param gain 目标增益。
///
/// BGM 分类包含复合时间线和资源试听，两条路由都必须在同次配置变更后刷新。
void AudioManager::setBGMGain(float gain)
{
    // BGM 总线增益同时作用于谱面时间线与独立试听源。
    m_bgmGain = std::clamp(gain, 0.0f, 1.0f);

    auto& settings   = Config::AppConfig::instance().getEditorSettings();
    settings.bgmGain = m_bgmGain;
    Config::AppConfig::instance().save();

    // 时间线和试听节点各自计算有效增益，二者都需要刷新。
    setMainTrackVolume(m_mainTrackVolume);
    refreshAuditionTrackVolume();
}

/// @brief 获取 BGM 全局增益。
/// @return 当前 BGM 增益。
float AudioManager::getBGMGain() const
{
    return m_bgmGain;
}

/// @brief 设置 BGM 增益静音状态并刷新主音轨音量。
/// @param muted 是否静音。
void AudioManager::setBGMGainMute(bool muted)
{
    m_bgmGainMuted = muted;

    auto& settings        = Config::AppConfig::instance().getEditorSettings();
    settings.bgmGainMuted = m_bgmGainMuted;
    Config::AppConfig::instance().save();

    setMainTrackVolume(m_mainTrackVolume);
    refreshAuditionTrackVolume();
}

/// @brief 获取 BGM 增益是否静音。
/// @return 静音时返回 true。
bool AudioManager::isBGMGainMuted() const
{
    return m_bgmGainMuted;
}

/// @brief 设置 SFX 全局增益，按需保存配置并刷新音效音量。
/// @param gain 目标增益。
/// @param persist 是否立即写入用户配置。
///
/// persist=false 用于从已有配置初始化运行状态；无论是否持久化，活跃池都应
/// 立即取得新有效增益，以免 UI 值与实际播放不一致。
void AudioManager::setSFXGain(float gain, bool persist)
{
    m_sfxGain = std::clamp(gain, 0.0f, 1.0f);

    // 初始化恢复配置时可关闭 persist，避免无意义地把同一值立即写回磁盘。
    if ( persist ) {
        auto& settings   = Config::AppConfig::instance().getEditorSettings();
        settings.sfxGain = m_sfxGain;
        Config::AppConfig::instance().save();
    }

    refreshSFXEffectiveVolumes();
}

/// @brief 获取 SFX 全局增益。
/// @return 当前 SFX 增益。
float AudioManager::getSFXGain() const
{
    return m_sfxGain;
}

/// @brief 设置 SFX 增益静音状态并刷新音效音量。
/// @param muted 是否静音。
void AudioManager::setSFXGainMute(bool muted)
{
    m_sfxGainMuted = muted;

    auto& settings        = Config::AppConfig::instance().getEditorSettings();
    settings.sfxGainMuted = m_sfxGainMuted;
    Config::AppConfig::instance().save();

    refreshSFXEffectiveVolumes();
}

/// @brief 获取 SFX 增益是否静音。
/// @return 静音时返回 true。
bool AudioManager::isSFXGainMuted() const
{
    return m_sfxGainMuted;
}

/// @brief 设置交互音效全局增益、保存配置并刷新音效音量。
/// @param gain 目标增益。
void AudioManager::setInteractionSFXGain(float gain)
{
    // 交互音效是 SFX 总线内的附加分类增益，不影响谱面打击音效。
    m_interactionSfxGain = std::clamp(gain, 0.0f, 1.0f);

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    settings.interactionSfxGain = m_interactionSfxGain;
    Config::AppConfig::instance().save();

    refreshSFXEffectiveVolumes();
}

/// @brief 获取交互音效全局增益。
/// @return 当前交互音效增益。
float AudioManager::getInteractionSFXGain() const
{
    return m_interactionSfxGain;
}

/// @brief 设置交互音效增益静音状态并刷新音效音量。
/// @param muted 是否静音。
void AudioManager::setInteractionSFXGainMute(bool muted)
{
    m_interactionSfxGainMuted = muted;

    auto& settings = Config::AppConfig::instance().getEditorSettings();
    settings.interactionSfxGainMuted = m_interactionSfxGainMuted;
    Config::AppConfig::instance().save();

    refreshSFXEffectiveVolumes();
}

/// @brief 获取交互音效增益是否静音。
/// @return 静音时返回 true。
bool AudioManager::isInteractionSFXGainMuted() const
{
    return m_interactionSfxGainMuted;
}

}  // namespace MMM::Audio
