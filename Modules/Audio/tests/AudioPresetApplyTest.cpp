#include "audio/AudioManager.h"
#include "audio/KeySoundTypes.h"
#include "config/AppConfig.h"
#include "log/colorful-log.h"
#include <cmath>

/// @brief 验证预设实际发布到混音状态和 EQ，而非只改配置界面。
/// @details 不打开操作系统设备，避免 CI 机器是否有声卡影响参数验证。
/// EQ 节点仍执行真实创建与参数更新，可以检查实际段数及滤波参数。
/// 两次应用使用相反设置，覆盖旧方案状态被清除的分支。
/// 本测试不测量实际声音输出延迟，也不代替跨平台听音验收。
/// @note 核心验证边界如下：
/// 总音量、音乐、效果与交互总线分别断言，避免只验证最终乘积。
/// Key 分类查询实时控制库，确认已播放 voice 所读取的控制值发生变化。
/// 均衡器验证真实节点段数、增益与 Q，覆盖标量和数组的映射。
/// 拉伸器的请求状态覆盖速度、半音与质量，不只测试其中一项。
/// 第二次应用将静音解除并关闭 EQ，验证清除旧状态的方向。
/// 未初始化后端时仍必须保存控制值，后续 init 会恢复到完整音频图。
/// 不创建音频文件或项目，因此测试不会改变用户的资源配置。
/// 参数测试只覆盖控制契约，听感与硬件延迟仍需实际设备验证。
int main()
{
    // 单例配置先重置，其他测试进程的个人选择不参与本次断言。
    // 这里不调用 save，任何预设值都仅存在于测试进程内存。
    auto& app = MMM::Config::AppConfig::instance();
    app.reset();
    auto& preset = app.getAudioPresets().current();
    // 为每条总线指定不同值，防止参数被错误地接到相邻总线仍通过测试。
    preset.m_volume              = 0.3F;
    preset.m_music               = 0.4F;
    preset.m_effect              = 0.5F;
    preset.m_interaction         = 0.6F;
    preset.m_effectMuted         = true;
    preset.m_sfx.boundHitSfxGain = 0.2F;
    // 使用非中性效果器参数，默认值无法偶然满足断言。
    preset.m_eqPreset = 1;
    preset.m_eqGains.assign(10, -6);
    preset.m_eqQs.assign(10, 2);
    preset.m_speed   = 0.75;
    preset.m_pitch   = -3;
    preset.m_quality = 3;
    MMM::Config::applyAudioPreset(preset, app.getEditorSettings());
    auto& audio = MMM::Audio::AudioManager::instance();
    audio.applyAudioPreset();
    // 查询运行时接口，不读取刚写入的配置字段作为验证依据。
    bool valid = audio.getGlobalVolume() == 0.3F &&
                 audio.getBGMGain() == 0.4F && audio.getSFXGain() == 0.5F &&
                 audio.getInteractionSFXGain() == 0.6F;
    // Key 控制库以 1/32768 量化增益，比较允许一个存储量化单位。
    // 此容差远小于 UI 的一个百分点，不会掩盖错误步进或总线绑定。
    valid &= audio.isSFXGainMuted() &&
             std::abs(audio.getKeySoundEffectGroupGain(
                          MMM::Audio::KeySoundEffectGroup::Bound) -
                      0.2F) < 1.0F / 32768.0F;
    // 查询中间频段，避免只设置第一个频段的实现意外通过。
    // 段数与参数一起检查，错误布局不能凭同名 EQ 类型掩盖。
    valid &= audio.getMainTrackEQBandCount() == 10 &&
             std::abs(audio.getMainTrackEQBandGain(3) + 6) < 0.001F;
    valid &= std::abs(audio.getMainTrackEQBandQ(3) - 2) < 0.001F;
    valid &= audio.getPlaybackSpeed() == 0.75 &&
             audio.getPlaybackPitch() == -3 &&
             audio.getPlaybackQuality() ==
                 MMM::Audio::AudioManager::StretchQuality::Best;
    // 失败时输出运行时各层值，便于区分配置未应用与 DSP 参数错误。
    if ( !valid ) {
        XERROR(
            "Preset state: buses {} {} {} {}, mute {}, key {}, EQ {} gain {} Q "
            "{}, speed {} pitch {} quality {}",
            audio.getGlobalVolume(),
            audio.getBGMGain(),
            audio.getSFXGain(),
            audio.getInteractionSFXGain(),
            audio.isSFXGainMuted(),
            audio.getKeySoundEffectGroupGain(
                MMM::Audio::KeySoundEffectGroup::Bound),
            audio.getMainTrackEQBandCount(),
            audio.getMainTrackEQBandGain(3),
            audio.getMainTrackEQBandQ(3),
            audio.getPlaybackSpeed(),
            audio.getPlaybackPitch(),
            static_cast<int>(audio.getPlaybackQuality()));
    }
    // 连续主轨调参只能更新 BGM 和 EQ，不能顺带把其他预设字段重新应用。
    // 故意令配置里的 Key 增益、全局音量与变速参数不同于当前运行值。
    app.getEditorSettings().globalVolume              = 0.9F;
    app.getEditorSettings().bgmGain                   = 0.7F;
    app.getEditorSettings().sfxConfig.boundHitSfxGain = 0.8F;
    preset.m_speed                                    = 1.5;
    preset.m_pitch                                    = 5;
    preset.m_quality                                  = 0;
    // 配置与运行时故意不一致，使误用完整预设入口可被直接检测。
    // 循环模拟一次连续拖动的多个变化边沿，不依赖系统时钟或睡眠。
    // 本用例不将控制参数正确性推断为真实播放帧率或硬件音频延迟。
    for ( int step = 0; step < 20; ++step ) {
        // 同类型 EQ 的单频段更新应立即生效，不能等到松手或重新播放。
        // 每轮采用不同值，避免仅验证最后一次重复设置造成的空操作。
        const float gain    = -5.0F + step * 0.1F;
        preset.m_eqGains[3] = gain;
        audio.applyMainTrackPreviewParameters();
        valid &= std::abs(audio.getMainTrackEQBandGain(3) - gain) < 0.001F;
        valid &= audio.getMainTrackEQBandCount() == 10 &&
                 audio.getGlobalVolume() == 0.3F && audio.getBGMGain() == 0.7F;
        // 拉伸器质量和速度与本次 EQ 操作无关，不能被完整预设应用覆盖。
        valid &= audio.getPlaybackSpeed() == 0.75 &&
                 audio.getPlaybackPitch() == -3 &&
                 audio.getPlaybackQuality() ==
                     MMM::Audio::AudioManager::StretchQuality::Best;
        valid &= std::abs(audio.getKeySoundEffectGroupGain(
                              MMM::Audio::KeySoundEffectGroup::Bound) -
                          0.2F) < 1.0F / 32768.0F;
    }
    // 关闭 EQ 必须拆除节点，不能只隐藏控件后继续处理声音。
    // 保留已设置的数组，仅关闭布局，验证开关而不是空数组导致旁路。
    // 与解除静音同时执行，覆盖一次应用处理多个不同状态的能力。
    preset.m_eqPreset    = 0;
    preset.m_effectMuted = false;
    MMM::Config::applyAudioPreset(preset, app.getEditorSettings());
    audio.applyAudioPreset();
    valid &= !audio.isMainTrackEQEnabled() && !audio.isSFXGainMuted();
    // 生命周期由单例管理，不在测试中启动后台播放器或卸载用户资源。
    // 汇总前后两次应用结果，清除阶段成功不能掩盖初次应用失败。
    return valid ? 0 : 1;
}
