#include "logic/audio/PlaybackVisualClock.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"

#include <cmath>
#include <cstdint>

namespace
{

/// @brief 秒单位结果的绝对误差容限，不是线程调度或真实音频设备的延迟预算。
constexpr double EPSILON = 1.0e-9;

/// @brief 构造可由假壁钟确定性驱动的音频时钟快照。
/// @param positionFrame 音频观测位置，固定采样率下每帧对应一毫秒。
/// @param observationTime 音频观测的单调时钟秒数，与 update
/// 的壁钟使用同一基准。
/// @param state 播放状态；不启动实际音频设备。
/// @param sequence 用于区分音频观测的版本号。
/// @param epoch 传输纪元，同时作为初始控制纪元。
/// @param seekSequence 定位请求版本，默认同时标记为已确认。
/// @param playbackSequence 播放控制请求版本，默认同时标记为已确认。
/// @param playbackRate 谱面时间相对壁钟的推进倍率。
/// @return 完整有效的值快照，测试可再局部修改字段模拟未确认请求。
/// @note 控制纪元与传输纪元相等表示未模拟控制后的额外跳转。
/// @note 调度代数固定为一，替换调度的场景必须显式改变该字段。
/// @note finished 默认为假，停止态不会自动等同于自然结束。
MMM::Audio::AudioTimelineClockSnapshot makeSnapshot(
    std::int64_t positionFrame, double observationTime,
    MMM::Audio::AudioTimelinePlaybackState state, std::uint64_t sequence,
    std::uint64_t epoch = 1U, std::uint64_t seekSequence = 2U,
    std::uint64_t playbackSequence = 2U, double playbackRate = 1.0)
{
    // 使用 1000 Hz 便于从整数帧直接读出毫秒位置，避免依赖设备采样率。
    // 这是算术测试夹具，不表示真实引擎必须支持或使用这个采样率。
    // 请求与确认默认一致；各控制测试只改需要制造差异的版本字段。
    // 秒转整数纳秒使用四舍五入，避免小数二进制表示导致时间戳少一纳秒。
    // 调用参数是小范围固定值，不用此夹具测试纳秒表示范围的溢出行为。
    return {
        .positionFrame         = positionFrame,
        .steadyTimeNanoseconds = static_cast<std::int64_t>(
            std::llround(observationTime * 1'000'000'000.0)),
        .sampleRate              = 1000U,
        .playbackRate            = playbackRate,
        .state                   = state,
        .epoch                   = epoch,
        .controlEpoch            = epoch,
        .scheduleGeneration      = 1U,
        .seekSequence            = seekSequence,
        .appliedSeekSequence     = seekSequence,
        .playbackSequence        = playbackSequence,
        .appliedPlaybackSequence = playbackSequence,
        .sequence                = sequence,
        .finished                = false,
        .valid                   = true,
    };
}

/// @brief 比较确定性时钟结果。
/// @param lhs 实际秒数或本用例明确约定的偏移值。
/// @param rhs 预期值。
/// @param epsilon 绝对误差容限，积分运算用例可单独放宽。
/// @return 差值在容限内时成功；不使用真实时间等待来消除误差。
bool near(double lhs, double rhs, double epsilon = EPSILON)
{
    // 各场景时间尺度固定，不随输入大小放宽回归容差。
    return std::abs(lhs - rhs) <= epsilon;
}

/// @brief 验证同一音频 block 内多次逻辑更新保持连续匀速。
/// @return 重复观测连续推进且新观测无回退时为 true。
/// @note 固定音频快照，只推进传入壁钟，隔离音频 block 离散性。
/// @note 首次更新与观测同时发生，所以首值不包含读取延迟补偿。
bool testSameBlockContinuousAdvance()
{
    // 下一块 1020 帧与第三次外推的 1.02 秒一致，不引入额外观测误差。
    // 基线为谱面一秒、壁钟十秒，两者不能混作同一个绝对时间值。
    // 不修改快照里的采样率，后续帧位置变化始终使用一致换算。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    const auto block = makeSnapshot(
        // 大同步间隔让短时间内的结果只体现连续外推，而非周期误差校准。
        1000,
        10.0,
        MMM::Audio::AudioTimelinePlaybackState::Playing,
        2U);
    const double first = clock.update(block, 10.0, config);
    // 三次读取同一版本仍须得到不同视觉位置，不能把 sequence 当成更新许可。
    const double second = clock.update(block, 10.01, config);
    const double third  = clock.update(block, 10.02, config);
    // 检查绝对结果而非只看递增，否则错误的固定速度也可能通过。
    if ( !near(first, 1.0) || !near(second, 1.01) || !near(third, 1.02) ) {
        XERROR("Visual clock repeated a discrete audio block position");
        return false;
    }

    auto nextBlock = block;
    // 下一观测恰好承接此前外推位置，再晚一毫秒读取应继续前进一毫秒。
    nextBlock.positionFrame         = 1020;
    nextBlock.steadyTimeNanoseconds = 10'020'000'000;
    nextBlock.sequence              = 4U;
    // 音频观测时间也随新块更新，避免构造位置更新但时间戳仍陈旧的另一种场景。
    const double next = clock.update(nextBlock, 10.021, config);
    if ( next < third || !near(next, 1.021) ) {
        // 同时保留单调性断言，突出新块到达不能造成可见回退的约束。
        XERROR("A new audio block moved the visual clock backwards");
        return false;
    }
    return true;
}

/// @brief 验证同步周期到期前的新 block 误差不会逐帧拉回视觉时间。
/// @return 校准间隔内始终采用已有连续锚点时为 true。
/// @note 0.2 秒和 0.8 秒均在一秒周期内，不覆盖周期到期后的补偿大小。
bool testNewBlockDoesNotCalibrateBeforeSyncInterval()
{
    // epoch 保持不变，位置滞后不能被解释成一次显式向后定位。
    // 校准节流与连续推进是两件事：暂不纠偏不能意味着停住视觉时间。
    // 不发送暂停或定位命令，确保结果变化只由同步周期控制。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 1.0;

    const auto initial = makeSnapshot(
        0, 100.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 100.0, config));

    auto delayed = initial;
    // 壁钟已前进 0.2 秒，音频位置仅前进 0.1 秒，故意产生可观察的落后。
    // 随后在同一 block 上再推进壁钟，不能逐次重复吸收这个误差。
    delayed.positionFrame         = 100;
    delayed.steadyTimeNanoseconds = 100'200'000'000;
    delayed.sequence              = 4U;
    if ( !near(clock.update(delayed, 100.2, config), 0.2) ||
         !near(clock.update(delayed, 100.8, config), 0.8) ) {
        // 第二次读取在同一原点上外推，不应累计第一轮校准造成的位置损失。
        XERROR("New audio block calibrated before syncInterval elapsed");
        return false;
    }
    return true;
}

/// @brief 验证暂停冻结以及 Seek/循环纪元允许显式回退。
/// @return 暂停保持位置且两次离散控制均采用各自目标时为 true。
/// @note 连续播放的防回退规则不能阻止用户定位或循环回绕。
/// @note 场景串联以保留转换历史，分别创建时钟会漏掉旧锚点的影响。
bool testPauseAndDiscontinuities()
{
    // 暂停通过播放控制版本表达，不要求音频观测版本也同时变化。
    // Seek 与循环分别使用不同纪元，确保两个离散目标各被消费一次。
    // 先播放、再暂停、再定位、最后回绕，每一步都使用上一阶段时钟内部状态。
    // 所有检查只读取确定性数值，不依赖实际设备是否已停止发声。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    const auto                      playing = makeSnapshot(
        1000, 20.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(playing, 20.0, config));
    static_cast<void>(clock.update(playing, 20.1, config));
    // 先推进到暂停位置，再给出稍晚的读取时刻，制造暂停事件与读取的时差。

