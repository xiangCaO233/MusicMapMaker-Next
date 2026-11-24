#include "ui/imgui/audio/WaveformAnalysis.h"

#include "log/colorful-log.h"

#include <ice/config/config.hpp>
#include <ice/manage/AudioFormat.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/manage/dec/IDecoderFactory.hpp>
#include <ice/manage/dec/IDecoderInstance.hpp>
#include <ice/manage/dec/MediaInfo.hpp>
#include <ice/thread/ThreadPool.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string_view>

namespace
{
/// @brief 使用恒幅长曲 PCM，测试跨过大量时间桶而且不依赖音频设备。
/// 后台任务扫描真实缓存，屏障只约束执行顺序，不将耗时作为通过阈值。
constexpr std::size_t DURATION{ 30U };

/// @brief 负幅第一首，使误交付在波形下界产生可观察值。
constexpr float FIRST_AMPLITUDE{ -0.25F };
/// @brief 正幅第二首，正式结果只有上界发生变化。
constexpr float SECOND_AMPLITUDE{ 0.5F };

/// @brief 支持完全缓存解码的内存 PCM 实例，各声道返回相同幅度。
class ConstantPCM : public ice::IDecoderInstance
{
public:
    /// @brief 模拟后端已把媒体转换为调用方所要求的内部格式。
    /// @param format 对所有声道使用同一内部采样率。
    /// @param amplitude 每次 read 产出的恒定有符号幅度。
    ConstantPCM(ice::AudioDataFormat format, float amplitude)
        : m_format(format), m_amplitude(amplitude)
    {
    }

    /// @brief 重置绝对帧游标，拒绝定位到数据范围之外。
    /// @param position 即将返回的首帧位置，单位为每声道帧数。
    /// @return 可继续读取请求位置时为真。
    bool seek(std::size_t position) override
    {
        // 末帧后一位是合法空区间，后续 read 返回零。
        if ( position > get_source_total_frames() ) return false;
        m_position = position;
        return true;
    }

    /// @brief 返回顺序 PCM，填满请求或可用帧后向下一位置推进。
    /// @param buffer 由真实缓存解码器分配的声道平面。
    /// @param requested 本次允许写入的每声道帧数。
    /// @return 实际提供帧数，游标越过数据区后保持零。
    std::size_t read(float** buffer, std::size_t requested) override
    {
        // 缓存扫描依赖真实 EOF 终止，不用请求次数推断音轨长度。
        const auto valid =
            std::min(requested, get_source_total_frames() - m_position);
        // 每个声道均为独立平面，避免从交错数据错误采样也能通过。
        for ( std::uint16_t ch = 0; ch < m_format.channels; ++ch )
            std::fill_n(buffer[ch], valid, m_amplitude);
        m_position += valid;
        return valid;
    }

    /// @brief 报告已经匹配内部格式的样本布局。
    /// @return 生命周期跟随单次解码实例的不可变格式。
    const ice::AudioDataFormat& get_source_format() const override
    {
        return m_format;
    }

    /// @brief 把曲长转换成实际 PCM 帧数。
    /// @details 假解码器使元信息与真实输出长度一致；长短偏差由专属回归验证。
    std::size_t get_source_total_frames() const override
    {
        return DURATION * m_format.samplerate;
    }

private:
    /// @brief 每个模拟实例的输出通道数和采样率。
    ice::AudioDataFormat m_format;
    /// @brief 同一首曲目的固定幅度，用于验证交付来源。
    float m_amplitude;
    /// @brief 下一次解码读取的绝对帧。
    std::size_t m_position{ 0U };
};

/// @brief 构造两条具有不同幅度的真实 ICE 缓存音轨，不依赖文件和音频设备。
class TestDecoderFactory : public ice::IDecoderFactory
{
public:
    /// @brief 只提供 AudioTrack 建立缓存所需的格式和时长元数据。
    /// @details 测试路径代表合成音源标识，因此探测不会访问磁盘。
    bool probe(std::string_view, ice::MediaInfo& info) const override
    {
        // 探测和解码报告相同布局，测试不经过重采样。
        info.format      = ice::ICEConfig::internal_format;
        info.frame_count = DURATION * info.format.samplerate;
        return true;
    }

    /// @brief 按音轨标识返回相反符号的解码信号。
    /// @param path 音轨标识只在构造时需要，模拟解码器随后独占自身游标和幅度。
    /// @param format 真实缓存解码期望的声道布局。
    std::unique_ptr<ice::IDecoderInstance> create_instance(
        std::string_view            path,
        const ice::AudioDataFormat& format) const override
    {
        // 首段峰值足以区分缓存来源，所以两首无需使用不同的时长。
        return std::make_unique<ConstantPCM>(
            format, path == "first" ? FIRST_AMPLITUDE : SECOND_AMPLITUDE);
    }
};

/// @brief 失败时保留检查点信息，便于区分生命周期错误与包络错误。
bool check(bool condition, const char* message)
{
    if ( !condition ) XERROR("{}", message);
    return condition;
}
}  // namespace

