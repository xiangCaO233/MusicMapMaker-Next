#include "audio/AudioManager.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

namespace
{

/// @brief 使用小容差比较播放时间。
/// @param lhs 实际会话时间，单位秒。
/// @param rhs 预期时间，单位秒。
/// @return 绝对误差小于微秒时成功，不把设备延迟作为误差容限。
/// @note 这里只比较固定用例的有限值，不承担生产路径的输入规范化。
bool near(double lhs, double rhs)
{
    // 输入来自确定性控制或快照计算，不读取系统壁钟等待结果收敛。
    return std::abs(lhs - rhs) < 1e-6;
}

/// @brief 使用覆盖 16 位运行时增益量化误差的容差比较增益。
/// @param lhs 从运行时控制库读回的增益。
/// @param rhs 配置或命令中的期望增益。
/// @return 允许量化误差但仍可区分各测试设置值时为 true。
/// @note 比较增益而非分贝，不能对读回值再执行一次对数变换。
bool nearGain(float lhs, float rhs)
{
    // 不能使用时间比较的微秒级容差要求量化增益逐位一致。
    return std::abs(lhs - rhs) < 4e-5F;
}

/// @brief 验证自然结束标志会在下一次播放请求前先回到零点。
/// @return 已归零并消费重播标志，且无可激活时间线时未错误置播放态。
/// @note 不提供谱面和音频描述，隔离重播准备与真正激活的边界。
/// @note 重播标志是一次性意图，处理后即使激活未完成也不应继续沿用旧结束位置。
bool testFinishedTimelineRewindsBeforeActivation()
{
    // SessionContext 的默认停止态保留到激活成功，发出请求不等于音频已经可播放。
    // 检查的是命令同步处理结果，不以稍后 update 帮忙完成回零作为成功条件。
    // 旧位置设为 37.5 秒，区别于默认零值，确保回零确实执行。
    // 此用例没有手动 Seek，归零只能来自自然结束遗留标志。
    // 控制器只借用本用例上下文，声明顺序保证它先于上下文析构。
    // 活动身份允许进入播放请求逻辑，后台会话会提前被拒绝。
    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.isActiveSession                   = true;
    context.currentTime                       = 37.5;
    context.restartPlaybackAfterFinishPending = true;

    controller.handleCommand(MMM::Logic::CmdSetPlayState{ true });
    // 播放请求没有可用时间线也应先消费自然结束后的回零意图。
    // 同时断言位置、标志和播放态，避免只归零却错误宣告激活成功。
    if ( context.isPlaying || !near(context.currentTime, 0.0) ||
         context.restartPlaybackAfterFinishPending ) {
        XERROR("Finished timeline did not rewind before activation");
        // 任意一项失败都会拒绝整个场景，不以另外两项正确抵扣。
        return false;
    }
    return true;
}

/// @brief 验证拉伸器尾音期间暂停不会把视觉时间冻结到谱面末尾之外。
/// @return 会话时间与连续视觉时钟都冻结在五秒终点时为 true。
/// @note 只模拟尾部的时间状态，不创建真实拉伸器或等待设备尾音。
/// @note 暂停是低频命令，时钟取值断言不要求真实经过一秒。
bool testPauseClampsVisualClockToTimelineEnd()
{
    // 暂停后的可读位置应来自谱面终点，而不是尾音缓冲残留的视觉位置。
    // 这里只覆盖越过终点的情况，不推断负时间或无终点时的暂停策略。
    // 终点来自会话音频描述符，不依赖外部音频文件的可探测时长。
    // 先使逻辑处于播放态，命令才会走暂停转换而非重复设置暂停状态。
    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.isActiveSession                           = true;
    context.isPlaying                                 = true;
    context.audioTimelineDescriptor.m_chartEndSeconds = 5.0;
    context.playbackVisualClock.rebase(8.0, 100.0, 1.0, false);
    // 固定视觉时钟在八秒，显式制造超过五秒谱面终点的状态。
    // rebase 使用不推进的锚点，避免测试结果依赖命令处理时读取的真实时间。

    controller.handleCommand(MMM::Logic::CmdSetPlayState{ false });
    // 暂停既要限制 currentTime，也要重建内部时钟，不能只修改显示字段。
    // 额外读取未来时刻验证它确实冻结，不会在下一轮再次越过终点。
    if ( context.isPlaying || !near(context.currentTime, 5.0) ||
         !near(context.playbackVisualClock.currentTimeAt(101.0), 5.0) ) {
        // 101 秒只是读取参数，冻结锚点不应因它晚于原始观测而继续增长。
        XERROR("Pause preserved a visual time beyond the timeline end");
        return false;
    }
    return true;
}

/// @brief 验证零采样与多采样谱面都生成独立稳定描述符。
/// @return 空采样谱面有指纹，缺失资源事件保留，路由变化触发重新激活时为 true。
/// @note 项目根目录仅用于路径基准；本用例不创建该目录或写入资源文件。
/// @note 资源缺失诊断数量按事件计算，不把相同状态折叠成单一全局警告。
bool testZeroAndMultipleSampleDescriptors()
{
    // 三次重建使用同一上下文，让指纹和待激活标志的状态转换可以被观察。
    // 若每次都新建上下文，就无法证明路由变化使旧描述失效。
    // 使用局部项目与谱面模型，不切换 EditorEngine 当前项目。
    // 时间点使用模型毫秒，描述符输出使用秒，五百毫秒应成为半秒终点。
    MMM::Project project;
    project.m_projectRoot = "/tmp/mmm-session-timeline-test";

    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    MMM::Timing timing;
    // 空采样谱面仍含一个时间点，因此终点半秒不来自音频事件长度。
    timing.m_timestamp = 500.0;
    context.currentBeatmap->m_timings.push_back(timing);
    // 没有音频事件仍需完整描述，不能以空事件表作为“不存在谱面”的判断。
    if ( !MMM::Logic::SessionUtils::rebuildAudioTimelineDescriptor(context,
                                                                   &project) ||
         !context.audioTimelineDescriptor.m_events.empty() ||
         context.audioTimelineDescriptor.m_fingerprint.empty() ||
         !near(context.audioTimelineDescriptor.m_chartEndSeconds, 0.5) ) {
        XERROR("Zero-sample chart did not build an independent descriptor");
        return false;
    }

    const std::string emptyFingerprint =
        context.audioTimelineDescriptor.m_fingerprint;
    // 保存旧指纹值而非引用，下一次描述符重建会替换原字符串。
    // 缺失资源是既有夹具条件，测试验证描述与诊断，不要求实际解码成功。
    context.currentBeatmap->m_audioSamples.push_back(
        { .m_timestamp = 0.0, .m_audioResourceId = "missing-a" });
    context.currentBeatmap->m_audioSamples.push_back(
        { .m_timestamp = 250.0, .m_audioResourceId = "missing-b" });
    // 两个不同时间和资源身份的事件都需保留，不能因资源未找到而从描述中消失。
    context.isAudioTimelineDescriptorDirty = true;
    // 模型直接改动不会自动经过编辑命令标脏，所以夹具显式请求重建。
    if ( !MMM::Logic::SessionUtils::rebuildAudioTimelineDescriptor(context,
                                                                   &project) ||
         context.audioTimelineDescriptor.m_events.size() != 2U ||
         context.audioTimelineDescriptor.m_diagnostics.size() != 2U ||
         context.audioTimelineDescriptor.m_fingerprint == emptyFingerprint ) {
        XERROR("Multiple samples were not preserved in the descriptor");
        return false;
    }

    const std::string multiSampleFingerprint =
        context.audioTimelineDescriptor.m_fingerprint;
    // 清除激活标志再改变路由，避免沿用上一轮 true 而误判本轮触发成功。
    // 同一事件仅改轨号，身份与时间不变，指纹应仍能识别路由内容变化。
    context.isAudioTimelineActivationPending               = false;
    context.currentBeatmap->m_audioSamples.front().m_track = 128U;
    // 选择非默认轨号，避免默认零轨与未更新路由字段碰巧相等。
    context.isAudioTimelineDescriptorDirty = true;
    if ( !MMM::Logic::SessionUtils::rebuildAudioTimelineDescriptor(context,
                                                                   &project) ||
         context.audioTimelineDescriptor.m_fingerprint ==
             multiSampleFingerprint ||
         !context.isAudioTimelineActivationPending ||
         context.audioTimelineDescriptor.m_events.front().bgmTrackIndex !=
             128U ) {
        XERROR("BGM lane routing change did not request an audio reload");
        // 同时检查新轨号与激活意图，不能只靠指纹变化推断运行时会采用新路由。
        return false;
    }
    return true;
}

/// @brief 验证多标签切换仅在 Main 音轨同步指纹相同时继承播放态。
/// @return 同指纹继承源时间及播放，不同指纹使用目标自身时间且不恢复播放。
/// @note 直接测试决策值，不创建标签窗口或实际切换全局音频节点。
/// @note 输入字符串是夹具身份，不声称它们具有真实音频内容的哈希格式。
bool testTimelineSwitchUsesMainAudioSyncFingerprint()
{
    // 指纹比较决定能否共享推进状态，目标谱面的本地位置仍作为不同指纹时的后备。
    // 不在测试中读取资源字节重新求哈希，指纹构造属于描述符测试职责。
    // 两次调用除指纹外使用相同参数，避免时间或状态差异混淆身份比较结果。
    // 源位置八秒、目标位置三秒，明确区分继承与保留本地两种决策。
    const auto different = MMM::Logic::SessionUtils::resolveAudioTimelineSwitch(
        "timeline-a", "timeline-b", 8.0, 3.0, true, false, true);
    const auto matching = MMM::Logic::SessionUtils::resolveAudioTimelineSwitch(
        "timeline-a", "timeline-a", 8.0, 3.0, true, false, true);
    // 同指纹与不同指纹结果并存为局部值，后一次调用不应改写前一次决策。
    if ( !near(different.m_targetTime, 3.0) || different.m_resumePlayback ||
         !near(matching.m_targetTime, 8.0) || !matching.m_resumePlayback ) {
        XERROR("Timeline switch ignored the Main audio sync fingerprint");
        // 位置与恢复播放标志必须同时匹配，不能把“相同音轨”只用于其中一项。
        return false;
    }
    return true;
}

/// @brief 验证 transport 自然结束快照停止会话并武装下次重播。
/// @return 停止外推、保存十二秒位置并置下次重播标志时为 true。
/// @note 快照所属完整指纹与会话一致，避免走来源不匹配的拒绝分支。
/// @note 初始会话处于播放中，结束快照必须主动将它转为停止。
bool testNaturalFinishSnapshotArmsRestart()
{
    // 自然结束保留最后播放位置，归零留待下一次播放请求而非接收结束快照时执行。
    // 十二秒断言防止“正确设置重播标志”同时过早清零当前时间。
    // 指纹非空使会话具备待匹配的描述身份；空描述属于另一类拒绝场景。
    // 会话先处于播放态，快照同时给出 Stopped 和 finished 表达自然结束。
    // 采样率一千使 12000 帧对应十二秒，观测与读取时刻一致。
    MMM::Logic::SessionContext context;
    context.audioTimelineDescriptor.m_fingerprint = "timeline";
    context.isPlaying                             = true;
    const MMM::Audio::AudioTimelineClockSnapshot snapshot{
        // 这是值快照，不从全局音频线程抓取状态，测试不依赖回调调度时机。
        .positionFrame         = 12000,
        .steadyTimeNanoseconds = 100'000'000'000,
        .sampleRate            = 1000U,
        .playbackRate          = 1.0,
        .state            = MMM::Audio::AudioTimelinePlaybackState::Stopped,
        .epoch            = 1U,
        .seekSequence     = 2U,
        .playbackSequence = 2U,
        .sequence         = 2U,
        .finished         = true,
        .valid            = true,
    };
    if ( MMM::Logic::SessionUtils::applyAudioTimelineTransportSnapshot(
             // 调用结果与下面的副作用一起验收，不把布尔返回当成通用成功标记。
             context,
             "timeline",
             snapshot,
             100.0,
             context.lastConfig.settings.syncConfig) ||
         context.isPlaying || !context.restartPlaybackAfterFinishPending ||
         !near(context.currentTime, 12.0) ) {
        XERROR("Natural finish snapshot did not arm playback restart");
        // apply 返回 false 在此表示不再推进播放，而非此快照一定解析失败。
        // 同时检查会话副作用，区分正确结束与单纯拒绝输入。
        return false;
    }
    return true;
}

/// @brief 验证普通 Stopped 快照不会伪装成自然播放结束。
/// @return 会话停止但不设置下一次自动归零标志时为 true。
/// @note 与自然结束用例配对，突出 finished 原因标志而非只判断枚举状态。
/// @note 这里没有下发停止命令，验收的是接收快照后的会话状态处理。
bool testNonFinishedStopDoesNotArmRestart()
{
    // finished 为假是明确原因信息，不是缺失字段后猜测的默认结束状态。
    // 返回停止结果与未设置重播标志一起表达普通停止语义。
    // 使用独立上下文，不继承上一个自然结束用例已经设置的重播标志。
    // 四秒是手动停止后的普通位置，不要求在下一次播放时回到起点。
    // 保持有效快照和匹配指纹，让拒绝来源不成为用例通过的原因。
    MMM::Logic::SessionContext context;
    context.audioTimelineDescriptor.m_fingerprint = "timeline";
    context.isPlaying                             = true;
    const MMM::Audio::AudioTimelineClockSnapshot snapshot{
        .positionFrame         = 4000,
        .steadyTimeNanoseconds = 100'000'000'000,
        .sampleRate            = 1000U,
        .playbackRate          = 1.0,
        .state            = MMM::Audio::AudioTimelinePlaybackState::Stopped,
        .epoch            = 1U,
        .seekSequence     = 2U,
        .playbackSequence = 2U,
        .sequence         = 2U,
        .finished         = false,
        .valid            = true,
    };
    if ( MMM::Logic::SessionUtils::applyAudioTimelineTransportSnapshot(
             context,
             "timeline",
             snapshot,
             100.0,
             context.lastConfig.settings.syncConfig) ||
         context.isPlaying || context.restartPlaybackAfterFinishPending ) {
        XERROR("Non-finished stop incorrectly armed playback restart");
        // 普通停止不应获得自然结束的再次播放归零策略。
        // 本组只约束重播标志与停止态，不扩大为所有停止位置策略的验证。
        return false;
    }
    return true;
}

/// @brief 验证同步 follower 只沿用源画布壁钟，不以离散音频 block 重新校准。
/// @return 使用已重建源时钟外推到 10.1 秒并更新最后解析时刻时为 true。
/// @note 跟随目标的完整指纹与本地描述指纹不同，这是合法的跟随状态。
/// @note 跟随时长与传输快照版本故意不一致，验证本地已重建源锚点的优先级。
bool testFollowerUsesRebasedSourceClock()
{
    // 源锚点倍率为一，0.1 秒壁钟应直接增加 0.1 秒谱面时间。
    // 观测纪元刻意较大，跟随分支不能仅因其与本地不同就强行重新定位。
    // 音频快照仍声明 Playing，以允许跟随时钟按壁钟正常外推。
    // 本地锚点十秒与音频块两秒故意相距很远，便于识别是否错误采用块位置。
    // 传入的是源时间线完整指纹，不拿本地指纹冒充全局 transport 来源。
    MMM::Logic::SessionContext context;
    context.audioTimelineDescriptor.m_fingerprint = "follower-timeline";
    context.isAudioTimelineSyncFollower           = true;
    context.m_audioTimelineSyncSourceFingerprint  = "source-timeline";
    context.playbackVisualClock.rebase(10.0, 100.0, 1.0, true);
    // 跟随时钟先由源画布建立，这一步是只外推而不重新校准的前置条件。
    const MMM::Audio::AudioTimelineClockSnapshot snapshot{
        .positionFrame         = 2000,
        .steadyTimeNanoseconds = 100'000'000'000,
        .sampleRate            = 1000U,
        .playbackRate          = 1.0,
        .state            = MMM::Audio::AudioTimelinePlaybackState::Playing,
        .epoch            = 99U,
        .seekSequence     = 12U,
        .playbackSequence = 12U,
        .sequence         = 99U,
        .valid            = true,
    };
    if ( !MMM::Logic::SessionUtils::applyAudioTimelineTransportSnapshot(
             context,
             "source-timeline",
             snapshot,
             100.1,
             context.lastConfig.settings.syncConfig) ||
         !near(context.currentTime, 10.1) ||
         !near(context.playbackVisualClock.lastResolvedSteadyTime(), 100.1) ) {
        XERROR("Follower clock was overwritten by the discrete audio block");
        // 两秒块位置若覆盖十秒锚点会形成明显回退，不需要宽泛的单调性推断。
        // 既检查位置也检查最后解析壁钟，避免只返回旧位置而没有推进内部状态。
        return false;
    }
    return true;
}

/// @brief 验证同步跟随者不会读取已切换到其他源的全局 transport。
/// @return 来源完整指纹不匹配时取消跟随并复位本地连续时钟。
/// @note 不把主音轨同步指纹相同当作完整 transport 来源相同。
/// @note 使用有效快照验证身份拒绝，不能靠 invalid 标志使断言偶然通过。
bool testFollowerRejectsUnexpectedSourceTimeline()
{
    // 拒绝后清除来源字符串，使后续调用不能继续误认自己仍跟随旧 transport。
    // 此场景不自动选择另一来源，重新建立跟随关系由外层负责。
    // expected-source 与 other-source 只在调用参数中不同，未修改本地预期来源。
    // 先建立非空来源和有效时钟，后续断言才能证明拒绝动作确实清理了状态。
    // 输入音频快照本身合法，唯一拒绝原因应是来源已切到别的时间线。
    MMM::Logic::SessionContext context;
    context.audioTimelineDescriptor.m_fingerprint = "follower-timeline";
    context.isAudioTimelineSyncFollower           = true;
    context.m_audioTimelineSyncSourceFingerprint  = "expected-source";
    context.playbackVisualClock.rebase(10.0, 100.0, 1.0, true);
    const MMM::Audio::AudioTimelineClockSnapshot snapshot{
        .positionFrame         = 2000,
        .steadyTimeNanoseconds = 100'000'000'000,
        .sampleRate            = 1000U,
        .playbackRate          = 1.0,
        .state = MMM::Audio::AudioTimelinePlaybackState::Playing,
        .valid = true,
    };
    if ( MMM::Logic::SessionUtils::applyAudioTimelineTransportSnapshot(
             context,
             "other-source",
             snapshot,
             100.1,
             context.lastConfig.settings.syncConfig) ||
         context.isAudioTimelineSyncFollower ||
         !context.m_audioTimelineSyncSourceFingerprint.empty() ||
         context.playbackVisualClock.initialized() ) {
        XERROR("Follower accepted an unexpected source timeline");
        // 时钟未初始化是关键收尾条件，防止取消跟随后继续沿旧源时间外推。
        // 跟随标志、来源字符串与时钟初始化状态须一起复位，防止残余状态继续外推。
        return false;
    }
    return true;
}

/// @brief 验证后台会话不能启动或替换全局音频时间线。
/// @return 后台播放请求被忽略且本地时间不变时为 true。
/// @note 不为后台会话安装时间线，控制权检查应在尝试激活前生效。
/// @note 本地可见并不等于拥有全局 transport，活动身份是独立控制条件。
bool testBackgroundSessionCannotControlTransport()
{
    // 命令仍通过正式控制器入口发送，不直接修改状态绕过活动身份检查。
    // 断言不要求后台上下文析构或清空谱面，只要求无法夺取音频控制权。
    // 不在夹具中先设置播放态，确保请求不能将后台会话从停止变成播放。
    // 使用非零本地时间，以便检测错误的自动回零或激活准备副作用。
    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.isActiveSession = false;
    context.currentTime     = 8.25;

    controller.handleCommand(MMM::Logic::CmdSetPlayState{ true });
    if ( context.isPlaying || !near(context.currentTime, 8.25) ) {
        // 拒绝控制不等于关闭或重置后台谱面，已有本地时间必须保留。
        XERROR("Background session unexpectedly controlled audio transport");
        // 不发第二条命令补救，首条后台请求本身就必须没有这些副作用。
        return false;
    }
    return true;
}

/// @brief 验证后台会话不能改写全局玩家轨道音效增益。
/// @return 后台命令被忽略且活动会话命令生效时返回 true。
/// @note 修改进程内 AudioManager 控制值，结束前恢复原始增益。
/// @note 同一控制器从后台变为活动，验证授权条件而不是两个不同对象的初始化差异。
bool testBackgroundSessionCannotControlKeySoundGain()
{
    // 从 AudioManager 读回实际运行时槽值，不只检查命令对象中的目标字段。
    // 测试串行访问这个全局槽，不在本场景验证并发增益更新的竞争规则。
    auto&                   audio       = MMM::Audio::AudioManager::instance();
    constexpr std::uint32_t TRACK_INDEX = 7U;
    // 固定轨号用于读取同一运行时槽，不依赖打开谱面的轨数配置。
    const float originalGain = audio.getPlayerKeySoundTrackGain(TRACK_INDEX);
    // 保留实际原值，不在恢复时写固定单位增益覆盖既有进程配置。

    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.isActiveSession = false;
    controller.handleCommand(MMM::Logic::CmdSetKeySoundTrackGain{
        // 明确选择玩家区，避免与同编号草稿轨的控制槽混淆。
        .area       = MMM::Logic::KeySoundTrackArea::Player,
        .trackIndex = TRACK_INDEX,
        .gain       = 0.25F,
    });
    if ( !nearGain(audio.getPlayerKeySoundTrackGain(TRACK_INDEX),
                   originalGain) ) {
        audio.setPlayerKeySoundTrackGain(TRACK_INDEX, originalGain);
        // 即使后台检查失败也恢复全局值，不把失败用例的副作用留给后续测试。
        XERROR("Background session unexpectedly changed player track gain");
        return false;
    }

    context.isActiveSession = true;
    controller.handleCommand(MMM::Logic::CmdSetKeySoundTrackGain{
        .area       = MMM::Logic::KeySoundTrackArea::Player,
        .trackIndex = TRACK_INDEX,
        .gain       = 0.25F,
    });
    const bool applied =
        nearGain(audio.getPlayerKeySoundTrackGain(TRACK_INDEX), 0.25F);
    // 活动阶段复用相同目标值，后台和活动的唯一区别是会话控制权。
    // 先保存断言结果再恢复，不能恢复后再测而误读原始值。
    audio.setPlayerKeySoundTrackGain(TRACK_INDEX, originalGain);
    if ( !applied ) {
        XERROR("Active session did not change player track gain");
        return false;
    }
    return true;
}

/// @brief 验证草稿区 Key 音命令只允许活动会话修改对应运行时控制。
/// @return 后台命令被忽略，活动会话可修改草稿总控、逐轨增益和静音时返回 true。
/// @note 同时覆盖草稿区总控和单轨控制，二者不是同一个静音状态。
/// @note 草稿控制使用独立区域标识，不应误改同编号玩家轨控制。
bool testDraftKeySoundControlsRequireActiveSession()
{
    // 草稿总静音与逐轨静音分别取反，以便任何一个命令被漏处理都能被识别。
    // 改变静音不应改变既有轨道增益，故两类属性同时验收。
    auto&                   audio       = MMM::Audio::AudioManager::instance();
    constexpr std::uint32_t TRACK_INDEX = 9U;
    const float originalGain = audio.getDraftKeySoundTrackGain(TRACK_INDEX);
    // 增益不由静音状态派生，即使原轨已静音也仍需保留其独立增益。
    const bool originalTrackMuted =
        audio.isDraftKeySoundTrackMuted(TRACK_INDEX);
    const bool originalAreaMuted = audio.isDraftKeySoundAreaMuted();
    // 读取真实初值，使用取反值确保静音命令一定要求一次可观察变化。
    // 不假定其他测试或本机默认配置一定处于非静音状态。

    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.isActiveSession = false;
    controller.handleCommand(MMM::Logic::CmdSetKeySoundTrackGain{
        .area       = MMM::Logic::KeySoundTrackArea::Draft,
        .trackIndex = TRACK_INDEX,
        .gain       = 0.35F,
    });
    controller.handleCommand(MMM::Logic::CmdSetKeySoundTrackMute{
        .area       = MMM::Logic::KeySoundTrackArea::Draft,
        .trackIndex = TRACK_INDEX,
        .muted      = !originalTrackMuted,
    });
    controller.handleCommand(
        MMM::Logic::CmdSetDraftKeySoundAreaMute{ .muted = !originalAreaMuted });
    const bool backgroundIgnored =
        // 记录后台阶段结果后仍继续执行活动阶段，末尾统一恢复三项原值。
        nearGain(audio.getDraftKeySoundTrackGain(TRACK_INDEX), originalGain) &&
        audio.isDraftKeySoundTrackMuted(TRACK_INDEX) == originalTrackMuted &&
        audio.isDraftKeySoundAreaMuted() == originalAreaMuted;

    context.isActiveSession = true;
    controller.handleCommand(MMM::Logic::CmdSetKeySoundTrackGain{
        .area = MMM::Logic::KeySoundTrackArea::Draft,
        // 只切换活动身份，轨号和目标值保持与后台阶段相同。
        .trackIndex = TRACK_INDEX,
        .gain       = 0.35F,
    });
    controller.handleCommand(MMM::Logic::CmdSetKeySoundTrackMute{
        .area       = MMM::Logic::KeySoundTrackArea::Draft,
        .trackIndex = TRACK_INDEX,
        .muted      = !originalTrackMuted,
    });
    controller.handleCommand(
        MMM::Logic::CmdSetDraftKeySoundAreaMute{ .muted = !originalAreaMuted });
    const bool activeApplied =
        // 增益和两个静音层级必须全部生效，不能只检查其中一个控制入口。
        nearGain(audio.getDraftKeySoundTrackGain(TRACK_INDEX), 0.35F) &&
        audio.isDraftKeySoundTrackMuted(TRACK_INDEX) != originalTrackMuted &&
        audio.isDraftKeySoundAreaMuted() != originalAreaMuted;

    audio.setDraftKeySoundTrackGain(TRACK_INDEX, originalGain);
    // 恢复不走受活动身份限制的业务命令，直接还原测试前的运行时控制库状态。
    audio.setDraftKeySoundTrackMuted(TRACK_INDEX, originalTrackMuted);
    audio.setDraftKeySoundAreaMuted(originalAreaMuted);
    // 恢复三项后再判断汇总结果，避免任一失败分支绕过其他项的清理。
    if ( !backgroundIgnored || !activeApplied ) {
        XERROR(
            "Draft Key sound commands did not respect active-session "
            "ownership");
        return false;
    }
    return true;
}

/// @brief 验证无活动会话时配置更新仍会同步全局打击音效控制库。
/// @return 玩家总控和两个绑定类别均按配置更新时返回 true。
/// @note 配置入口与会话命令入口权限不同：无活动谱面也必须应用全局偏好。
/// @note 非有限与越界增益的处理结果同时约束配置存储和实际控制库。
bool testEditorConfigSynchronizesGlobalKeySoundControls()
{
    // 绑定与未绑定类别设置不同的启用状态和增益，暴露错误共用同一控制槽的问题。
    // 原配置恢复同样走生产入口，以同步恢复运行时控制而非只还原内存副本。
    auto&      engine         = MMM::Logic::EditorEngine::instance();
    auto&      audio          = MMM::Audio::AudioManager::instance();
    const auto originalConfig = engine.getEditorConfig();
    // 原配置保存为值副本，后面的 setEditorConfig 会更新引擎当前配置。

    auto updatedConfig = originalConfig;
    // 在完整副本上仅改变待测字段，不用默认新配置抹掉其他已有选项。
    updatedConfig.settings.sfxConfig.enableHitSfx        = false;
    updatedConfig.settings.sfxConfig.enableUnboundHitSfx = false;
    updatedConfig.settings.sfxConfig.unboundHitSfxGain   = 0.4F;
    updatedConfig.settings.sfxConfig.enableBoundHitSfx   = true;
    updatedConfig.settings.sfxConfig.boundHitSfxGain     = 1.6F;
    engine.setEditorConfig(updatedConfig);
    // 总控关闭不应阻止各绑定类别独立保存增益与静音设置。

    const bool synchronized =
        // enable 与 muted 极性相反，既检查关闭类别也检查仍启用的类别。
        audio.isPlayerKeySoundAreaMuted() &&
        audio.isKeySoundEffectGroupMuted(
            MMM::Audio::KeySoundEffectGroup::Unbound) &&
        nearGain(audio.getKeySoundEffectGroupGain(
                     MMM::Audio::KeySoundEffectGroup::Unbound),
                 0.4F) &&
        !audio.isKeySoundEffectGroupMuted(
            MMM::Audio::KeySoundEffectGroup::Bound) &&
        nearGain(audio.getKeySoundEffectGroupGain(
                     MMM::Audio::KeySoundEffectGroup::Bound),
                 1.6F);

    updatedConfig.settings.sfxConfig.unboundHitSfxGain =
        // 非有限值与过大有限值分别覆盖两条规范化路径。
        std::numeric_limits<float>::quiet_NaN();
    updatedConfig.settings.sfxConfig.boundHitSfxGain = 9.0F;
    engine.setEditorConfig(updatedConfig);
    const auto normalizedConfig = engine.getEditorConfig();
    // 同时验收规范化配置和运行时控制值，不能只在显示配置上修正而漏发音频更新。
    const bool normalized =
        // NaN 应归零，过大值应限制到二；不能把所有异常统一回退为单位增益。
        normalizedConfig.settings.sfxConfig.unboundHitSfxGain == 0.0F &&
        normalizedConfig.settings.sfxConfig.boundHitSfxGain == 2.0F &&
        nearGain(audio.getKeySoundEffectGroupGain(
                     MMM::Audio::KeySoundEffectGroup::Unbound),
                 0.0F) &&
        nearGain(audio.getKeySoundEffectGroupGain(
                     MMM::Audio::KeySoundEffectGroup::Bound),
                 2.0F);
    engine.setEditorConfig(originalConfig);
    // 在检查结果前恢复，正常失败返回也不能将 NaN 或极端测试增益留在全局配置。
    if ( !synchronized || !normalized ) {
        XERROR("Editor config did not synchronize global hit sound controls");
        return false;
    }
    return true;
}

/// @brief 验证工具栏配置回写不会覆盖由 AppConfig 直接维护的协作视野模式。
/// @return 调整分拍数后引擎与全局配置仍保留协作视野模式时返回 true。
/// @note 模拟工具栏持有旧配置副本后，另一个入口直接修改全局视野设置。
/// @note 测试调用配置入口而非真实工具栏控件，不涵盖 UI 绘制与事件路由。
bool testBeatDivisorUpdatePreservesCollaborationViewportRenderMode()
{
    // TrackEdge 是全局入口刚写入的新值，工具栏副本不应成为该字段的新权威来源。
    // 用例只证明视野模式保留，不把其他所有配置字段的合并策略一并纳入验收。
    auto&      engine               = MMM::Logic::EditorEngine::instance();
    auto&      appConfig            = MMM::Config::AppConfig::instance();
    const auto originalEngineConfig = engine.getEditorConfig();
    const auto originalGlobalMode =
        appConfig.getEditorSettings().collaborationViewportRenderMode;
    // 引擎副本与全局值分别保存，二者在测试开始时不必假定已经完全一致。

    appConfig.getEditorSettings().collaborationViewportRenderMode =
        MMM::Config::CollaborationViewportRenderMode::TrackEdge;
    auto toolbarConfig = originalEngineConfig;
    // 工具栏继续使用旧副本，正是本场景要覆盖的覆盖新全局设置风险。
    toolbarConfig.settings.beatDivisor =
        // 上限处减一，其余加一，确保修改有效且不制造越界分拍值。
        toolbarConfig.settings.beatDivisor == 64
            ? 63
            : toolbarConfig.settings.beatDivisor + 1;
    engine.setEditorConfig(toolbarConfig);
    // 分拍数确实发生变化，但不应借此把旧视野模式一并写回。

    const auto updatedEngineConfig = engine.getEditorConfig();
    // 读取更新后的副本，不能拿工具栏旧副本自证引擎已经保留全局模式。
    const bool preserved =
        // 引擎与 AppConfig 两侧均检查，防止只有一侧保留新模式而暂时看似正常。
        updatedEngineConfig.settings.collaborationViewportRenderMode ==
            MMM::Config::CollaborationViewportRenderMode::TrackEdge &&
        appConfig.getEditorSettings().collaborationViewportRenderMode ==
            MMM::Config::CollaborationViewportRenderMode::TrackEdge;

    appConfig.getEditorSettings().collaborationViewportRenderMode =
        originalGlobalMode;
    engine.setEditorConfig(originalEngineConfig);
    // 先还原全局模式再还原引擎配置，保持该字段既有的全局归属规则。
    if ( !preserved ) {
        XERROR("Beat divisor update reset collaboration viewport render mode");
        return false;
    }
    return true;
}

/// @brief 验证手动跳转取消自然结束后的自动回到开头。
/// @return 新定位保留在八秒且清除重播待办标志时为 true。
/// @note 用户明确定位优先于自然结束遗留的下次归零意图。
/// @note 本测试不启动播放，验证的是定位命令已取消后续自动归零安排。
bool testSeekCancelsPendingRestart()
{
    // 普通 CmdSeek 的默认非拖动语义适用于用户单次定位，不需要后续提交命令收尾。
    // 不先执行重播请求，让 pending 标志仍在时直接模拟用户手动定位。
    // 目标在二十秒范围内，避免边界钳制影响对手动定位优先级的判断。
    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.audioTimelineDescriptor.m_chartEndSeconds = 20.0;
    context.restartPlaybackAfterFinishPending         = true;

    controller.handleCommand(MMM::Logic::CmdSeek{ 8.0 });
    // 既不能保留重播标志等待下一次播放覆盖目标，也不能仅清标志却不改变位置。
    if ( context.restartPlaybackAfterFinishPending ||
         !near(context.currentTime, 8.0) ) {
        XERROR("Manual seek did not cancel pending playback restart");
        // 若只位置正确但标志未清，下一次播放仍可能覆盖用户选中的八秒。
        return false;
    }
    return true;
}

/// @brief 验证连续 Seek 仅在拖动期间保持联机视口延迟发布状态。
/// @return 预览命令置位且最终提交命令清除状态时返回 true。
/// @note 这里只检查会话的非阻塞预览状态，不发送联机数据或等待网络确认。
/// @note 最终提交与上一条预览具有相同时间，专门覆盖只变状态不变坐标的情况。
bool testSeekScrubStateEndsOnCommit()
{
    // 两次 isScrubbing=true 对应持续预览，最后 false 表示手势提交而非取消。
    // 场景没有固定时长等待，连续交互反馈必须在每条命令返回时即可观察。
    // 控制器连续处理三条命令，保留手势状态，不为每个目标重新创建上下文。
    // 两次拖动目标都处于合法时长内，测试即时反馈与最终提交而非时间钳制。
    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.audioTimelineDescriptor.m_chartEndSeconds = 20.0;

    controller.handleCommand(
        MMM::Logic::CmdSeek{ .time = 6.0, .isScrubbing = true });
    if ( !context.isSeekScrubbing || !near(context.currentTime, 6.0) ) {
        // 第一条预览命令必须立即改变本地位置，不能等拖动结束才统一应用。
        XERROR("Continuous seek did not enter local preview state");
        return false;
    }

    controller.handleCommand(
        MMM::Logic::CmdSeek{ .time = 9.0, .isScrubbing = true });
    if ( !context.isSeekScrubbing || !near(context.currentTime, 9.0) ) {
        // 第二条命令覆盖当前目标，不排队保留六秒状态延后播放。
        XERROR("Continuous seek did not update its local preview target");
        // 保持预览标志但位置不更新同样失败，不能以非阻塞状态代替即时反馈。
        return false;
    }

    controller.handleCommand(
        MMM::Logic::CmdSeek{ .time = 9.0, .isScrubbing = false });
    if ( context.isSeekScrubbing || !near(context.currentTime, 9.0) ) {
        // 最终位置不变也必须退出预览状态，不能因坐标相同而跳过提交语义。
        XERROR("Committed seek did not release viewport synchronization");
        // 提交后当前时间仍应保留最终预览值，不回退到手势开始前的位置。
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行复合时间线播放控制权与重播策略测试。
/// @return 所有既有场景通过返回零，首个失败短路返回一。
/// @note 断言覆盖控制状态和描述符，不代表真实设备发声或 UI 操作已验收。
/// @note 配置相关用例调用既有隔离测试环境，输出不写入谱面测试资源目录。
int main()
{
    // 涉及全局控制值的场景自行恢复原值，局部会话场景不共享编辑上下文。
    // 短路返回保留首个失败日志，不能从未执行的后续用例推断它们已经通过。
    return testZeroAndMultipleSampleDescriptors() &&
                   testTimelineSwitchUsesMainAudioSyncFingerprint() &&
                   testNaturalFinishSnapshotArmsRestart() &&
                   testNonFinishedStopDoesNotArmRestart() &&
                   testFollowerUsesRebasedSourceClock() &&
                   testFollowerRejectsUnexpectedSourceTimeline() &&
                   testFinishedTimelineRewindsBeforeActivation() &&
                   testPauseClampsVisualClockToTimelineEnd() &&
                   testBackgroundSessionCannotControlTransport() &&
                   testBackgroundSessionCannotControlKeySoundGain() &&
                   testDraftKeySoundControlsRequireActiveSession() &&
                   testEditorConfigSynchronizesGlobalKeySoundControls() &&
                   testBeatDivisorUpdatePreservesCollaborationViewportRenderMode() &&
                   testSeekCancelsPendingRestart() &&
                   testSeekScrubStateEndsOnCommit()
               ? 0
               : 1;
}