    auto paused = playing;
    // 暂停观测比逻辑读取早 20 毫秒，冻结点应取观测时刻而非读取时刻。
    paused.positionFrame    = 1100;
    paused.state            = MMM::Audio::AudioTimelinePlaybackState::Paused;
    paused.playbackSequence = 4U;
    paused.steadyTimeNanoseconds = 20'100'000'000;
    const double frozen          = clock.update(paused, 20.12, config);
    // 再跳过较长壁钟间隔，验证冻结不是只对转换发生的那一次更新有效。
    if ( !near(frozen, 1.1) ||
         !near(clock.update(paused, 21.0, config), frozen) ) {
        XERROR("Pause did not freeze the continuous visual clock");
        return false;
    }

    auto seek = paused;
    // 暂停状态不变，仅通过新的定位请求与纪元将位置退回半秒。
    // 本用例不等待 applied 字段确认，视觉层必须先反映控制请求。
    seek.positionFrame = 500;
    seek.epoch         = 2U;
    seek.controlEpoch  = 2U;
    seek.seekSequence  = 4U;
    // 请求版本变化与回退共同出现，不能仅靠帧位置减小猜测用户定位。
    seek.sequence              = 4U;
    seek.steadyTimeNanoseconds = 21'000'000'000;
    if ( !near(clock.update(seek, 21.0, config), 0.5) ) {
        // 暂停时仍允许用户跳转，冻结规则不能覆盖显式定位目标。
        XERROR("Seek epoch did not re-anchor to its explicit target");
        return false;
    }

    auto loop = seek;
    // 恢复播放同时出现新的传输纪元，目标比前次定位还早。
    // 明确目标应优先于旧视觉历史，不通过平滑追赶掩盖回绕。
    loop.positionFrame    = 250;
    loop.state            = MMM::Audio::AudioTimelinePlaybackState::Playing;
    loop.epoch            = 3U;
    loop.controlEpoch     = 3U;
    loop.playbackSequence = 6U;
    // 转回 Playing 后从新目标起步，不能先外推暂停前的运行区间。
    loop.sequence              = 6U;
    loop.steadyTimeNanoseconds = 21'100'000'000;
    if ( !near(clock.update(loop, 21.1, config), 0.25) ) {
        XERROR("Loop epoch did not allow an intentional backward jump");
        return false;
    }
    return true;
}