/// @brief 在计时无关的后台屏障期间模拟连续绘制、关闭项目以及换曲。
int main()
{
    // ICE 的工作者下限为四个，固定测试队列的实际并发度。
    // 解码独立于分析工作者，避免人为制造同池互等。
    ice::ThreadPool decodePool(4);
    ice::ThreadPool filePool(4);
    // 两个活跃音轨可共用一个无状态工厂，各次读取得到独立游标。
    const auto factory = std::make_shared<TestDecoderFactory>();
    auto       first   = ice::AudioTrack::create(
        "first", decodePool, factory, ice::CachingStrategy::CACHY);
    // 等长曲目能确保失效逻辑检查音轨身份，不能只依赖时长变化。
    auto second = ice::AudioTrack::create(
        "second", decodePool, factory, ice::CachingStrategy::CACHY);
    if ( !check(first && second, "cannot set up cached test tracks") ) return 1;

    // 占住前三个工作者，使最终放行的第四个工作者独占分析队列。
    // 同一工作者运行后续屏障时，前面的分析任务已经完成，毋须按 CPU 速度估计。
    // 失败返回时 promise 析构也会让 future 就绪，池析构可以排空任务。
    std::promise<void> hold;
    const auto         held = hold.get_future().share();
    // 测试固定四个工作者而非仅请求一个，防止 ICE 最小线程数使屏障提前完成。
    for ( int worker = 0; worker < 3; ++worker ) {
        auto blocker = filePool.enqueue([held] { held.wait(); });
        if ( !check(blocker.valid(), "file pool did not accept a blocker") )
            return 1;
    }
    // 第四个工作者在开始分析前等待 UI 执行关闭和切轨分支。
    std::promise<void> release;
    auto               gate =
        filePool.enqueue([unblock = release.get_future()] { unblock.wait(); });
    if ( !check(gate.valid(), "file pool did not accept a barrier") ) return 1;

    // 当前 UI 线程持有状态机，队列仍关闭时无需等待开始或完成通知。
    MMM::UI::WaveformAnalysis analysis;
    analysis.start(first, DURATION, {}, 7U, filePool, decodePool);
    // 正式绘制循环只观察 future，本线程必须在屏障仍关闭时继续下一步。
    analysis.poll(first, DURATION, 7U);
    bool passed =
        check(analysis.isRunning() && analysis.bins().empty(),
              "poll must return immediately while the waveform is pending");

    // 关闭窗口丢弃交付句柄，即使立即重开并请求相同音轨。
    analysis.cancel();
    // 正式绘制循环只观察 future，本线程必须在屏障仍关闭时继续下一步。
    analysis.poll(first, DURATION, 7U);
    passed &= check(!analysis.isRunning() && analysis.bins().empty(),
                    "closed request is still deliverable");
    // 擦除旧交付句柄后，相同来源允许立即重新提交。
    analysis.start(first, DURATION, {}, 7U, filePool, decodePool);

    // 时间线改成第二首立即使旧结果失效，后台仍保活其独立资源。
    analysis.poll(second, DURATION, 7U);
    passed &= check(!analysis.isRunning() && analysis.bins().empty(),
                    "track switch must cancel the pending first result");
    // 模拟工程卸载主动丢弃自己的强引用，后台不得借用 UI 中的句柄。
    first.reset();
    analysis.start(second, DURATION, {}, 7U, filePool, decodePool);
    // 放行时前两个请求已被取消，只有第二首应进行完整统计。
    release.set_value();

    // 同池栅栏保证排在其前方的结果已经完成；用有界等待定位任务未交付。
    // 此处等待用来证明结果早于之后的代次比较，正式 UI 不会等待屏障。
    // 只有剩余的同一工作者能消费此标记，完成顺序因此不会随调度变化。
    auto finished = filePool.enqueue([] {});
    if ( !check(finished.valid() && finished.wait_for(std::chrono::seconds(
                                        10)) == std::future_status::ready,
                "waveform worker did not drain") )
        return 1;
    // EQ 代次在后台计算期间变化；即使已完成也不能交付旧参数的包络。
    // 曲目身份仍然相同，唯一变化的参数代次足以拒绝后台结果。
    analysis.poll(second, DURATION, 8U);
    passed &=
        check(!analysis.isRunning() && analysis.bins().empty(),
              "a completed result with a stale EQ revision was published");

    // 新请求重新生成完整包络，证明过时结果的抛弃不妨碍再次同步。
    // 观察端销毁也必须在队列排空前返回，遗留任务不得访问视图或尝试交付。
    {
        MMM::UI::WaveformAnalysis closing;
        closing.start(second, DURATION, {}, 8U, filePool, decodePool);
    }

    // 即使前一观察端已销毁，当前实例也能独立接受它自己的完整结果。
    analysis.start(second, DURATION, {}, 8U, filePool, decodePool);
    finished = filePool.enqueue([] {});
    if ( !check(finished.valid() && finished.wait_for(std::chrono::seconds(
                                        10)) == std::future_status::ready,
                "replacement waveform worker did not drain") )
        return 1;
    analysis.poll(second, DURATION, 8U);
    // 左右声道上界必须匹配第二首，下界为零，不能混入第一首的负峰值。
    // 只在同一次 poll 后借用数组，不跨越清空或交付动作。
    const auto& bins = analysis.bins();
    passed &= check(
        !analysis.isRunning() && !bins.empty() && bins.front().minL == 0.0F &&
            bins.front().maxL == SECOND_AMPLITUDE &&
            bins.front().maxR == SECOND_AMPLITUDE,
        "only the current track and EQ generation may populate the view");
    // 已交付的包络也必须在切曲时作废，防止新窗口先绘制上一首的曲线。
    // 输入音轨释放不依赖下一次手工点击，空来源应在首次轮询生效。
    analysis.poll({}, 0.0, 8U);
    passed &= check(analysis.bins().empty(),
                    "released track left a visible envelope");
    return passed ? 0 : 1;
}
