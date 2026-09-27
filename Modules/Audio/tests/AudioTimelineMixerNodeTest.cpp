#include "audio/AudioTimelineMixerNode.h"
#include "audio/KeySoundControl.h"

#include "log/colorful-log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>
#include <string>
#include <vector>

namespace
{

// 本测试以真实解码 PCM 和小尺寸 AudioBuffer 驱动节点，覆盖下列契约：
//
// - 同一资源可在不同时间位置和音量下形成独立片段；
// - 负起点只裁去时间线零点前的源帧，不改变后续源帧对应关系；
// - 多片段交叠按确定顺序累加，主增益在累加完成后统一生效；
// - Seek 后每个活跃片段都从与新时间线位置对应的源帧恢复；
// - Pause 产生静音且不推进位置，Stop 与 Play 组合从零重新开始；
// - 循环采用 [L, R) 半开区间，抵达 R 后同一 block 从 L 继续填充；
// - 自然结束只发送一次 final，显式重新播放后才允许再次发送；
// - 边界查询承诺的下一段输入不会被随后到达的控制命令插入；
// - 时钟快照中的位置、状态、纪元、时间戳和控制序列保持一致；
// - 连续发布多份调度时，音频线程只在 block 起点接管最后一份；
// - 被替换调度不在音频回调析构，而由控制线程显式回收；
// - BGM 区域与逐轨增益在下一 block 生效，但不改变调度代次。
//
// 测试使用很小的 block，是为了让边界、跳转和控制应用点能够精确断言。
// 所有样本比较使用统一误差，避免解码浮点值的细微舍入影响协议验证。
//
// 测试结构还遵循以下隔离规则：
//
// - 每个场景自己构造 MixerNode，避免控制邮箱版本跨场景延续；
// - 需要样本真值时直接从 AudioTrack 或人工 PCM 读取，不调用被测混音 helper；
// - 状态断言紧跟触发操作，明确区分发布意图与音频线程确认；
// - 循环与结束场景同时检查位置、状态和纪元，不用单一字段猜测结果；
// - 生命周期场景使用 weak_ptr 观察，不改变被测 shared_ptr 引用计数；
// - 每个输出缓冲只用于一个阶段，避免旧样本掩盖 clear 或静音缺陷；
// - 所有日志只在测试线程失败路径执行，不影响被测实时函数的约束。

/// @brief 浮点 PCM 样本比较使用的绝对误差。
///
/// 测试目标是验证索引、增益和边界，不要求不同平台完成逐位相同的浮点运算。
constexpr float SAMPLE_EPSILON = 1.0e-5F;

/// @brief 记录时间线自然结束通知次数。
/// @param context 指向测试栈上计数器的非拥有指针。
///
/// 回调只做一次整数递增，用来模拟实时安全的下游 final 通知接收者。
void countFinalInputNotification(void* context) noexcept
{
    if ( !context ) return;
    ++(*static_cast<std::size_t*>(context));
}

/// @brief 比较输出缓冲区与两个源区间的加权和。
/// @param output 节点实际生成的完整输出 block。
/// @param firstSource 从 block 起点开始参与混合的参考源。
/// @param secondSource 可选的第二个交叠参考源。
/// @param secondOutputStart 第二个源在输出中的起始偏移。
/// @param firstVolume 第一个源的期望总增益。
/// @param secondVolume 第二个源的期望总增益。
/// @return 所有声道和帧均在误差内时返回 true。
///
/// helper 明确按输出坐标计算期望值，可同时发现源起点、输出偏移与音量中任一
/// 项发生错误；发现首个差异即记录精确位置，避免大量重复日志。
/// firstSource 必须覆盖完整输出长度；secondSource 只需覆盖从 secondOutputStart
/// 到输出末端的区间。调用点负责保证两个参考缓冲的声道布局与 output 一致。
bool verifyMixedRange(const ice::AudioBuffer& output,
                      const ice::AudioBuffer& firstSource,
                      const ice::AudioBuffer* secondSource,
                      std::size_t secondOutputStart, float firstVolume,
                      float secondVolume)
{
    // 逐声道逐帧建立期望，不复用被测节点的 span 计算逻辑。
    for ( std::size_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < output.num_frames(); ++frame ) {
            float expected =
                firstSource.raw_ptrs()[channel][frame] * firstVolume;
            // 第二个片段开始前只有 firstSource，之后验证真正的加法混合。
            if ( secondSource && frame >= secondOutputStart ) {
                expected +=
                    secondSource
                        ->raw_ptrs()[channel][frame - secondOutputStart] *
                    secondVolume;
            }
            if ( std::abs(output.raw_ptrs()[channel][frame] - expected) >
                 SAMPLE_EPSILON ) {
                XERROR(
                    "Timeline mix mismatch: channel={}, frame={}, actual={}, "
                    "expected={}",
                    channel,
                    frame,
                    output.raw_ptrs()[channel][frame],
                    expected);
                return false;
            }
        }
    }
    return true;
}