/// @brief 验证已观察的 pending Seek 被音频线程确认后不会前跳一个 block。
/// @return 请求立即可见，随后确认不额外消费音频 block 长度时为 true。
/// @note 请求与确认使用同一读取时刻，排除正常壁钟推进的影响。
/// @note 长校准周期避免误差校准掩盖确认处理的离散位置错误。
bool testPendingSeekAppliedAckKeepsContinuousTime()
{
    // 五秒目标远离旧一秒锚点，避免近距离容差掩盖定位未生效。
    // pending 与 applied 的区别在邮箱版本，不用真实线程竞争制造中间状态。
    // 音频位置均为整数毫秒，预期不需要设备缓冲延迟的容差。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    const auto initial = makeSnapshot(
        1000, 10.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 10.0, config));

    auto pending          = initial;
    pending.positionFrame = 5000;
    // 请求版本领先应用版本，模拟 UI 已更新目标但音频回调尚未确认。
    pending.steadyTimeNanoseconds = 10'100'000'000;
    pending.seekSequence          = 4U;
    pending.appliedSeekSequence   = 2U;
    // 先观察请求是前提，直接从初态跳到确认不属于已观察请求的确认场景。
    if ( !near(clock.update(pending, 10.1, config), 5.0) ) {
        XERROR("Pending Seek did not expose its target immediately");
        return false;
    }

    auto applied = pending;
    // 应用位置包含 20 毫秒音频推进，epoch 与 controlEpoch 一起变化表示纯确认。
    // 视觉已经消费过该定位，确认不能再次把 5.0 秒锚点前移到 5.02 秒。
    applied.positionFrame       = 5020;
    applied.epoch               = 2U;
    applied.controlEpoch        = 2U;
    applied.appliedSeekSequence = 4U;
    applied.sequence            = 4U;
    if ( !near(clock.update(applied, 10.1, config), 5.0) ) {
        // 若重锚为 5.02 秒，说明同一控制被视觉请求与音频确认各消费了一次。
        XERROR("Applied Seek acknowledgement advanced by one audio block");
        return false;
    }
    return true;
}

/// @brief 验证 Seek 应用后同一 block 的循环回绕仍按最终位置重建锚点。
/// @return 控制确认后的额外传输变化不会被当作纯确认忽略时为 true。
/// @note 与纯 Seek 确认用例配对，区分两类看似相同的 applied 版本变化。
/// @note 这里验证控制之后又发生的回绕，而非同一请求被重复确认。
bool testSeekAckWithSameBlockLoopStillReanchors()
{
    // 十秒目标与六秒回绕明显不同，不以极小数值差异区分两类转换。
    // 请求确认并非一律忽略纪元变化，确认后仍可能发生有业务含义的额外变化。
    // 通过最终位置与两个纪元共同描述回绕，不靠读取频率猜测回绕次数。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    const auto initial = makeSnapshot(
        1000, 20.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 20.0, config));

    auto pending                  = initial;
    pending.positionFrame         = 10000;
    pending.steadyTimeNanoseconds = 20'100'000'000;
    pending.seekSequence          = 4U;
    pending.appliedSeekSequence   = 2U;
    if ( !near(clock.update(pending, 20.1, config), 10.0) ) {
        return false;
    }

    auto looped = pending;
    // 定位目标为 10 秒，但同块内又回绕到 6 秒；两个纪元不相等是关键差异。
    // 读取时刻保持不变，因此预期六秒来自真实离散变化而非墙钟外推。
    looped.positionFrame         = 6000;
    looped.steadyTimeNanoseconds = 20'100'000'000;
    looped.epoch                 = 4U;
    looped.controlEpoch          = 3U;
    // 控制纪元只计到定位生效，传输纪元还包含回绕，不能将二者压平为相等。
    looped.appliedSeekSequence = 4U;
    looped.sequence            = 4U;
    // 确认不取消回绕，最终位置取该快照的六秒而不是此前目标十秒。
    if ( !near(clock.update(looped, 20.1, config), 6.0) ) {
        // 保持十秒虽满足“确认不跳变”，却丢失了同块内真正发生的循环。
        XERROR("Same-block loop was mistaken for a pure control ack");
        return false;
    }
    return true;
}

