#include "WaveformAnalysis.h"

#include "log/colorful-log.h"

#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 波形离线 EQ 单次处理的最大帧数，限制独立工作缓冲的容量。
constexpr std::size_t WAVEFORM_EQ_BLOCK_FRAMES{ 44100U };

/// @brief 根据请求持有的音轨、时长和 EQ 统计整曲包络。
/// @param track 音轨句柄按值接管，可替换为仅供分析的完整解码音轨。
/// @param duration 工程音频时长，单位为秒。
/// @param eqSettings 不可变的当前主轨 EQ 参数。
/// @param stop 用于在准备解码前或每个时间桶之间协作取消。
/// @param decodePool 独立于执行本函数的线程池，避免同池等待自身任务。
/// @warning 后台文件任务入口。流式音轨重新打开文件并等待完整解码，不能由
/// 绘制线程调用。已进入解码时，协作停止在解码返回后生效。
std::vector<WaveformBin> calculateWaveform(
    std::shared_ptr<ice::AudioTrack> track, double duration,
    const Audio::OfflineAnalysisEQSettings& eqSettings, std::stop_token stop,
    ice::ThreadPool& decodePool)
{
    // 立即关闭后仍可能轮到已提交的任务；在获取 PCM 前检查令牌，
    // 以免为已经不存在的观察端打开文件或排入解码。
    if ( stop.stop_requested() ) return {};
    if ( track->cachingStrategy() == ice::CachingStrategy::STREAMING ) {
        // 播放页缓存缺页时会返回静音；独立音轨避免扰动播放游标或争用预读页。
        // 本地池不依赖 AudioManager 的销毁顺序，完全缓存解码在通用池执行。
        ice::AudioPool analysisPool;
        track = analysisPool
                    .get_or_load(
                        decodePool, track->path(), ice::CachingStrategy::CACHY)
                    .lock();
        // 局部 AudioPool 会在此分支结束时释放；任务自己的句柄继续持有音轨。
        if ( !track ) {
            XERROR("Failed to open waveform analysis track.");
            return {};
        }
    }

    // 等待真实帧数只占用后台线程，并在全曲缓冲分配之前检查解码失败与取消。
    // 复合音轨投影仍沿用工程时长；读到的 PCM 来自兼容接口给出的第一条有效音轨。
    // 解码先完成，后续逐桶 read 只复制缓存，不触发磁盘读取。
    if ( track->num_frames() == 0U || stop.stop_requested() ) return {};
    // ICE 内部输出格式在运行前确定；等值参数用于读入、滤波和帧边界换算。
    const auto   format     = ice::ICEConfig::internal_format;
    const double sampleRate = format.samplerate;

    // 时长来自复合音轨；末端额外点保留既有桶边界语义。
    // 每个桶一次性值初始化为静音，提交前均由本任务独占，没有中间发布。
    std::vector<WaveformBin> bins(
        static_cast<std::size_t>(duration *
                                 WaveformAnalysis::POINTS_PER_SECOND) +
        1U);

    // 等效滤波器在任务内创建，递推状态不与播放或其他分析请求共享。
    Audio::OfflineAnalysisEQ eq(eqSettings, sampleRate, format.channels);
    // 预留整块容量。读取只写有效前缀，随后缩短活动帧数供原位 EQ 消费。
    // set_active_frames 不改变通道步长或底层存储，下一桶可以再读满整块。
    ice::AudioBuffer buffer;
    if ( !buffer.resize(format, WAVEFORM_EQ_BLOCK_FRAMES) ) return {};

    for ( std::size_t p = 0; p < bins.size(); ++p ) {
        // 取消后不发布部分结果。固定桶大小限制扫描阶段的取消延迟。
        if ( stop.stop_requested() ) return {};

        // 起止位置分别由整数桶坐标换算，避免累加秒数造成长曲边界漂移。
        // 相邻桶沿用同一端点的舍入策略，没有重复计算一个边界样本。
        const double startTime = p / WaveformAnalysis::POINTS_PER_SECOND;
        const double endTime   = (p + 1U) / WaveformAnalysis::POINTS_PER_SECOND;
        const auto   startFrame =
            static_cast<std::size_t>(startTime * sampleRate);
        const auto endFrame = static_cast<std::size_t>(endTime * sampleRate);
        // 半开区间没有样本时保留零基线；正常区间不能超过临时缓冲。
        if ( endFrame <= startFrame ) continue;
        const auto frames =
            std::min(endFrame - startFrame, WAVEFORM_EQ_BLOCK_FRAMES);

        // 按桶的绝对帧位置读取，不修改任何正在播放的节点游标。
        // 短读只覆盖开头；空桶保持零基线，也不允许旧采样更新滤波器历史。
        const auto validFrames = track->read(buffer, startFrame, frames);
        if ( validFrames == 0 || !buffer.set_active_frames(validFrames) )
            continue;
        // 使用与包络统计相同的有效区间，避免对上个桶留下的尾部二次滤波。
        eq.process(buffer);

        // 极值从零开始包含静音基线；单声道复用左声道，忽略左右以外的声道。
        auto&  bin  = bins[p];
        auto** data = buffer.raw_ptrs();
        for ( std::size_t f = 0; f < validFrames; ++f ) {
            const auto left  = data[0][f];
            const auto right = buffer.num_channels() > 1 ? data[1][f] : left;
            bin.minL         = std::min(bin.minL, left);
            bin.maxL         = std::max(bin.maxL, left);
            bin.minR         = std::min(bin.minR, right);
            bin.maxR         = std::max(bin.maxR, right);
        }
    }
    // 返回值转移整个时间序列的所有权；只有观察端仍有对应 future 才能接收。
    return bins;
}

}  // namespace