/// @brief 验证负起点裁切、同文件多声部和 Seek 源帧恢复。
/// @param track 用来独立读取期望 PCM 的测试音轨。
/// @param audio 同一音轨冻结出的被测时间线资源。
/// @return 初始混合、主增益、Seek、Pause 和电平均符合契约时返回 true。
///
/// 两个片段故意使用相同 sourceKey，证明调度身份来自事件而不是按文件去重。
/// 负起点片段在时间线零点应从源第 16 帧开始；正起点片段从输出第 8 帧加入。
/// Seek 到 20 后，两者分别对应源第 36 帧和第 12 帧，能暴露错误的 voice 复用。
///
/// 场景分为四段：
///
/// - 构造后检查无效片段过滤没有误删同资源的两个事件；
/// - 首个 block 对照两段独立 AudioTrack 读取，验证负起点裁切与重叠累加；
/// - 设置 0.5 主增益后检查左右峰值非零，验证电平取自最终输出；
/// - Seek 后重新建立两份参考 PCM，确保源坐标随时间线位置同步改变；
/// - Pause 后再次拉取，确保输出静音、位置冻结且峰值归零。
///
/// 首片段静态音量 0.5 再乘主增益得到 0.25；第二片段静态音量 0.25
/// 再乘主增益得到 0.125。这里显式使用最终增益，能发现主增益应用次数错误。
/// 片段构造顺序特意为正起点在前、负起点在后，避免测试偶然依赖输入顺序。
/// requestedTimelineEndFrame 为 96，但较晚片段尾部可能更远，因此结束帧只检查
/// 不小于实际片段尾部，不把外部样本的具体总长度写死到测试契约中。
bool testOverlapNegativeStartAndSeek(
    const std::shared_ptr<ice::AudioTrack>&                         track,
    const std::shared_ptr<const MMM::Audio::PreparedTimelineAudio>& audio)
{
    constexpr std::size_t BLOCK_FRAMES = 32U;
    // 输入顺序与起点顺序相反，顺带验证构造阶段会建立确定调度顺序。
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 20U,
                .sourceKey  = "same",
                .startFrame = 8,
                .volume     = 0.25F,
                .audio      = audio,
            },
            {
                .eventId    = 10U,
                .sourceKey  = "same",
                .startFrame = -16,
                .volume     = 0.5F,
                .audio      = audio,
            },
        },
        96,
        16U);

    if ( node.clipCount() != 2U ||
         node.timelineEndFrame() <
             static_cast<MMM::Audio::AudioTimelineFrame>(track->num_frames()) +
                 8 ) {
        XERROR("Timeline mixer did not preserve both overlapping clips");
        return false;
    }
    // 构造元数据正确后再检查参数接口，便于失败时区分准备与混音问题。
    // 主增益应作用于两个片段的合成结果，因此期望总增益分别减半。
    node.setMasterGain(0.5F);
    if ( std::abs(node.masterGain() - 0.5F) > SAMPLE_EPSILON ) {
        XERROR("Timeline mixer did not retain its master gain");
        return false;
    }

    ice::AudioBuffer output(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    ice::AudioBuffer firstReference(ice::ICEConfig::internal_format,
                                    BLOCK_FRAMES);
    ice::AudioBuffer secondReference(ice::ICEConfig::internal_format,
                                     BLOCK_FRAMES - 8U);
    firstReference.clear();
    secondReference.clear();
    // 参考值直接从 AudioTrack 读取，不依赖 PreparedTimelineAudio 的混合实现。
    track->read(firstReference, 16U, BLOCK_FRAMES);
    track->read(secondReference, 0U, BLOCK_FRAMES - 8U);

    node.play();
    node.process(output);
    // positionFrame 表示下一帧坐标，所以完整 32 帧 block 后应恰好等于 32。
    if ( node.positionFrame() !=
             static_cast<MMM::Audio::AudioTimelineFrame>(BLOCK_FRAMES) ||
         !verifyMixedRange(
             output, firstReference, &secondReference, 8U, 0.25F, 0.125F) ) {
        return false;
    }
    if ( node.leftLevel() <= 0.0F || node.rightLevel() <= 0.0F ) {
        XERROR("Timeline mixer did not publish output levels");
        return false;
    }

    constexpr std::size_t SEEK_BLOCK_FRAMES = 8U;
    // Seek 跨过两个片段的起点，两条 voice 都必须重算各自源偏移。
    node.seek(20);
    ice::AudioBuffer seekOutput(ice::ICEConfig::internal_format,
                                SEEK_BLOCK_FRAMES);
    ice::AudioBuffer seekFirstReference(ice::ICEConfig::internal_format,
                                        SEEK_BLOCK_FRAMES);
    ice::AudioBuffer seekSecondReference(ice::ICEConfig::internal_format,
                                         SEEK_BLOCK_FRAMES);
    seekFirstReference.clear();
    seekSecondReference.clear();
    track->read(seekFirstReference, 36U, SEEK_BLOCK_FRAMES);
    track->read(seekSecondReference, 12U, SEEK_BLOCK_FRAMES);
    node.process(seekOutput);
    // 两个片段在 Seek 后从同一输出位置开始活跃，secondOutputStart 因此为零。
    if ( !verifyMixedRange(seekOutput,
                           seekFirstReference,
                           &seekSecondReference,
                           0U,
                           0.25F,
                           0.125F) ) {
        XERROR("Timeline seek did not resume every overlapping source frame");
        return false;
    }

    // Pause 后仍调用 process，验证回调清零输出但保持刚消费完的位置。
    node.pause();
    ice::AudioBuffer pausedOutput(ice::ICEConfig::internal_format,
                                  SEEK_BLOCK_FRAMES);
    node.process(pausedOutput);
    for ( std::size_t channel = 0U; channel < pausedOutput.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < pausedOutput.num_frames();
              ++frame ) {
            if ( pausedOutput.raw_ptrs()[channel][frame] != 0.0F ) {
                XERROR("Paused timeline emitted non-silent audio");
                return false;
            }
        }
    }
    return node.positionFrame() == 28 &&
           node.state() == MMM::Audio::AudioTimelinePlaybackState::Paused &&
           node.leftLevel() == 0.0F && node.rightLevel() == 0.0F;
}

/// @brief 验证半开循环在 R 处截断并于每轮 L 重建交叠 voice。
/// @param track 提供至少一个循环长度的已解码 PCM。
/// @return 16 帧输出恰好由两个相同 8 帧周期组成时返回 true。
///
/// 片段与循环都从第 4 帧开始，因此每次回到 L 都应重新从资源第 0 帧读取。
/// 最终位置仍为 L，epoch 至少包含 Seek 和两次边界跳转产生的非连续变化。
///
/// 场景特意让输出长度是循环长度的两倍：
///
/// - 第一段消费 [4, 12) 并读取资源 [0, 8)；
/// - 到达排除终点 12 时 Transport 跳回包含起点 4；
/// - 第二段再次消费 [4, 12) 并重新读取资源 [0, 8)；
/// - block 尾部再次回绕，因此最终发布位置为 4；
/// - 循环过程不设置 finished，也不得发送自然结束通知。
///
/// 若实现错误地采用闭区间，第二周期会错位一帧，逐样本比较会立即失败。
/// setLoop 的返回值也在消费前检查，确保失败不是由非法循环被静默忽略造成。
bool testHalfOpenLoop(const std::shared_ptr<ice::AudioTrack>& track)
{
    const auto audio = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    if ( !audio ) return false;
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 1U,
                .sourceKey  = "loop",
                .startFrame = 4,
                .volume     = 1.0F,
                .audio      = audio,
            },
        },
        32,
        32U);
    if ( !node.setLoop({ 4, 12 }) ) {
        XERROR("Timeline mixer rejected a valid half-open loop");
        return false;
    }
    node.seek(4);
    node.play();

    // reference 只保存一个循环周期，输出比较通过 frame % 8 映射两轮。
    ice::AudioBuffer output(ice::ICEConfig::internal_format, 16U);
    ice::AudioBuffer reference(ice::ICEConfig::internal_format, 8U);
    reference.clear();
    track->read(reference, 0U, 8U);
    // 单个 16 帧设备 block 跨越两次 8 帧循环，要求节点在 block 内分段。
    node.process(output);

    for ( std::size_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < output.num_frames(); ++frame ) {
            // 取模只用于测试期望值，不复用被测节点的循环位置实现。
            const float expected = reference.raw_ptrs()[channel][frame % 8U];
            if ( std::abs(output.raw_ptrs()[channel][frame] - expected) >
                 SAMPLE_EPSILON ) {
                XERROR("Half-open timeline loop mismatch at frame {}", frame);
                return false;
            }
        }
    }

    return node.positionFrame() == 4 && node.epoch() >= 3U && !node.finished();
}