/// @brief 验证自然结束后的 pending Play 在 Seek 归零确认时不二次前跳。
/// @return 重播请求立即归零，音频确认后仍保持同一瞬间的零点时为 true。
/// @note 用已结束的旧节点初始化，确保零点不是未初始化状态的偶然结果。
bool testFinishedPlayAckKeepsPendingRestartPosition()
{
    // pending 同时更新播放态和结束原因，不能只改零点却仍表示自然结束。
    // 重播前存在十二秒旧位置，确保 pending Play 确实替换旧结束锚点。
    // 归零是控制意图，不应被后续首块已经生成的少量样本位置抢先覆盖。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    auto finished = makeSnapshot(
        // 起始状态是已自然结束的停止节点，不是普通暂停后恢复。
        12000,
        30.0,
        MMM::Audio::AudioTimelinePlaybackState::Stopped,
        2U);
    finished.epoch        = 5U;
    finished.controlEpoch = 5U;
    finished.finished     = true;
    // 同时设置停止态与结束原因，状态枚举本身不携带结束原因。
    static_cast<void>(clock.update(finished, 30.0, config));

    auto pending = finished;
    // 重播请求清除 finished 并切换播放态，应用版本暂时保留旧值。
    // 与定位测试不同，这里通过播放请求版本表达重新开始。
    pending.positionFrame         = 0;
    pending.steadyTimeNanoseconds = 30'100'000'000;
    pending.state            = MMM::Audio::AudioTimelinePlaybackState::Playing;
    pending.playbackSequence = 4U;
    pending.appliedPlaybackSequence = 2U;
    pending.finished                = false;
    if ( !near(clock.update(pending, 30.1, config), 0.0) ) {
        // 先断言请求阶段，避免确认阶段的通过掩盖请求反馈不及时。
        return false;
    }

    auto applied          = pending;
    applied.positionFrame = 20;
    // 归零后的首块已推进 20 帧，纯播放确认不能把已显示零点再次前移。
    applied.epoch                   = 6U;
    applied.controlEpoch            = 6U;
    applied.appliedPlaybackSequence = 4U;
    applied.sequence                = 4U;
    if ( !near(clock.update(applied, 30.1, config), 0.0) ) {
        XERROR("Finished Play acknowledgement advanced by one audio block");
        return false;
    }
    return true;
}

/// @brief 验证 Stop 的 Seek 与播放命令双确认只消费一次离散控制。
/// @return 双版本确认不会改变停止锚点时为 true。
/// @note 同一次停止可能同时触发定位与播放控制，不能按两个独立跳转处理。
/// @note 停止后检查的是确认不会改变锚点，不在此模拟真实停止设备的耗时。
bool testStopDoubleAckKeepsPendingStopPosition()
{
    // 零点是停止目标，不是播放状态下短暂经过的零点。
    // 停止同时改变播放态和位置，单测不能仅检查 isPlaying 而遗漏零点位置。
    // 本场景两个确认一起出现，不推断分不同更新到达时的具体顺序。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    const auto initial = makeSnapshot(
        1000, 40.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 40.0, config));

    auto pending          = initial;
    pending.positionFrame = 0;
    // 同时将两个请求版本置新、两个 applied 保持旧值，建立已经可见的停止状态。
    pending.steadyTimeNanoseconds = 40'100'000'000;
    pending.state        = MMM::Audio::AudioTimelinePlaybackState::Stopped;
    pending.seekSequence = 4U;
    pending.appliedSeekSequence     = 2U;
    pending.playbackSequence        = 4U;
    pending.appliedPlaybackSequence = 2U;
    if ( !near(clock.update(pending, 40.1, config), 0.0) ) {
        // 停止请求先可见，即使音频回调仍保留旧 applied 版本。
        return false;
    }

    auto applied  = pending;
    applied.epoch = 3U;
    // 两类确认同帧到达，传输和控制纪元一致，期间没有另一次循环或定位。
    applied.controlEpoch            = 3U;
    applied.appliedSeekSequence     = 4U;
    applied.appliedPlaybackSequence = 4U;
    applied.sequence                = 4U;
    if ( !near(clock.update(applied, 40.1, config), 0.0) ) {
        // 两类 applied 追上既有请求，没有发出新的控制请求。
        XERROR("Stop double acknowledgement changed the pending stop anchor");
        return false;
    }
    return true;
}

/// @brief 验证调度换代不比较跨代 epoch，且明显位置变化仍会重建锚点。
/// @return 近距离旧请求确认保持连续，大距离替换采用新位置时为 true。
/// @note 故意让新调度纪元小于旧纪元，防止把序号大小误当成时间连续性。
/// @note 连续性例外针对已观察请求的确认，不赋予代数大小业务时间含义。
bool testScheduleGenerationTransitionRules()
{
    // 远处替换为二十五秒，明确超过实现用于近距离确认的半秒边界。
    // 一个时钟连续经历两次替换，检验例外成立后不会永久关闭换代重锚行为。
    // 第一阶段保存连续位置，第二阶段采用离散位置，两种预期必须分别断言。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    auto initial = makeSnapshot(
        10000, 50.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    initial.epoch = 100U;
    // 旧节点积累较大纪元，仅用于制造跨代数值反差，不代表播放位置更靠后。
    initial.controlEpoch = 100U;
    static_cast<void>(clock.update(initial, 50.0, config));

    auto pending                  = initial;
    pending.positionFrame         = 10100;
    pending.steadyTimeNanoseconds = 50'100'000'000;
    pending.seekSequence          = 4U;
    pending.appliedSeekSequence   = 2U;
    if ( !near(clock.update(pending, 50.1, config), 10.1) ) {
        return false;
    }

    auto replacement = pending;
    // 已观察的定位在新调度中确认，原始位置与预测位置仅差 0.2 秒。
    // 允许这类近距离确认继续显示 10.2 秒，不跳到块位置 10.4 秒。
    replacement.positionFrame         = 10400;
    replacement.steadyTimeNanoseconds = 50'200'000'000;
    replacement.epoch                 = 1U;
    replacement.controlEpoch          = 1U;
    replacement.scheduleGeneration    = 2U;
    // 不能把旧节点的一百号纪元与新节点的一号直接相减判断跳转次数。
    replacement.appliedSeekSequence = 4U;
    replacement.sequence            = 4U;
    if ( !near(clock.update(replacement, 50.2, config), 10.2) ) {
        // 10.4 秒虽是新调度音频原始位置，但这个近距离确认应保持已有视觉连续性。
        XERROR("Schedule generation compared unrelated transport epochs");
        return false;
    }

    auto displaced = replacement;
    // 下一次调度替换移动到远处，不能把“换代可连续”的例外泛化到所有替换。
    displaced.positionFrame         = 25000;
    displaced.steadyTimeNanoseconds = 50'300'000'000;
    displaced.epoch                 = 0U;
    displaced.controlEpoch          = 0U;
    displaced.scheduleGeneration    = 3U;
    // 这次换代不带新的已观察请求确认，且位置明显偏移，必须重建锚点。
    displaced.sequence = 6U;
    if ( !near(clock.update(displaced, 50.3, config), 25.0) ) {
        XERROR("Displaced replacement schedule did not re-anchor");
        return false;
    }
    return true;
}