/// @brief 销毁 UI 观察端，后台任务仅保留独立捕获的输入与令牌。
/// @details 关闭时没有隐式 join，也不需要视图对象续命直到解码结束。
WaveformAnalysis::~WaveformAnalysis()
{
    cancel();
}

/// @brief 注册新的交付句柄并发起后台统计。
/// @details 即使两次请求指向同一音轨，后者也有独立的停止令牌；先前的结果
/// 不能覆盖后来的请求。原有完整数据可继续供同一音轨使用。
void WaveformAnalysis::start(const std::shared_ptr<ice::AudioTrack>& track,
                             double                                  duration,
                             Audio::OfflineAnalysisEQSettings        eq,
                             std::uint64_t                           eqRevision,
                             ice::ThreadPool&                        filePool,
                             ice::ThreadPool&                        decodePool)
{
    // 拒绝无音轨或无法用于整段桶计数的时长，不污染现有包络。
    if ( !track || !std::isfinite(duration) || duration <= 0.0 ) return;
    // 给最后一个桶的右端预留一秒，保证秒数转为帧位置时可表示。
    const double maxDuration =
        static_cast<double>(std::numeric_limits<std::size_t>::max()) /
            ice::ICEConfig::internal_format.samplerate -
        1.0;
    if ( duration >= maxDuration ) return;

    // 新来源先清除原包络；同来源重新同步仅替换待处理请求。
    retarget(track, duration);
    cancel();
    // 修订号留在 UI，用于判定交付资格，后台只需冻结后的滤波器设置。
    m_eqRevision = eqRevision;
    m_stopSource = std::stop_source{};
    // 后台只持有音轨、参数和令牌；唯一借用项是比文件池后关闭的解码池。
    m_future = filePool.enqueue([track,
                                 duration,
                                 eq   = std::move(eq),
                                 stop = m_stopSource.get_token(),
                                 &decodePool]() mutable {
        // 任务既不捕获视图，也不捕获 AudioManager。
        return calculateWaveform(
            std::move(track), duration, eq, stop, decodePool);
    });
}

/// @brief 检查来源与参数后，取走当前请求的完成结果。
/// @details future 是唯一交付渠道，后台不会直接写 UI 观察状态。
/// @warning UI 每帧调用，轮询等待必须为零，只有 ready 时才可 get。
void WaveformAnalysis::poll(const std::shared_ptr<ice::AudioTrack>& track,
                            double duration, std::uint64_t eqRevision)
{
    retarget(track, duration);
    // 即使结果已就绪，也要先判定当前参数是否仍匹配其提交代次。
    if ( m_eqRevision != eqRevision ) cancel();
    // 空句柄表示没有有效交付资格，运行中的句柄只能做零超时轮询。
    if ( !m_future.valid() || m_future.wait_for(std::chrono::seconds::zero()) !=
                                  std::future_status::ready )
        return;

    // 整批移交缓存，后续切片不会读到只统计了部分时间桶的版本。
    auto bins = m_future.get();
    // 空结果表示取消或解码失败，继续显示当前来源已经接收的完整包络。
    if ( !bins.empty() ) m_bins = std::move(bins);
}

/// @brief 无同步等待地放弃未交付结果。
/// @details 只修改 UI 的 future；停止令牌供队列中和正在扫描的任务独立检查。
void WaveformAnalysis::cancel()
{
    if ( !m_future.valid() ) return;
    // 先标记停止，仍在排队的任务不必建立解码器；正在扫描的任务在桶边界退出。
    m_stopSource.request_stop();
    // ThreadPool 提供 packaged_task 的 future；释放观察端不阻塞或访问 PCM。
    m_future = {};
}

/// @brief 取消请求并忘记旧来源，下一帧只能绘制空包络或新来源。
/// @details 清空缓存长度但复用容器，避免持续过渡帧反复释放和重建存储。
void WaveformAnalysis::reset()
{
    cancel();
    m_track.reset();
    m_duration = 0.0;
    // 仅缩短容器长度，无需在重新定向时触及旧 PCM 或释放解码资源。
    m_bins.clear();
}

/// @brief 为已选择的音轨身份和时长维护统一包络缓存。
/// @warning UI 每帧调用；稳定时仅比较弱引用身份和标量。
void WaveformAnalysis::retarget(const std::shared_ptr<ice::AudioTrack>& track,
                                double duration)
{
    // 比较所有权标识可区分随后在同一地址分配的两个不同音轨。
    if ( !m_track.owner_before(track) && !track.owner_before(m_track) &&
         duration == m_duration )
        return;
    reset();
    // 弱引用不延长音轨生命周期，窗口长期打开时也不阻碍音频池回收。
    m_track    = track;
    m_duration = duration;
}

}  // namespace MMM::UI
