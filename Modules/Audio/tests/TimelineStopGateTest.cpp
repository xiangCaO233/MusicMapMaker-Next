#include "audio/SoundEffectPool.h"
#include "audio/TimelineStopGateNode.h"
#include "log/colorful-log.h"
#include <cmath>
#include <ice/config/config.hpp>
#include <ice/core/MixBus.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <memory>
#include <vector>

namespace
{
/// @brief 经过真实全局拉伸器比较有无 STOP 的输出时长。
/// @param ratio 歌曲与 Key 共用的播放倍率。
/// @param withStop 是否在中间插入半秒等待。
/// @return 最后一个有效输出样本的索引，失败返回零。
/// @note 不接真实设备，不依赖睡眠或机器运行速度。
std::size_t stretchedEnd(double ratio, bool withStop)
{
    using namespace MMM::Audio;
    // PCM 长度按帧声明，和设备实际采样率无关；预期差值也使用帧。
    // 使用相同输入数据和相同算法配置，唯一自变量是 STOP。
    // 不比较有损拉伸后的逐样本幅值，避免把算法相位变化当作暂停错误。
    // 最后非静音样本能观察到真正的输出长度，而不是仅查看输入游标。
    // 正弦幅度显著高于判零阈值，短暂过零不会使整个尾部被误判。
    // 阈值过滤极小算法尾噪，窗口容差随后覆盖尾部长度的微小变化。
    // half-second 的语义由生产采样率换算，这里直接测试等价的输入帧预算。
    constexpr std::size_t FRAMES = 48000, BLOCK = 256;
    // 连续正弦避免变速算法对直流信号的特殊处理。
    // 半秒等待前后均保留足够音频，观察实际输出而非仅查传输状态。
    std::vector<std::vector<float>> channels(2, std::vector<float>(FRAMES));
    for ( auto& channel : channels )
        for ( std::size_t i = 0; i < FRAMES; ++i )
            channel[i] = .2F * std::sin(static_cast<double>(i) * .07);
    auto pcm = PreparedTimelineAudio::fromOwnedChannels(std::move(channels));
    auto timeline = std::make_shared<AudioTimelineMixerNode>(
        std::vector<PreparedTimelineClip>{}, FRAMES, BLOCK * 16);
    std::vector<TimelineStopEvent> stops;
    if ( withStop ) stops.push_back({ 24000, 24000 });
    timeline->replaceSchedule(
        { { .eventId = 1, .startFrame = 0, .audio = pcm } },
        FRAMES,
        BLOCK * 16,
        std::move(stops));
    auto gate = std::make_shared<TimelineStopGateNode>(timeline, *timeline);
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(gate);
    if ( !stretcher.prepare(ice::ICEConfig::internal_format, BLOCK) ) return 0;
    stretcher.set_playback_ratio(ratio);
    // 回调协议与 AudioManager 相同：短块限制源消耗，末端刷新算法尾部。
    // 这里直接借用测试生命周期内稳定的时间线地址。
    stretcher.set_input_boundary_provider(
        timeline.get(), [](void* context, std::size_t maximum) noexcept {
            const auto boundary = static_cast<AudioTimelineMixerNode*>(context)
                                      ->prepareInputBoundary(maximum);
            return ice::TimeStretcher::InputSpan{
                boundary.frameCount,
                boundary.kind == AudioTimelineInputBoundaryKind::Final
                    ? ice::TimeStretcher::InputBoundary::Final
                    : ice::TimeStretcher::InputBoundary::None
            };
        });
    // 播放前安装所有回调和上游引用，避免测试中途修改音频图。
    // STOP 不改变 epoch，拉伸器不会因等待而清空未播放的输入历史。
    // 末端 Final 单独标记，允许算法把已经读取的尾部输出完毕。
    // 输入短块限制仍然有效，即使拉伸器一次请求跨过等待边界。
    // 对无 STOP 的对照组保持完全相同的边界协议。
    timeline->play();
    std::size_t      last = 0;
    ice::AudioBuffer output(ice::ICEConfig::internal_format, BLOCK);
    // 最慢倍率也可在固定预算内播放结束；这不是按墙钟等待。
    // 固定块数为最慢倍率留足余量，结束后输出静音不再更新 last。
    // 每轮 process 都用同一缓冲，必须由生产节点覆盖旧内容。
    // 不用 finished 标志提前停止，因为拉伸器可能仍有算法尾部。
    // 循环上限防止错误状态导致测试永久等待。
    // 所有处理都在测试线程，结果不受线程调度或设备时钟影响。
    // 若未输出任何有效音频，零返回值会使调用方明确失败。
    // 若 STOP 被漏掉，两组长度相同，同样不能通过差值断言。
    for ( std::size_t block = 0; block < 1400; ++block ) {
        stretcher.process(output);
        for ( std::size_t i = 0; i < BLOCK; ++i )
            if ( std::abs(output.raw_ptrs()[0][i]) > .0001F )
                last = block * BLOCK + i;
    }
    return last;
}
}  // namespace