/// @brief 验证空资源安全过滤和自然结束状态。
/// @return 空片段被移除、尾部静音、final 幂等且可重新触发时返回 true。
///
/// requestedTimelineEndFrame 独立于片段资源，因此即使所有片段无效，Transport
/// 仍须推进到第 6 帧并在同一输出 block 内结束，后四帧保持静音。
/// 结束后的 Seek 应立即显示目标并转为暂停；再次 Play 才开始下一轮结束通知。
///
/// 本场景同时区分三个容易混淆的状态：
///
/// - state 是音频线程实际 Transport 状态，自然结束后为 Paused；
/// - requestedState 是界面应呈现的控制意图，自然结束后为 Stopped；
/// - finished 只表示自然尾部已经提交，而不是任意停止状态。
///
/// 第一次 10 帧拉取只推进 6 帧，输出全静音且通知计数为一。Seek 到 2 后的
/// 一帧拉取仍保持暂停，不应误触第二次 final。重新 Play 后跨过尾部才把计数加二。
/// 空音频片段带有完整事件元数据，确保过滤条件只依据资源有效性而不是键值。
bool testMissingResourceAndFinish()
{
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 1U,
                .sourceKey  = "missing",
                .startFrame = 0,
                .volume     = 1.0F,
                .audio      = {},
            },
        },
        6,
        4U);
    std::size_t finalNotificationCount = 0U;
    node.setFinalInputListener(&finalNotificationCount,
                               &countFinalInputNotification);
    if ( node.clipCount() != 0U ) {
        XERROR("Timeline mixer retained a missing audio resource");
        return false;
    }

    // final listener 使用栈上计数器，节点不会取得其所有权。
    // 10 帧缓冲大于 6 帧时间线，用于同时验证精确结束与尾部清零。
    node.play();
    ice::AudioBuffer output(ice::ICEConfig::internal_format, 10U);
    node.process(output);
    if ( node.positionFrame() != 6 || !node.finished() ||
         finalNotificationCount != 1U ||
         node.state() != MMM::Audio::AudioTimelinePlaybackState::Paused ||
         node.requestedState() !=
             MMM::Audio::AudioTimelinePlaybackState::Stopped ) {
        XERROR("Timeline mixer did not stop at the composite end frame");
        return false;
    }
    for ( std::size_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < output.num_frames(); ++frame ) {
            if ( output.raw_ptrs()[channel][frame] != 0.0F ) return false;
        }
    }

    // 先读取即时位置，再用一帧 process 确认 Seek 被应用但仍未开始播放。
    // 自然结束后的 Seek 不应自行播放，但 UI 查询必须立即看到位置 2。
    node.seek(2);
    if ( node.positionFrame() != 2 ) {
        XERROR("Paused timeline did not expose its pending seek target");
        return false;
    }
    ice::AudioBuffer seekOutput(ice::ICEConfig::internal_format, 1U);
    node.process(seekOutput);
    if ( node.positionFrame() != 2 || node.finished() ||
         finalNotificationCount != 1U ||
         node.state() != MMM::Audio::AudioTimelinePlaybackState::Paused ||
         node.requestedState() !=
             MMM::Audio::AudioTimelinePlaybackState::Paused ) {
        return false;
    }

    // 新的显式 Play 清除 finished，下一次抵达尾部应再次通知且仅通知一次。
    node.play();
    node.process(output);
    node.process(output);
    return node.finished() && finalNotificationCount == 2U;
}