/// @brief 验证调度换代与旧控制确认不能吞掉同帧新出现的 pending Seek。
/// @return 新定位请求优先于保持旧确认连续性的分支时为 true。
/// @note 两类控制拥有独立版本，不能只维护一个“控制被确认”的总标志。
bool testNewPendingSeekOverridesScheduleAck()
{
    // Seek 与播放请求可使用相同数值版本，二者仍属于独立版本域。
    // 替换位置距离预测值很近，若只看距离会误触发保持连续性的例外。
    // 新定位版本必须作为更高优先级信息，不能等下一个音频块才显示。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    const auto initial = makeSnapshot(
        1000, 60.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 60.0, config));

    auto pendingPlayback = initial;
    // 先让视觉层见到播放请求，后续 applied 变化才能被识别为旧请求确认。
    pendingPlayback.positionFrame           = 1100;
    pendingPlayback.steadyTimeNanoseconds   = 60'100'000'000;
    pendingPlayback.playbackSequence        = 4U;
    pendingPlayback.appliedPlaybackSequence = 2U;
    // 此阶段仅播放待确认，定位版本不变，下一阶段才会出现新的定位请求。
    if ( !near(clock.update(pendingPlayback, 60.1, config), 1.1) ) {
        return false;
    }

    auto replacement = pendingPlayback;
    // 换代时旧播放请求被确认，但定位请求刚出现且尚未确认。
    // 新目标 1.4 秒必须立即显示，不能保持旧锚点推演出的 1.2 秒。
    replacement.positionFrame           = 1400;
    replacement.steadyTimeNanoseconds   = 60'200'000'000;
    replacement.scheduleGeneration      = 2U;
    replacement.seekSequence            = 4U;
    replacement.appliedSeekSequence     = 2U;
    replacement.appliedPlaybackSequence = 4U;
    // 旧播放确认不能抹掉同帧新 Seek 的目标位置。
    replacement.sequence = 4U;
    if ( !near(clock.update(replacement, 60.2, config), 1.4) ) {
        // 预期取新目标而非旧锚点外推值，以便暴露分支优先级错误。
        XERROR("Schedule acknowledgement swallowed a new pending Seek");
        return false;
    }
    return true;
}

/// @brief 验证长暂停恢复后不会沿用暂停前的音频时间原点。
/// @return 暂停不累积时间，恢复后的首块不会用旧原点触发错误校准时为 true。
/// @note 暂停间隔超过同步周期，专门检验恢复时是否沿用过期的校准历史。
bool testLongPauseResumeResetsAudioOrigin()
{
    // 恢复由新播放控制版本表示，不能仅靠壁钟前进推断播放已开始。
    // 恢复前先重复读取暂停快照，确认长期冻结和恢复衔接都正确。
    // 首个恢复块带少量滞后，比完全吻合的观测更容易暴露旧原点污染。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 1.0;

    const auto playing = makeSnapshot(
        1000, 70.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(playing, 70.0, config));

    auto paused = playing;
    // 用虚拟壁钟跨过五秒暂停，不调用 sleep，测试耗时不随暂停长度增加。
    paused.positionFrame         = 1100;
    paused.steadyTimeNanoseconds = 70'100'000'000;
    paused.state            = MMM::Audio::AudioTimelinePlaybackState::Paused;
    paused.playbackSequence = 4U;
    paused.sequence         = 4U;
    // 暂停的两次读取共用同一块，不依赖音频线程继续发布新观测。
    if ( !near(clock.update(paused, 70.1, config), 1.1) ||
         !near(clock.update(paused, 75.1, config), 1.1) ) {
        return false;
    }

    auto resumed = paused;
    // 恢复瞬间位置不变，只开始建立新的时间推进斜率。
    resumed.state            = MMM::Audio::AudioTimelinePlaybackState::Playing;
    resumed.playbackSequence = 6U;
    resumed.steadyTimeNanoseconds = 75'100'000'000;
    // 将恢复观测更新到暂停末尾，不能把五秒停顿计入新的运行原点。
    if ( !near(clock.update(resumed, 75.1, config), 1.1) ) {
        // 恢复本身不能把暂停的壁钟时间加入谱面时间。
        return false;
    }

    auto firstResumedBlock = resumed;
    // 新块仅推进 50 毫秒，视觉壁钟已推进 100 毫秒；
    // 校准周期未到，预期 1.2 秒而非直接采用新块的 1.15 秒。
    firstResumedBlock.positionFrame         = 1150;
    firstResumedBlock.steadyTimeNanoseconds = 75'200'000'000;
    firstResumedBlock.sequence              = 6U;
    // 恢复块确实为新版本，不能靠读取旧版本绕过原点更新路径而通过测试。
    if ( !near(clock.update(firstResumedBlock, 75.2, config), 1.2) ) {
        XERROR("Resume reused the pre-pause audio origin and hard re-anchored");
        return false;
    }
    return true;
}