/// @brief 用真实 PCM 验证 BGM 与两个独立 Key 音源同时冻结和准确续播。
/// @details 不使用设备计时或 sleep；直接按真实图顺序执行音频块。
int main()
{
    // 测试不创建系统设备，输入输出都由确定性帧数驱动。
    // 时间线、混音器、Key 池均使用生产实现。
    using namespace MMM::Audio;
    // 非常量序列能发现“静音期间继续消耗 PCM”的错误。
    std::vector<std::vector<float>> channels(2, std::vector<float>(4096));
    for ( auto& channel : channels )
        for ( std::size_t i = 0; i < channel.size(); ++i )
            channel[i] = .05F + i * .00001F;
    const auto pcm =
        PreparedTimelineAudio::fromOwnedChannels(std::move(channels));
    // Timeline 的区间长度足够容纳全部测试块，不能提前自然结束。
    // 资源共享只发生在准备阶段，音频处理阶段不更换所有者。
    auto timeline = std::make_shared<AudioTimelineMixerNode>(
        std::vector<PreparedTimelineClip>{}, 4096, 64);
    std::vector<PreparedTimelineClip> clips{
        { .eventId = 1, .startFrame = 0, .audio = pcm }
    };
    // 相同位置的两个 STOP 必须累计等待；第三个暂停检验继续推进后的再次触发。
    timeline->replaceSchedule(
        std::move(clips), 4096, 64, { { 8, 3 }, { 8, 2 }, { 20, 4 } });
    // Gate 位于整条音频图的入口，必须同时阻止三个子源推进。
    // 单独暂停 BGM 或仅清空混音输出都会被斜坡期望值检测到。
    auto mixer = std::make_shared<ice::MixBus>();
    mixer->prepare(ice::ICEConfig::internal_format, 64);
    mixer->add_source(timeline);
    // 两个池代表默认 Key 和独立文件绑定；二者拥有独立播放游标。
    // 同一 PCM 数据用于简化可人工核对的三倍幅度期望。
    SoundEffectPool key(pcm), customKey(pcm);
    mixer->add_source(key.getMixer());
    mixer->add_source(customKey.getMixer());
    TimelineStopGateNode gate(mixer, *timeline);
    key.play(1.0F);
    customKey.play(1.0F);
    timeline->play();
    // 音乐帧只在非暂停区间累加，输出静音帧单独计数。
    // 这两个量不同正是 STOP 的时间模型，不能共用一个总帧计数。
    std::size_t musicFrame = 0, silentFrames = 0;
    // 任意非整齐块长模拟拉伸器的输入请求，不能依赖 STOP 对齐设备块。
    for ( int block = 0; block < 18; ++block ) {
        // 先查询边界再拉取，和全局拉伸器的输入协议相同。
        // 返回短块时必须尊重短块长度，不能跨过 STOP 起点。
        const auto boundary = timeline->prepareInputBoundary(7);
        if ( boundary.frameCount == 0 || boundary.frameCount > 7 ) return 1;
        ice::AudioBuffer output(ice::ICEConfig::internal_format,
                                boundary.frameCount);
        gate.process(output);
        // 输出之后读取已发布快照，验证 UI 可观察到等待状态。
        // 零样本不会来自输入斜坡，所以静音判断无歧义。
        const bool frozen = timeline->clockSnapshot().stopFrozen;
        for ( std::size_t i = 0; i < output.num_frames(); ++i ) {
            const float expected =
                frozen ? 0 : 3 * (.05F + (musicFrame + i) * .00001F);
            if ( std::abs(output.raw_ptrs()[0][i] - expected) > 1e-5F ) {
                XERROR("STOP PCM mismatch at music frame {}: {} != {}",
                       musicFrame + i,
                       output.raw_ptrs()[0][i],
                       expected);
                return 2;
            }
        }
        // 静音等待不能推动歌曲游标，续播则必须从未消费的帧开始。
        // 每块检查游标，避免只检查末尾时互相抵消的错误。
        if ( frozen )
            silentFrames += boundary.frameCount;
        else
            musicFrame += boundary.frameCount;
        if ( timeline->positionFrame() !=
             static_cast<AudioTimelineFrame>(musicFrame) )
            return 3;
    }
    // 同刻 3+2 帧与后续 4 帧应全部消费且仅消费一次。
    // 除总等待长度之外，前面的逐帧断言还检验了各个边界。
    if ( silentFrames != 9 ) return 4;
    // 回到暂停位置应重新触发，不沿用上一次已消费的 STOP 索引。
    // Seek 建立新的 transport epoch，旧 STOP 余量不能污染新位置。
    // 本次恰好定位到边界，因此应完整触发第一项。
    timeline->seek(8);
    auto             boundary = timeline->prepareInputBoundary(7);
    ice::AudioBuffer silence(ice::ICEConfig::internal_format,
                             boundary.frameCount);
    gate.process(silence);
    if ( boundary.frameCount != 3 || timeline->positionFrame() != 8 ||
         !timeline->clockSnapshot().stopFrozen )
        return 5;
    // 暂停再继续应保留剩余事件，不重新播放已经完成的同刻第一项。
    // 用户暂停不属于 Seek，恢复时不能再次触发已经完成的第一项。
    // 同刻第二项的两帧等待应原样保留。
    timeline->pause();
    if ( timeline->prepareInputBoundary(7).frameCount != 0 ) return 6;
    timeline->play();
    if ( timeline->prepareInputBoundary(7).frameCount != 2 ) return 7;
    // 同一倍率比较差值，抵消拉伸器固定启动与尾部延迟。
    // 允许一个分析窗误差，但不能把等待漏掉或重复乘倍率。
    for ( const double ratio : { 0.25, 1.0, 2.0 } ) {
        const auto   plain    = stretchedEnd(ratio, false);
        const auto   stopped  = stretchedEnd(ratio, true);
        const double expected = 24000 / ratio;
        if ( plain == 0 || stopped <= plain ||
             std::abs(static_cast<double>(stopped - plain) - expected) >
                 2048 ) {
            XERROR("STOP stretched duration mismatch at {}: {} vs {}",
                   ratio,
                   stopped - plain,
                   expected);
            return 8;
        }
    }
    XINFO("STOP freezes BGM, Key and custom Key PCM without consuming samples");
    return 0;
}