/// @brief 验证边界查询、精确结束和结束后的调度替换不会自行复活。
/// @return final 与 discontinuity 边界及结束保持语义全部正确时返回 true。
///
/// 首次查询请求 16 帧，但时间线只剩 8 帧，因此必须返回 8 帧 Final。消费恰好
/// 8 帧后立即自然结束；随后替换更长调度和设置循环都只是配置变化，不是 Play。
/// 最后显式 Seek 加 Play 进入 [2, 6)，验证循环边界与自然结束互不混淆。
///
/// 关键断言按以下边界协议组织：
///
/// - prepare 只描述下一段连续输入，不提前推进位置；
/// - process 消费与承诺相同的八帧，并在末端同步发布 final；
/// - 已完成节点接管新调度后保持 finished 与 Stopped；
/// - 仅设置循环不会构成播放请求；
/// - 显式 Seek 和 Play 清除结束状态并进入循环；
/// - 循环段末端返回 Discontinuity，而不是复用 Final；
/// - 回绕后 epoch 与 controlEpoch 不同，用于提示下游重置连续处理状态。
///
/// 替换后的 probe block 只有一帧，目的只是迫使音频线程接管 pending 状态；
/// 它不应推进已结束时间线，也不应改变既有 final 通知次数。
bool testInputBoundaryAndFinishedReplacement()
{
    using MMM::Audio::AudioTimelineInputBoundaryKind;
    using MMM::Audio::AudioTimelinePlaybackState;

    MMM::Audio::AudioTimelineMixerNode node({}, 8, 8U);
    std::size_t                        finalNotificationCount = 0U;
    node.setFinalInputListener(&finalNotificationCount,
                               &countFinalInputNotification);
    node.play();

    // 边界长度必须精确限制到排除终点，不能把设备请求长度原样返回。
    const auto finalBoundary = node.prepareInputBoundary(16U);
    if ( finalBoundary.frameCount != 8U ||
         finalBoundary.kind != AudioTimelineInputBoundaryKind::Final ) {
        XERROR("Timeline mixer did not expose its exact final input boundary");
        return false;
    }

    ice::AudioBuffer exactOutput(ice::ICEConfig::internal_format, 8U);
    node.process(exactOutput);
    // 精确长度消费不能要求额外零帧 process 才发布完成状态。
    if ( node.positionFrame() != 8 || !node.finished() ||
         finalNotificationCount != 1U ||
         node.requestedState() != AudioTimelinePlaybackState::Stopped ) {
        XERROR("Timeline mixer did not publish an exact natural finish");
        return false;
    }

    // 调度替换继承已结束状态，不能把新 Transport 的默认状态暴露为播放。
    node.replaceSchedule({}, 16, 8U);
    ice::AudioBuffer probeOutput(ice::ICEConfig::internal_format, 1U);
    node.process(probeOutput);
    if ( !node.finished() ||
         node.state() != AudioTimelinePlaybackState::Stopped ||
         node.requestedState() != AudioTimelinePlaybackState::Stopped ||
         node.positionFrame() != 0 ) {
        XERROR("Finished timeline restarted after a schedule replacement");
        return false;
    }

    // 单独改变循环配置仍不构成重新播放授权。
    if ( !node.setLoop({ 2, 6 }) ) return false;
    node.process(probeOutput);
    if ( !node.finished() ||
         node.state() != AudioTimelinePlaybackState::Stopped ) {
        XERROR("Loop configuration revived a naturally finished timeline");
        return false;
    }

    // 只有显式位置与播放意图到达后，边界才变为循环 discontinuity。
    node.seek(2);
    node.play();
    const auto loopBoundary = node.prepareInputBoundary(16U);
    if ( loopBoundary.frameCount != 4U ||
         loopBoundary.kind != AudioTimelineInputBoundaryKind::Discontinuity ) {
        XERROR("Timeline mixer did not expose its half-open loop boundary");
        return false;
    }

    ice::AudioBuffer loopOutput(ice::ICEConfig::internal_format, 4U);
    node.process(loopOutput);
    // 四帧恰好填满 [2, 6)，最终位置回到 2 且不触发第二次 final。
    const auto loopSnapshot = node.clockSnapshot();
    return !node.finished() && node.positionFrame() == 2 &&
           node.state() == AudioTimelinePlaybackState::Playing &&
           finalNotificationCount == 1U && loopSnapshot.valid &&
           loopSnapshot.epoch != loopSnapshot.controlEpoch;
}

/// @brief 验证 Stop 与紧随其后的 Play 仍会从零点开启新纪元。
/// @return 合并应用两个邮箱后第一帧从零开始播放时返回 true。
///
/// Stop 会发布零点 Seek 和停止命令，紧接的 Play 又覆盖播放命令。音频线程在
/// 下个 block 必须仍先应用 Seek，再应用最终 Play，不能因 Stop
/// 被覆盖而漏掉回零。
///
/// 前置阶段先播放四帧再暂停，使当前位置明确不是零。随后不产生中间音频 block，
/// 连续写入 Stop 与 Play。测试只拉取一帧并期待位置为一：如果回零 Seek 丢失，
/// 位置会从四继续变为五；如果最终 Play 丢失，位置会保持零且状态不是 Playing。
/// 因此一个简短断言同时覆盖两个独立邮箱按固定顺序合并的行为。
/// pause 与中间 process 还确保测试起点来自稳定发布快照，而非待应用 Play 状态。
bool testImmediateStopThenPlay()
{
    MMM::Audio::AudioTimelineMixerNode node({}, 32, 8U);
    ice::AudioBuffer block(ice::ICEConfig::internal_format, 4U);
    node.play();
    node.process(block);
    if ( node.positionFrame() != 4 ) return false;

    node.pause();
    node.process(block);
    // 确保暂停已经进入 Transport，再测试两个尚未应用的新命令。
    // 两个调用之间不插入 process，模拟 UI 同一帧连续触发的控制操作。
    node.stop();
    node.play();

    // 只消费一帧便可精确确认新播放从零点开始。
    ice::AudioBuffer firstRestartFrame(ice::ICEConfig::internal_format, 1U);
    node.process(firstRestartFrame);
    if ( node.positionFrame() != 1 ||
         node.state() != MMM::Audio::AudioTimelinePlaybackState::Playing ) {
        XERROR("Immediate stop-then-play did not restart the timeline at zero");
        return false;
    }
    return true;
}