/// @brief 验证半速与双速均按谱面时间倍率连续推进。
/// @return 两种倍率都满足起点加壁钟间隔乘倍率时为 true。
/// @note 倍率作用于增量而非起始位置，两种情况都从两秒开始。
bool testPlaybackRates()
{
    // 每轮先建立基线再外推，否则首次观测定位会与连续推进混为一个阶段。
    // 同时覆盖慢于与快于正常速度，不能只验证正常倍率一的恒等变换。
    // 正常速率合法化或非法倍率后备不属于这一组既有输入的验收范围。
    MMM::Config::SyncConfig config;
    for ( const auto [rate, expected] :
          { std::pair{ 0.5, 2.1 }, std::pair{ 2.0, 2.4 } } ) {
        // 每个倍率使用独立时钟，防止上一轮的锚点或校准历史污染下一轮。
        MMM::Logic::PlaybackVisualClock clock;
        const auto                      snapshot =
            makeSnapshot(2000,
                         30.0,
                         MMM::Audio::AudioTimelinePlaybackState::Playing,
                         2U,
                         1U,
                         2U,
                         2U,
                         rate);
        static_cast<void>(clock.update(snapshot, 30.0, config));
        // 只改变 now 不改音频帧位置，推进应来自连续时钟。
        // 0.2 秒壁钟分别产生 0.1 秒与 0.4 秒谱面推进。
        if ( !near(clock.update(snapshot, 30.2, config), expected) ) {
            XERROR("Visual clock did not apply playback rate {}", rate);
            return false;
        }
    }
    return true;
}

/// @brief 验证播放中切换倍率只重建斜率，不改变切换瞬间的位置。
/// @return 变速瞬间连续，后续按新倍率推进且首个新块不二次跳转时为 true。
/// @note 同一快照版本改变倍率，避免将成功归因于新块重建了整个锚点。
bool testPlaybackRateRebase()
{
    // 即时变速不修改音频位置，位置衔接必须来自旧斜率而非快照中的新位置。
    // 同步周期保持默认，变速对斜率和原点的处理不能依赖修改配置来回避校准。
    // 四个结果分别覆盖旧速率、切换瞬间、新速率和新观测接续。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    auto                            snapshot = makeSnapshot(
        1000, 50.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(snapshot, 50.0, config));
    const double beforeChange = clock.update(snapshot, 50.2, config);
    // 先形成已推进区间，若仍在起始时刻变速，错误缩放历史也难以暴露。

    snapshot.playbackRate = 2.0;
    // 同一壁钟时刻再次调用，单独隔离倍率变化对位置的影响。
    const double atChange    = clock.update(snapshot, 50.2, config);
    const double afterChange = clock.update(snapshot, 50.3, config);
    // 后续 0.1 秒按双速计算，切换前 0.2 秒保留单速结果。
    snapshot.positionFrame = 1400;
    // 新音频块与双速预测值一致，应保持 1.4 秒，不能重复积分已经走过的区间。
    snapshot.steadyTimeNanoseconds = 50'300'000'000;
    snapshot.sequence              = 4U;
    const double firstNewBlock     = clock.update(snapshot, 50.3, config);
    // 同时刻的新块与先前外推必须落在同一位置，不重复积分已走过的区间。
    if ( !near(beforeChange, 1.2) || !near(atChange, beforeChange) ||
         !near(afterChange, 1.4) || !near(firstNewBlock, 1.4) ) {
        XERROR("Playback speed change did not preserve and rebase visual time");
        return false;
    }
    return true;
}

