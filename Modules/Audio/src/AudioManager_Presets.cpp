#include "audio/AudioManager.h"

#include "AudioManagerSoundEffectLoadingState.h"
#include "audio/KeySoundControl.h"
#include "audio/SoundEffectPool.h"
#include "config/AppConfig.h"
#include "config/AudioPresets.h"

namespace MMM::Audio
{
/// @brief 把已选预设发布到实际混音图。
/// @details 配置库负责选择与持久化，这里只处理运行时状态。
/// @warning 低频控制入口；会更新路由和扫描已加载池，不得逐帧调用。
void AudioManager::applyAudioPreset(bool applyPlayback)
{
    // AppConfig 已恢复选择，音频层不负责再次读磁盘或解释旧格式。
    // 这里只借用同一控制线程上的配置，不保留跨调用的容器引用。
    const auto& app      = Config::AppConfig::instance();
    const auto& settings = app.getEditorSettings();
    const auto& preset   = app.getAudioPresets().current();
    // 一次发布所有总线，避免逐字段 setter 反复保存不完整方案。
    // 基础音量与静音独立保存；静音不能把原音量破坏性改成零。
    // 四条总线的来源一致，防止切换到一半出现新旧方案混合。
    m_globalVolume            = settings.globalVolume;
    m_globalMuted             = settings.globalMuted;
    m_bgmGain                 = settings.bgmGain;
    m_bgmGainMuted            = settings.bgmGainMuted;
    m_sfxGain                 = settings.sfxGain;
    m_sfxGainMuted            = settings.sfxGainMuted;
    m_interactionSfxGain      = settings.interactionSfxGain;
    m_interactionSfxGainMuted = settings.interactionSfxGainMuted;
    // 键音分类使用常驻控制库，已播放的 voice 也能立即获得新增益。
    // enable 字段与音频控制库 muted 字段语义相反。
    // 整个分类关闭时仍保留用户设置的增益，重新开启后恢复。
    const auto& sfx = settings.sfxConfig;
    m_keySoundControls->setPlayerAreaMuted(!sfx.enableHitSfx);
    m_keySoundControls->setEffectGroupMuted(KeySoundEffectGroup::Unbound,
                                            !sfx.enableUnboundHitSfx);
    m_keySoundControls->setEffectGroupGain(KeySoundEffectGroup::Unbound,
                                           sfx.unboundHitSfxGain);
    m_keySoundControls->setEffectGroupMuted(KeySoundEffectGroup::Bound,
                                            !sfx.enableBoundHitSfx);
    m_keySoundControls->setEffectGroupGain(KeySoundEffectGroup::Bound,
                                           sfx.boundHitSfxGain);
    // 项目采样属于创作内容；客户端方案只覆盖皮肤常驻音效池。
    // 没有覆盖项时恢复默认，不能继承刚离开的预设的静音状态。
    // 尚未加载的池后续从 SFX 配置读取覆盖值，无需为切换预设强制加载。
    // 只扫描登记表，不触碰实体系统或执行文件探测。
    // 只借用稳定私有状态中的登记，不复制容器或延长资源所有权。
    for ( auto& [key, registration] : m_sfxLoading->m_registeredSoundEffects ) {
        if ( registration.m_usesProjectResourceConfig ) continue;
        const auto gain = sfx.permanentSfxVolumes.find(key);
        const auto mute = sfx.permanentSfxMutes.find(key);
        m_sfxMutes[key] = mute != sfx.permanentSfxMutes.end() && mute->second;
        if ( auto pool = m_sfxPools.find(key); pool != m_sfxPools.end() ) {
            pool->second->setVolume(gain != sfx.permanentSfxVolumes.end()
                                        ? gain->second
                                        : registration.m_defaultVolume);
        }
    }
    // 所有基础值落定后刷新有效增益，避免沿用旧分组乘积。
    refreshSFXEffectiveVolumes();
    updateSFXSyncSpeedRouting(sfx.hitSfxSyncSpeed);
    // 主轨参数使用与连续调节相同的增量入口，避免重复发布相同滤波链。
    applyMainTrackPreviewParameters();
    // 既有 setter 维护拉伸器 discontinuity 与音效调度边界。
    // 它们只改变试听，不把速度、音高烘焙进谱面文件。
    // 速度、音高和质量必须一起恢复，避免只恢复界面读数。
    // 设备后端继续沿用软件设置，不随耳机听音方案切换而重新枚举。
    // 活动会话可以延后速度更新，由播放控制器先收取旧倍率下的时钟。
    if ( applyPlayback ) {
        setPlaybackSpeed(preset.m_speed);
        setPlaybackPitch(preset.m_pitch);
    }
    setPlaybackQuality(static_cast<StretchQuality>(preset.m_quality));
}
/// @brief 仅更新主轨监听参数，保留音效池、PCM 调度和拉伸器状态。
/// @warning 用户连续调参路径；不执行文件操作、音效池遍历或全局路由重接。
void AudioManager::applyMainTrackPreviewParameters()
{
    const auto& app      = Config::AppConfig::instance();
    const auto& settings = app.getEditorSettings();
    const auto& preset   = app.getAudioPresets().current();
    // 主轨音量是总线标量，只刷新时间线与独立试听的有效增益。
    m_bgmGain      = settings.bgmGain;
    m_bgmGainMuted = settings.bgmGainMuted;
    refreshAudioTimelineVolume();
    refreshAuditionTrackVolume();
    // 切换频段类型才创建节点；拖动单频段不重新接线或替换解码缓存。
    const auto eq = static_cast<EQPreset>(preset.m_eqPreset);
    if ( getMainTrackEQPreset() != eq ) createMainTrackEQ(eq);
    // 段数由实际节点决定，损坏或旧版配置数组不能扩展运行时滤波器。
    // 缺少的元素使用中性值，多余元素留在配置中但不访问未知频段。
    // 比较运行时值而非另存影子副本，兼容其他入口切换或关闭 EQ。
    const auto bands = getMainTrackEQBandCount();
    for ( size_t i = 0; i < bands; ++i ) {
        const float gain =
            i < preset.m_eqGains.size() ? preset.m_eqGains[i] : 0.0F;
        const float q = i < preset.m_eqQs.size() ? preset.m_eqQs[i] : 1.414F;
        // ICE 单频段 setter 会发布完整滤波状态，只对实际改变的参数调用。
        // 这样拖动一个增益不会对全部十段或十五段重复构造两遍状态。
        if ( getMainTrackEQBandGain(i) != gain )
            setMainTrackEQBandGain(i, gain);
        if ( getMainTrackEQBandQ(i) != q ) setMainTrackEQBandQ(i, q);
    }
}
}  // namespace MMM::Audio