/// @brief 验证逻辑线程取得的位置、状态、时间戳和控制序列来自一致快照。
/// @return 初始、待应用控制、已应用控制及自然结束快照均一致时返回 true。
///
/// 本场景逐阶段比较 sequence 与 appliedSequence：控制调用后请求版本领先，
/// process 后应用版本追平。这样可区分 UI 的即时反馈与音频线程真实接收点。
/// controlEpoch 应在普通推进后等于 epoch，在控制导致跳转时反映对应新纪元。
///
/// 快照阶段依次为：
///
/// - 初始构造发布完整偶数 sequence、停止状态和非零调度代次；
/// - Play 发布后状态立即变为 Playing，但应用版本仍是初始值；
/// - 首个 block 后位置前进且播放应用版本追平；
/// - Seek 发布后位置立即显示 10，应用版本仍落后；
/// - 下个 block 后从 10 消费到 14，Seek 应用版本和纪元同步更新；
/// - Pause 发布后即时状态为 Paused；
/// - 另一节点自然结束后再 Play，快照立即回零但仍标记命令待应用；
/// - 消费一帧后位置为一，播放应用版本最终追平。
///
/// 各阶段还要求时间戳单调不回退，防止 UI 用较旧请求覆盖较新音频锚点。
/// sequence 只要求相对初始值前进且保持偶数，不依赖具体递增次数，避免把内部
/// 发布频率误固化成公共行为。
bool testCoherentTimelineClockSnapshot()
{
    using MMM::Audio::AudioTimelineMixerNode;
    using MMM::Audio::AudioTimelinePlaybackState;

    // 构造函数必须发布一份可立即读取、代次非零的完整停止快照。
    AudioTimelineMixerNode node({}, 32, 8U);
    const auto             initial = node.clockSnapshot();
    if ( !initial.valid || (initial.sequence & 1U) != 0U ||
         initial.steadyTimeNanoseconds <= 0 ||
         initial.state != AudioTimelinePlaybackState::Stopped ||
         initial.controlEpoch != initial.epoch ||
         initial.scheduleGeneration == 0U ||
         initial.appliedSeekSequence != initial.seekSequence ||
         initial.appliedPlaybackSequence != initial.playbackSequence ) {
        XERROR("Timeline clock did not publish a coherent initial snapshot");
        return false;
    }

    // 尚未 process 时，状态先显示 Playing，但 applied 版本仍停留在初始值。
    node.play();
    const auto pendingPlay = node.clockSnapshot();
    if ( !pendingPlay.valid ||
         pendingPlay.state != AudioTimelinePlaybackState::Playing ||
         pendingPlay.playbackSequence == initial.playbackSequence ||
         pendingPlay.appliedPlaybackSequence !=
             initial.appliedPlaybackSequence ) {
        XERROR("Timeline clock did not expose a stable pending Play command");
        return false;
    }

    ice::AudioBuffer block(ice::ICEConfig::internal_format, 4U);
    // 一个 block 后播放请求被接收，位置和 applied 版本必须在同一快照更新。
    node.process(block);
    const auto advanced = node.clockSnapshot();
    if ( !advanced.valid || advanced.sequence <= initial.sequence ||
         advanced.positionFrame != 4 ||
         advanced.state != AudioTimelinePlaybackState::Playing ||
         advanced.controlEpoch != advanced.epoch ||
         advanced.scheduleGeneration != initial.scheduleGeneration ||
         advanced.appliedPlaybackSequence != advanced.playbackSequence ) {
        XERROR("Timeline clock position did not advance with its publication");
        return false;
    }

    // 待应用 Seek 立即覆盖显示位置和锚点时间，但还不能伪造已应用纪元。
    node.seek(10);
    const auto pendingSeek = node.clockSnapshot();
    if ( !pendingSeek.valid || pendingSeek.positionFrame != 10 ||
         pendingSeek.seekSequence == advanced.seekSequence ||
         pendingSeek.appliedSeekSequence != advanced.appliedSeekSequence ||
         pendingSeek.steadyTimeNanoseconds < advanced.steadyTimeNanoseconds ) {
        XERROR("Timeline clock did not expose a stable pending Seek command");
        return false;
    }

    // 应用 Seek 后再消费四帧，最终位置应为 14 且应用版本追平。
    node.process(block);
    const auto appliedSeek = node.clockSnapshot();
    if ( !appliedSeek.valid || appliedSeek.positionFrame != 14 ||
         appliedSeek.epoch == advanced.epoch ||
         appliedSeek.controlEpoch != appliedSeek.epoch ||
         appliedSeek.appliedSeekSequence != appliedSeek.seekSequence ) {
        XERROR("Timeline clock did not publish the applied Seek epoch");
        return false;
    }

    // Pause 也应在下一个音频 block 前先以待应用状态呈现给逻辑线程。
    node.pause();
    const auto pendingPause = node.clockSnapshot();
    if ( !pendingPause.valid ||
         pendingPause.state != AudioTimelinePlaybackState::Paused ) {
        XERROR("Timeline clock did not expose a stable pending Pause command");
        return false;
    }

    // finishedNode 的长度与 block 一致，能验证抵达终点当次发布结束。
    // 单独节点覆盖自然结束后再次 Play 的即时回零显示。
    AudioTimelineMixerNode finishedNode({}, 4, 4U);
    finishedNode.play();
    finishedNode.process(block);
    const auto finished = finishedNode.clockSnapshot();
    if ( !finished.valid || !finished.finished ) return false;
    finishedNode.play();
    const auto restarted = finishedNode.clockSnapshot();
    if ( !restarted.valid || restarted.positionFrame != 0 ||
         restarted.finished ||
         restarted.state != AudioTimelinePlaybackState::Playing ||
         restarted.appliedPlaybackSequence == restarted.playbackSequence ) {
        XERROR("Finished timeline clock did not expose pending Play at zero");
        return false;
    }

    ice::AudioBuffer restartFrame(ice::ICEConfig::internal_format, 1U);
    finishedNode.process(restartFrame);
    // 应用重启后 controlEpoch 与 epoch 相同，说明本 block 没有内部循环跳转。
    const auto appliedRestart = finishedNode.clockSnapshot();
    if ( !appliedRestart.valid || appliedRestart.positionFrame != 1 ||
         appliedRestart.controlEpoch != appliedRestart.epoch ||
         appliedRestart.appliedPlaybackSequence !=
             appliedRestart.playbackSequence ) {
        XERROR("Finished Play acknowledgement fields were inconsistent");
        return false;
    }
    return true;
}