/// @brief 验证替换时间线后即使命令序列重号，也不会沿用旧节点锚点。
/// @return 调用方显式 reset/rebase 后，新节点从指定位置开始时为 true。
/// @note 此场景验证外部替换生命周期，不声称单靠重号快照就能自动识别替换。
/// @note 新旧节点的观测和控制版本重号，可靠重置来自显式生命周期调用。
bool testReplacementWithReusedSequences()
{
    // 重置后的首次观测有效，因此不涉及缺失音频快照时的后备外推。
    // 不把新节点序号人为改大，保留重号这一触发旧历史误用的条件。
    // reset 与 rebase 的先后顺序代表调用方交接节点的既有用法。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    const auto                      oldNode = makeSnapshot(
        10000, 60.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(oldNode, 60.0, config));
    static_cast<void>(clock.update(oldNode, 60.2, config));
    // 旧节点已超过十秒，清除的不只是初始化标志，也包括旧位置历史。

    clock.reset();
    // 清历史后以新节点位置建立基线，序号可以与旧节点相同而不继承十秒锚点。
    clock.rebase(3.0, 60.2, 1.0, true);
    // 重建时刻与第一次新节点读取一致，所以结果应恰好三秒。
    const auto replacementNode = makeSnapshot(
        3000, 60.2, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    const double replaced = clock.update(replacementNode, 60.2, config);
    if ( !near(replaced, 3.0) ) {
        // 新节点首次有效观测必须与新基线一致，不保留旧节点已推进的 0.2 秒。
        XERROR("Replacement timeline reused the previous node clock anchor");
        return false;
    }
    return true;
}

/// @brief 验证周期校准沿用历史低通、死区和积分修正语义。
/// @return 小误差校准结果与既有积分公式一致时为 true。
/// @note 预期绑定历史系数，是算法语义回归而非任意参数的数学性质验证。
bool testHistoricalIntegralCalibration()
{
    // 模式由夹具显式给定，结果不依赖本机用户偏好中的同步模式。
    // 音频稍落后于视觉，校准应减少视觉时间而非反向扩大偏差。
    // 误差经过低通后仍高于死区，才能实际检验积分分支。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval   = 1.0;
    config.integralFactor = 0.1F;
    // 非单位因子使只做低通或直接校正的实现不能通过同一预期值。
    config.mode            = MMM::Config::SyncMode::WaterTank;
    config.waterTankBuffer = 10.0F;
    // 显式固定该用例相关偏好，避免改变默认配置后误判积分算法回归。

    const auto initial = makeSnapshot(
        0, 100.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 100.0, config));

    auto delayed                  = initial;
    delayed.positionFrame         = 1000;
    delayed.steadyTimeNanoseconds = 101'100'000'000;
    delayed.sequence              = 4U;
    const double corrected        = clock.update(delayed, 101.1, config);
    // 0.1 秒原点差经 0.05 低通得到 0.005 秒，再乘 0.1 积分因子。
    // 因此从 1.1 秒减去 0.0005 秒，断言使用略宽容差容纳浮点运算。
    if ( !near(corrected, 1.0995, 1.0e-8) ) {
        XERROR(
            "Visual clock integral calibration drifted from legacy semantics");
        return false;
    }
    return true;
}

/// @brief 验证大误差校准使用低通音频估计而非原始 block 位置。
/// @return 大误差重定位采用平滑估计，而不是直接跳到二十秒时为 true。
/// @note 保持调度和控制版本不变，确保进入普通周期校准而非 Seek 分支。
bool testLargeErrorUsesSmoothedAudioEstimate()
{
    // 帧数合法且有限，本场景不应落入非法输入后备路径。
    // 本场景音频明显领先视觉，校准方向与小误差落后场景相反。
    // 不设置新的 epoch，防止直接定位分支绕过需要验证的低通估计。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 1.0;

    const auto initial = makeSnapshot(
        0, 100.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 100.0, config));

    auto delayed          = initial;
    delayed.positionFrame = 20000;
    // 两秒壁钟内观测到二十秒位置，制造足以进入大误差分支的差异。
    delayed.steadyTimeNanoseconds = 102'000'000'000;
    delayed.sequence              = 4U;
    const double corrected        = clock.update(delayed, 102.0, config);
    // 十八秒原点差经 0.05 低通得到 0.9 秒，两秒外推加上它得到 2.9 秒。
    // 平滑后仍是大误差，不应再乘小误差的积分因子。
    if ( !near(corrected, 2.9, 1.0e-8) ) {
        XERROR(
            "Large visual clock correction ignored the smoothed audio "
            "estimate");
        return false;
    }
    return true;
}

/// @brief 验证积分因子保留历史超调语义而不被限制到单位区间。
/// @return 因子二完整参与修正，不被当成概率值钳制到一时为 true。
/// @note 观测时间结构沿用小误差场景，仅改变积分设置以隔离因素。
bool testIntegralFactorIsNotClamped()
{
    // 因子二可精确表示，预期差异不是配置浮点序列化误差造成的。
    // 保持默认同步模式，只显式改变周期和积分因子。
    // 结果容差足以区分因子二与因子一，不把数值差异视为浮点噪声放过。
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval   = 1.0;
    config.integralFactor = 2.0F;
    // 这是历史配置语义回归，不在测试中推荐该值作为实际播放默认值。

    const auto initial = makeSnapshot(
        0, 100.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 100.0, config));

    auto delayed                  = initial;
    delayed.positionFrame         = 1000;
    delayed.steadyTimeNanoseconds = 101'100'000'000;
    delayed.sequence              = 4U;
    const double corrected        = clock.update(delayed, 101.1, config);
    if ( !near(corrected, 1.09, 1.0e-8) ) {
        // 0.005 秒误差乘二得到 0.01 秒；若钳制到一会得到错误的 1.095 秒。
        XERROR("Visual clock integral factor was clamped");
        return false;
    }
    return true;
}