/// @brief 验证循环规范化、同 block 回绕和调度换代快照字段保持一致。
/// @return 循环、待应用 Seek 和新调度快照的字段关系均正确时返回 true。
///
/// 从位置 6 消费四帧会先到排除终点 8，再回到起点 4，最终仍停在 6；因此
/// epoch 必须领先于 controlEpoch。循环中 Seek 到 20 应立即规范为起点 4。
/// 清除循环并替换调度后，generation 必须变化，而全部控制确认版本仍保持一致。
///
/// 位置 6 在循环 [4, 8) 内只剩两帧，四帧 block 的内部结构为 [6, 8) 加
/// [4, 6)。最终位置数值仍为 6，如果不检查 epoch，逻辑侧无法知道中间发生回绕。
/// 待应用 Seek 超过 R 时以 L 作为即时 UI 位置，但只有 process 后应用序列才
/// 追平。最后调度替换验证循环清除、控制确认与 generation 发布属于同一快照。
/// replacement 只比较 generation 是否变化，不假设全局起始值或固定增量。
bool testClockSnapshotLoopAndScheduleGeneration()
{
    using MMM::Audio::AudioTimelineMixerNode;
    using MMM::Audio::AudioTimelinePlaybackState;

    // 空片段足以验证 Transport 与时钟协议，不引入 PCM 混合噪声。
    AudioTimelineMixerNode node({}, 64, 8U);
    const auto             initial = node.clockSnapshot();
    if ( !node.setLoop({ 4, 8 }) ) return false;
    node.seek(6);
    node.play();

    // 四帧跨越一次循环边界，但首尾位置恰好相同，必须靠纪元识别跳转。
    ice::AudioBuffer loopBlock(ice::ICEConfig::internal_format, 4U);
    node.process(loopBlock);
    const auto looped = node.clockSnapshot();
    if ( !looped.valid || looped.positionFrame != 6 ||
         looped.state != AudioTimelinePlaybackState::Playing ||
         looped.epoch == looped.controlEpoch ||
         looped.scheduleGeneration != initial.scheduleGeneration ||
         looped.appliedSeekSequence != looped.seekSequence ||
         looped.appliedPlaybackSequence != looped.playbackSequence ) {
        XERROR("Loop block clock fields did not preserve the control boundary");
        return false;
    }

    // 超过 R 的待应用 Seek 在 UI 快照中也要按循环规则立即回到 L。
    node.seek(20);
    const auto pendingNormalizedSeek = node.clockSnapshot();
    if ( !pendingNormalizedSeek.valid ||
         pendingNormalizedSeek.positionFrame != 4 ||
         node.positionFrame() != 4 ||
         pendingNormalizedSeek.appliedSeekSequence ==
             pendingNormalizedSeek.seekSequence ) {
        XERROR("Pending looped Seek was not normalized to the loop start");
        return false;
    }

    // 两帧消费从规范化后的 L 前进到 6，并确认 Seek 已进入 Transport。
    ice::AudioBuffer afterSeekBlock(ice::ICEConfig::internal_format, 2U);
    node.process(afterSeekBlock);
    const auto appliedNormalizedSeek = node.clockSnapshot();
    if ( !appliedNormalizedSeek.valid ||
         appliedNormalizedSeek.positionFrame != 6 ||
         appliedNormalizedSeek.controlEpoch != appliedNormalizedSeek.epoch ||
         appliedNormalizedSeek.appliedSeekSequence !=
             appliedNormalizedSeek.seekSequence ) {
        XERROR("Normalized Seek acknowledgement fields were inconsistent");
        return false;
    }

    // 调度替换后的快照应以新 generation 区分，不能沿用旧调度身份。
    node.clearLoop();
    node.replaceSchedule({}, 64, 8U);
    node.process(afterSeekBlock);
    const auto replacement = node.clockSnapshot();
    if ( !replacement.valid ||
         replacement.scheduleGeneration == looped.scheduleGeneration ||
         replacement.controlEpoch != replacement.epoch ||
         replacement.appliedSeekSequence != replacement.seekSequence ||
         replacement.appliedPlaybackSequence != replacement.playbackSequence ) {
        XERROR("Replacement schedule clock fields were inconsistent");
        return false;
    }
    return true;
}

/// @brief 验证边界查询与紧随拉取共同形成一个不可插入控制命令的区间。
/// @return 首次拉取履行旧承诺、第二次拉取应用新 Seek 时返回 true。
///
/// prepareInputBoundary 返回后故意发布 Seek。紧随其后的同尺寸 process 必须仍从
/// 旧位置 0 消费四帧，再把位置查询合并为待应用目标 10。下一次边界查询才接收
/// Seek，并让随后的 blockStartFrame 与最终位置分别成为 10 和 14。
///
/// 这里分别断言 blockStartFrame 和 positionFrame：前者记录实际音频输入起点，
/// 后者允许叠加待应用控制意图。第一次拉取后两者有意不同，证明控制命令没有
/// 插入已封存的 PCM 段，同时界面也不必等待第二次拉取才反馈拖动位置。
/// 第二次 prepare 接收命令后，两者重新恢复为同一连续区间的起点与终点。
/// controlEpoch 与 epoch 最终相等，说明 Seek 是 block 起点控制而非块内跳转。
bool testPreparedBoundarySealsNextPull()
{
    MMM::Audio::AudioTimelineMixerNode node({}, 20, 8U);
    node.play();
    // 第一份边界承诺在控制命令到达前固定，长度与下一次 buffer 完全一致。
    const auto prepared = node.prepareInputBoundary(4U);
    if ( prepared.frameCount != 4U ||
         prepared.kind != MMM::Audio::AudioTimelineInputBoundaryKind::None ) {
        return false;
    }

    // 此 Seek 只能影响查询到的即时位置，不能改写已经承诺的 block 起点。
    node.seek(10);
    ice::AudioBuffer firstPull(ice::ICEConfig::internal_format, 4U);
    node.process(firstPull);
    if ( node.blockStartFrame() != 0 || node.positionFrame() != 10 ) {
        XERROR("Control command entered a previously prepared input segment");
        return false;
    }

    // 第二次 prepare 才应用 Seek，并封存从新位置开始的下一段输入。
    const auto afterSeek = node.prepareInputBoundary(4U);
    if ( afterSeek.frameCount != 4U ) return false;
    ice::AudioBuffer secondPull(ice::ICEConfig::internal_format, 4U);
    node.process(secondPull);
    // 第二次拉取既履行新承诺，也应在同一发布点确认 Seek 序列。
    const auto appliedSeek = node.clockSnapshot();
    return node.blockStartFrame() == 10 && node.positionFrame() == 14 &&
           appliedSeek.controlEpoch == appliedSeek.epoch &&
           appliedSeek.appliedSeekSequence == appliedSeek.seekSequence;
}

/// @brief 验证连续调度提交只在 block 起点启用最后一份完整状态。
/// @param track 用于构造不依赖被测节点的参考 PCM。
/// @param audio 由同一音轨准备出的时间线资源。
/// @return 输出只使用最后发布调度且位置连续时返回 true。
///
/// 节点先消费一个旧调度 block，再连续发布 0.25 与 0.75 两份替代调度，中间
/// 不调用 process。下一 block 必须完整采用 0.75 调度，不能混入旧状态或中间态。
/// 替换只改变片段表，不改变当前播放位置，因此参考源从第 8 帧开始读取。
///
/// 三份调度使用不同 sourceKey、eventId 与音量，使任何错误接管都能通过输出
/// 增益直接识别。预期片段数仍为一，证明元数据查询也指向最后发布状态。
/// 最终位置为十六且未结束，说明替换过程没有重置 Transport 或意外提交 final。
/// 本测试不依赖并发时序，而是确定性复现同一 block 前多个控制提交的合并规则。
/// 参考缓冲在新调度发布后才读取，但其数据来自独立 AudioTrack，不受调度影响。
bool testAtomicScheduleReplacement(
    const std::shared_ptr<ice::AudioTrack>&                         track,
    const std::shared_ptr<const MMM::Audio::PreparedTimelineAudio>& audio)
{
    constexpr std::size_t              BLOCK_FRAMES = 8U;
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 1U,
                .sourceKey  = "old",
                .startFrame = 0,
                .volume     = 0.1F,
                .audio      = audio,
            },
        },
        64,
        BLOCK_FRAMES);
    node.play();

    // 先推进位置，为验证替换状态继承当前 Transport 坐标建立前置条件。
    ice::AudioBuffer discarded(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    node.process(discarded);
    if ( node.positionFrame() !=
         static_cast<MMM::Audio::AudioTimelineFrame>(BLOCK_FRAMES) ) {
        XERROR("Timeline replacement precondition did not advance");
        return false;
    }

    // 第一份 pending 在音频线程接管前会被第二份完整替换。
    node.replaceSchedule(
        {
            {
                .eventId    = 2U,
                .sourceKey  = "superseded",
                .startFrame = 0,
                .volume     = 0.25F,
                .audio      = audio,
            },
        },
        64,
        BLOCK_FRAMES);
    // 最后一份发布必须成为下个 block 唯一可见的调度。
    node.replaceSchedule(
        {
            {
                .eventId    = 3U,
                .sourceKey  = "replacement",
                .startFrame = 0,
                .volume     = 0.75F,
                .audio      = audio,
            },
        },
        64,
        BLOCK_FRAMES);

    ice::AudioBuffer output(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    ice::AudioBuffer reference(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    reference.clear();
    // 新调度继承位置 8，所以参考 PCM 同样从源第 8 帧取样。
    track->read(reference, BLOCK_FRAMES, BLOCK_FRAMES);
    node.process(output);
    // 若读到 superseded 状态，样本增益会是 0.25；旧状态则会是 0.1。
    if ( !verifyMixedRange(output, reference, nullptr, 0U, 0.75F, 0.0F) ) {
        XERROR("Timeline block observed a partial or superseded schedule");
        return false;
    }
    return node.positionFrame() ==
               static_cast<MMM::Audio::AudioTimelineFrame>(BLOCK_FRAMES * 2U) &&
           node.clipCount() == 1U && !node.finished();
}

/// @brief 验证旧调度只在 block 接管新状态后由控制线程释放。
/// @param track 已完整缓存的测试音轨。
/// @return 验证通过时返回 true。
///
/// weak_ptr 用于观察资源生命周期而不延长它。调度发布前旧资源仍由 active 状态
/// 持有；音频 block 接管空调度后，旧状态进入 retired 栈但仍不能在回调中析构。
/// 只有显式 reclaimRetiredSchedules 才应让 weak_ptr 过期，重复回收返回零。
///
/// 生命周期检查分成四个明确阶段：
///
/// - 测试释放自己的 shared_ptr 后，active 调度仍持有资源；
/// - 发布新调度但尚未 process 时，回收函数必须返回零；
/// - process 完成指针交换后，资源仍由 retired 状态持有；
/// - 控制线程显式回收一个状态后 weak_ptr 才过期；
/// - 第二次回收返回零，证明同一节点没有重复入栈或重复释放。
///
/// 这比只检查回收数量更严格，也能发现 PCM 所有权被意外复制到其他长期对象。
/// 空替换调度自身没有资源所有权，因此回收后 weak_ptr 理应立刻失效。
bool testRetiredScheduleReclamation(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    constexpr std::size_t BLOCK_FRAMES = 8U;
    auto retiredAudio = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    if ( !retiredAudio ) return false;

    std::weak_ptr<const MMM::Audio::PreparedTimelineAudio> retiredAudioWeak =
        retiredAudio;
    // weak_ptr 在下列各阶段只用于 expired 查询，不会延长资源生命周期。
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 1U,
                .sourceKey  = "retired",
                .startFrame = 0,
                .volume     = 1.0F,
                .audio      = retiredAudio,
            },
        },
        64,
        BLOCK_FRAMES);
    // 移除测试侧强引用，此后资源是否存活完全由节点调度所有权决定。
    retiredAudio.reset();

    // 发布替换不等于音频线程已经接管，active 状态此刻仍必须存活。
    node.replaceSchedule({}, 64, BLOCK_FRAMES);
    if ( retiredAudioWeak.expired() || node.reclaimRetiredSchedules() != 0U ) {
        XERROR("Active schedule was reclaimed before the block boundary");
        return false;
    }

    ice::AudioBuffer output(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    // process 只把旧状态压入退役栈，实时线程禁止执行析构。
    node.process(output);
    if ( retiredAudioWeak.expired() ) {
        XERROR("Audio callback released a retired schedule");
        return false;
    }

    // reclaim 在非实时测试线程调用，对应生产中的低频控制轮询。
    // 控制线程回收后，旧状态及其最后一个 PreparedTimelineAudio 强引用被释放。
    if ( node.reclaimRetiredSchedules() != 1U || !retiredAudioWeak.expired() ) {
        XERROR("Control thread did not release the retired schedule");
        return false;
    }
    return node.reclaimRetiredSchedules() == 0U;
}