/// @brief 验证状态栏、波形、频谱及三类画布共用同一快照补间比例。
/// @return 时间推演与坐标补间比例一致且播放尾部停止外推时为 true。
/// @note 只验证共享快照算术，不启动 UI 或执行 GPU 渲染。
/// @note 普通画布与时间线共用偏移方法，预览额外应用渲染比例。
bool testSharedRenderSnapshotResolution()
{
    // 预览比例二可精确表示，观察缩放语义而非放大舍入误差。
    // 播放末尾取恰好相等的边界，不只验证超过终点的情况。
    // 同一个值快照服务多个消费者，测试不创建相互独立的 UI 时钟。
    // 若消费者各自增加不同时间偏移，状态栏与画布可能出现可见错位。
    MMM::Logic::RenderSnapshot snapshot;
    snapshot.isPlaying   = true;
    snapshot.currentTime = 5.0;
    // 视觉时间与播放时间故意相差一秒，补间应同量推进而非强制合并两种时间。
    snapshot.playbackTime                 = 4.0;
    snapshot.totalTime                    = 20.0;
    snapshot.snapshotSysTime              = 40.0;
    snapshot.playbackSpeed                = 2.0;
    snapshot.allowUiPlaybackInterpolation = true;
    snapshot.uiInterpolationAbsYSpeed     = 100.0;
    snapshot.uiInterpolationYOffsetScale  = 1.5;
    // 偏移比例与播放倍率属于不同量纲，均设为非单位值以暴露漏乘或重复乘。
    snapshot.renderScaleY = 2.0F;

    const double now = 40.025;
    // 双速下 25 毫秒壁钟对应 50 毫秒谱面时间；坐标还需应用自身缩放因子。
    const double elapsed = snapshot.playbackInterpolationElapsed(now);
    // 只计算一次 elapsed，再供坐标补间共用，避免比较不同时刻采样的结果。
    const double basicOffset    = snapshot.getInterpolatedOffset(elapsed);
    const double timelineOffset = snapshot.getInterpolatedOffset(elapsed);
    // 相同快照与 elapsed 应给出相同偏移，不按消费者重新取壁钟。
    const double previewOffset =
        snapshot.getInterpolatedOffset(elapsed) * snapshot.renderScaleY;
    if ( !near(snapshot.resolveCurrentTimeAt(now), 5.05) ||
         // 时间增量为双速的 0.05 秒，坐标增量为 0.025 × 100 × 1.5。
         !near(snapshot.resolvePlaybackTimeAt(now), 4.05) ||
         !near(basicOffset, 3.75) || !near(timelineOffset, 3.75) ||
         !near(previewOffset, 7.5) ) {
        XERROR(
            "Shared render snapshot time or canvas offset proportions differ");
        return false;
    }

    snapshot.playbackTime = snapshot.totalTime = 10.0;
    // 播放已经到末尾但视觉时间含偏移，停止外推不能把视觉时间强行钳到总时长。
    snapshot.currentTime     = 11.0;
    snapshot.snapshotSysTime = 41.0;
    // 即使 isPlaying 仍为真，到达总时长也必须让补间 elapsed 归零。
    // 模拟尾音尚未结束但谱面时间已到终点的过渡阶段。
    if ( !near(snapshot.playbackInterpolationElapsed(41.025), 0.0) ||
         !near(snapshot.resolveCurrentTimeAt(41.025), 11.0) ||
         !near(snapshot.resolvePlaybackTimeAt(41.025), 10.0) ) {
        XERROR("Final stretcher tail continued UI playback interpolation");
        // 末尾检查时间值而非只看 elapsed，防止某个解析入口绕过终点约束。
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行连续播放视觉时钟与共享 UI 补间测试。
/// @return 全部场景通过返回零；首个失败通过短路使程序返回一。
/// @note 无测试选择参数，直接运行全部既有场景直到首个失败。
int main()
{
    // 使用确定性壁钟与局部状态，场景顺序不用于模拟真实线程调度。
    // 返回码不代表真实设备延迟或 UI 观感已验收。
    // 快照在当前线程按值构造，不使用日志到达顺序作为同步证据。
    // 不写测试资源，结果由断言和进程返回码表达。
    return testSameBlockContinuousAdvance() &&
                   testNewBlockDoesNotCalibrateBeforeSyncInterval() &&
                   testPauseAndDiscontinuities() &&
                   testPendingSeekAppliedAckKeepsContinuousTime() &&
                   testSeekAckWithSameBlockLoopStillReanchors() &&
                   testFinishedPlayAckKeepsPendingRestartPosition() &&
                   testStopDoubleAckKeepsPendingStopPosition() &&
                   testScheduleGenerationTransitionRules() &&
                   testNewPendingSeekOverridesScheduleAck() &&
                   testLongPauseResumeResetsAudioOrigin() &&
                   testPlaybackRates() && testPlaybackRateRebase() &&
                   testReplacementWithReusedSequences() &&
                   testHistoricalIntegralCalibration() &&
                   testLargeErrorUsesSmoothedAudioEstimate() &&
                   testIntegralFactorIsNotClamped() &&
                   testSharedRenderSnapshotResolution()
               ? 0
               : 1;
}