/// @brief 验证 BGM 区域与逐轨控制在下一 block 生效且不替换调度。
/// @return 默认、组合增益、静音与 generation 断言全部通过时返回 true。
///
/// 使用人工 PCM 可以直接按索引计算期望值。区域增益 0.5 与轨道增益 0.5 应
/// 相乘为 0.25；随后静音应让完整 block 为零。两次控制都不得重建调度，否则
/// 热路径参数调整会造成资源分配与播放状态抖动。
///
/// 人工样本由声道常量与帧递增量组成：
///
/// - 声道项让左右声道交换可见；
/// - 帧项让错误的 block 起点与源偏移可见；
/// - 首块单位增益验证默认控制不会改写内容；
/// - 第二块逐样本乘 0.25 验证区域和轨道增益组合；
/// - 第三块逐样本为零验证静音优先级；
/// - 全程 generation 不变，验证控制对象由实时路径直接读取；
/// - clipCount 保持一，验证参数更新未触发资源过滤或调度替换。
///
/// 测试不要求运行时控制在调用中的当前 block 中途生效，只要求下一次完整
/// process 使用同一个参数快照，这与实时混音的 block 粒度一致。
bool testLiveBgmTrackControlsKeepSchedule()
{
    constexpr std::size_t   BLOCK_FRAMES = 8U;
    constexpr std::size_t   TOTAL_FRAMES = BLOCK_FRAMES * 4U;
    constexpr std::uint32_t TRACK_INDEX  = 5U;

    // 每个声道和帧都使用不同值，可同时发现声道交换与源位置偏移错误。
    std::vector<std::vector<float>> channels(
        ice::ICEConfig::internal_format.channels,
        std::vector<float>(TOTAL_FRAMES));
    for ( std::size_t channel = 0U; channel < channels.size(); ++channel ) {
        for ( std::size_t frame = 0U; frame < TOTAL_FRAMES; ++frame ) {
            channels[channel][frame] = 0.1F * static_cast<float>(channel + 1U) +
                                       0.001F * static_cast<float>(frame);
        }
    }
    const auto audio = MMM::Audio::PreparedTimelineAudio::fromOwnedChannels(
        std::move(channels));
    // OwnedChannels 避免测试依赖外部样本长度，并保证所有 block 都有完整 PCM。
    if ( !audio ) return false;

    // 控制库生命周期覆盖节点，节点只持有稳定观察指针。
    MMM::Audio::KeySoundControlBank    controls;
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId       = 91U,
                .sourceKey     = "live-control",
                .startFrame    = 0,
                .bgmTrackIndex = TRACK_INDEX,
                .volume        = 1.0F,
                .audio         = audio,
            },
        },
        static_cast<MMM::Audio::AudioTimelineFrame>(TOTAL_FRAMES),
        BLOCK_FRAMES,
        &controls);
    node.play();

    ice::AudioBuffer first(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    // 默认控制为单位增益，第一 block 必须逐样本等于源数据。
    node.process(first);
    const auto scheduleGeneration = node.clockSnapshot().scheduleGeneration;
    for ( std::size_t channel = 0U; channel < first.num_channels();
          ++channel ) {
        const auto source = audio->channel(channel);
        for ( std::size_t frame = 0U; frame < BLOCK_FRAMES; ++frame ) {
            if ( std::abs(first.raw_ptrs()[channel][frame] - source[frame]) >
                 SAMPLE_EPSILON ) {
                XERROR("Default BGM track control changed the source sample");
                return false;
            }
        }
    }

    // 区域与轨道增益均在下一 block 读取，组合结果为 0.25。
    controls.setBgmAreaGain(0.5F);
    controls.setBgmTrackGain(TRACK_INDEX, 0.5F);
    ice::AudioBuffer gained(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    node.process(gained);
    for ( std::size_t channel = 0U; channel < gained.num_channels();
          ++channel ) {
        const auto source = audio->channel(channel);
        for ( std::size_t frame = 0U; frame < BLOCK_FRAMES; ++frame ) {
            const float expected = source[BLOCK_FRAMES + frame] * 0.25F;
            if ( std::abs(gained.raw_ptrs()[channel][frame] - expected) >
                 SAMPLE_EPSILON ) {
                XERROR("BGM gain control did not apply on the next block");
                return false;
            }
        }
    }

    // 参数更新不能通过 replaceSchedule 偷换整份时间线状态。
    if ( node.clockSnapshot().scheduleGeneration != scheduleGeneration ) {
        XERROR("BGM gain control unexpectedly replaced the timeline schedule");
        return false;
    }

    // 静音仍应推进时间线，只把有效运行时增益归零。
    controls.setBgmTrackMuted(TRACK_INDEX, true);
    ice::AudioBuffer muted(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    node.process(muted);
    // 静音块仍位于第三个源区间；逐零检查不能被资源本身的偶然零值替代。
    for ( std::size_t channel = 0U; channel < muted.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < BLOCK_FRAMES; ++frame ) {
            if ( muted.raw_ptrs()[channel][frame] != 0.0F ) {
                XERROR("BGM track mute did not apply on the next block");
                return false;
            }
        }
    }

    const auto afterMute = node.clockSnapshot();
    return afterMute.scheduleGeneration == scheduleGeneration &&
           node.clipCount() == 1U;
}

}  // namespace

/// @brief 运行实时多采样时间线混音测试。
/// @param argc 命令行参数数量。
/// @param argv argv[1] 为可解码且至少包含 64 帧的音频样本路径。
/// @return 全部独立协议场景通过时返回零。
///
/// 真实音轨只加载一次并在各场景间共享；节点本身在每个测试函数内重新构造，
/// 避免播放状态、循环、调度代次和退役资源跨场景污染。
/// 命令行缺少样本、资源加载失败或 PCM 无法冻结均属于测试夹具失败，立即返回一。
int main(int argc, char** argv)
{
    if ( argc < 2 ) {
        XERROR("Usage: AudioTimelineMixerNodeTest <sample_path>");
        return 1;
    }

    // AudioPool 与 ThreadPool 复用生产解码路径，验证 PreparedTimelineAudio
    // 集成。
    const std::filesystem::path samplePath(argv[1]);
    ice::ThreadPool             threadPool(2);
    ice::AudioPool              audioPool;
    auto track = audioPool.get_or_load(threadPool, samplePath.string()).lock();
    // 至少 64 帧保证重叠、Seek 与替换场景的参考读取均位于资源范围内。
    if ( !track || track->num_frames() < 64U ) {
        XERROR("Failed to prepare timeline mixer test sample");
        return 1;
    }
    const auto audio = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    if ( !audio ) {
        XERROR("Failed to freeze timeline mixer test PCM");
        return 1;
    }

    // track 用于独立真值，audio 用于节点调度；二者共享底层资源但读取路径不同。
    // 短路执行能保留首个失败场景的精确日志，并避免在破坏前置条件后继续运行。
    const bool passed =
        testOverlapNegativeStartAndSeek(track, audio) &&
        testHalfOpenLoop(track) && testMissingResourceAndFinish() &&
        testInputBoundaryAndFinishedReplacement() &&
        testImmediateStopThenPlay() && testCoherentTimelineClockSnapshot() &&
        testClockSnapshotLoopAndScheduleGeneration() &&
        testPreparedBoundarySealsNextPull() &&
        testAtomicScheduleReplacement(track, audio) &&
        testRetiredScheduleReclamation(track) &&
        testLiveBgmTrackControlsKeepSchedule();
    return passed ? 0 : 1;
}
