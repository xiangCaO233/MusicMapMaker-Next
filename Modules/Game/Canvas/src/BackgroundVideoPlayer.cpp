#include "canvas/BackgroundVideoPlayer.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <cmath>
#include <ice/thread/ThreadPool.hpp>
#include <utility>

namespace MMM::Canvas
{

/// @brief 在应用共享线程池中启动单一视频解码循环。
///
/// 工作任务捕获 this，因此类禁止移动；对象销毁前会请求停止并等待任务退出。
/// 若线程池尚未初始化，播放器保持可调用但不会产生解码帧。
BackgroundVideoPlayer::BackgroundVideoPlayer()
{
    // 复用 Runtime 线程池，避免每个画布额外创建和管理原生线程。
    auto* appThreadPool = Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) {
        XERROR("Background video: Runtime thread pool is not initialized.");
        return;
    }
    // stop_token 与条件变量共同唤醒阻塞任务，确保析构无需等待新请求到来。
    const auto token = m_stopSource.get_token();
    m_workerFuture =
        appThreadPool->enqueue([this, token]() { workerLoop(token); });
}

/// @brief 协作式停止工作循环，并等待解码器在所属线程释放。
/// @warning 低频析构路径：若 FFmpeg 正在读取本地视频，wait 会等待该次调用返回。
BackgroundVideoPlayer::~BackgroundVideoPlayer()
{
    // 先发停止信号再通知条件变量，覆盖线程正在等待请求的常见状态。
    m_stopSource.request_stop();
    m_condition.notify_all();
    if ( m_workerFuture.valid() ) {
        // 解码器由 workerLoop 栈持有，必须等它 close 后才能销毁本对象成员。
        m_workerFuture.wait();
        // 显式清空 future，避免保留已经完成任务的共享状态。
        m_workerFuture = std::future<void>{};
    }
}

/// @brief 切换背景视频资源并使此前所有完成帧失效。
/// @param path 新视频路径；空路径表示停用背景视频。
/// @return 新资源所属的请求代际。
///
/// 资源修订与请求代际分别表达“需要重开解码器”和“旧帧不可再发布”。即使
/// 重复设置同一路径，也按一次显式资源重载处理。
std::uint64_t BackgroundVideoPlayer::setSource(
    const std::filesystem::path& path)
{
    // 请求字段与 readyFrame 在同一锁域内更新，UI 不会取到旧资源残留帧。
    std::lock_guard lock(m_mutex);
    m_requestedSource = path;
    m_requestedTime   = 0.0;
    m_readyFrame.reset();
    // sourceRevision 即使路径相同也递增，显式重新加载仍会重建解码器。
    ++m_sourceRevision;
    // generation 隔离资源或非连续 Seek，阻止旧解码结果跨代际发布。
    ++m_requestGeneration;
    ++m_requestRevision;
    // revision 是条件变量谓词使用的每请求唤醒序列。
    m_condition.notify_one();
    return m_requestGeneration;
}

/// @brief 更新 latest-wins 解码时间，并按需开启新的 Seek 代际。
/// @param targetTime 视频内部目标时间，单位秒。
/// @param startsNewGeneration 是否使正在处理的旧请求失去发布资格。
/// @return 请求更新后的代际。
///
/// 连续播放请求只覆盖目标时间；跳转请求提升 generation，使正在锁外执行的
/// 旧解码结果在提交前被丢弃。
std::uint64_t BackgroundVideoPlayer::requestFrame(double targetTime,
                                                  bool   startsNewGeneration)
{
    std::lock_guard lock(m_mutex);
    // 非有限时间既不能交给 FFmpeg，也不能推进修订号唤醒无效工作。
    if ( !std::isfinite(targetTime) ) {
        return m_requestGeneration;
    }
    m_requestedTime = targetTime;
    if ( startsNewGeneration ) {
        // 连续播放沿用代际，拖动或跳转则递增以淘汰途中完成的旧帧。
        ++m_requestGeneration;
    }
    ++m_requestRevision;
    // 多次快速请求只保存最后时间，工作线程醒来后自然跳过中间帧。
    m_condition.notify_one();
    return m_requestGeneration;
}

/// @brief 非阻塞地把最新完成帧移动给 UI 线程。
/// @param frame 成功时接收完成帧及其请求代际。
/// @return 锁忙或没有新帧时返回 false。
///
/// 完成槽只保留一帧，工作线程的新结果会覆盖尚未消费的旧结果，符合视频
/// 播放宁可丢帧也不能阻塞 UI 的 latest-wins 策略。
bool BackgroundVideoPlayer::tryTakeLatestFrame(BackgroundVideoFrame& frame)
{
    // UI 热路径绝不等待解码线程发布，锁竞争时下一帧再尝试。
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if ( !lock.owns_lock() || !m_readyFrame ) {
        return false;
    }
    // move 转移较大的像素缓冲区，随后清空槽位表示该帧已被消费。
    frame = std::move(*m_readyFrame);
    m_readyFrame.reset();
    return true;
}

/// @brief 依次处理最新资源和时间请求，并发布仍属于当前代际的帧。
/// @param stopToken 播放器析构时触发的协作式停止令牌。
///
/// 解码器只存在于本函数所属线程。共享锁仅用于复制请求和提交结果，耗时的
/// open/decode/close 全部在锁外执行，保证 UI 请求与取帧不会等待磁盘解码。
/// requestRevision 控制是否需要处理，sourceRevision 控制解码器生命周期，
/// requestGeneration 则控制结果能否发布，三者不可合并为单一计数器。
void BackgroundVideoPlayer::workerLoop(std::stop_token stopToken)
{
    // FFmpeg 上下文固定在线程栈上，避免跨线程访问其内部状态。
    Utils::VideoFrameDecoder decoder;
    std::filesystem::path    activeSource;
    std::uint64_t            activeSourceRevision    = 0;
    std::uint64_t            handledRevision         = 0;
    std::uint64_t            lastPublishedGeneration = 0;
    double                   lastPublishedTimestamp  = 0.0;
    bool                     hasPublishedTimestamp   = false;
    bool                     lastPublishedReachedEnd = false;

    while ( !stopToken.stop_requested() ) {
        // 每轮先准备请求副本，离开锁域后只使用该轮一致快照。
        // 局部变量不跨轮保留，确保每次唤醒都完整读取最新请求状态。
        std::filesystem::path requestedSource;
        double                requestedTime     = 0.0;
        std::uint64_t         requestRevision   = 0;
        std::uint64_t         sourceRevision    = 0;
        std::uint64_t         requestGeneration = 0;
        {
            std::unique_lock lock(m_mutex);
            // 谓词防止虚假唤醒；停止请求与新修订号都能结束等待。
            m_condition.wait(lock, [this, &stopToken, &handledRevision]() {
                return stopToken.stop_requested() ||
                       m_requestRevision != handledRevision;
            });
            if ( stopToken.stop_requested() ) {
                break;
            }
            // 在一次锁域内复制全部字段，避免时间与资源修订来自不同请求。
            requestedSource   = m_requestedSource;
            requestedTime     = m_requestedTime;
            requestRevision   = m_requestRevision;
            sourceRevision    = m_sourceRevision;
            requestGeneration = m_requestGeneration;
        }

        if ( sourceRevision != activeSourceRevision ) {
            // 资源修订变化时先关闭旧解码器，并清除跨资源发布去重状态。
            decoder.close();
            activeSource            = requestedSource;
            activeSourceRevision    = sourceRevision;
            hasPublishedTimestamp   = false;
            lastPublishedGeneration = 0;
            lastPublishedReachedEnd = false;
            if ( !activeSource.empty() && !decoder.open(activeSource) ) {
                // 打开失败视为该修订已处理，等待显式新请求而不是忙循环重试。
                handledRevision = requestRevision;
                continue;
            }
        }

        if ( activeSource.empty() || !decoder.isOpen() ) {
            // 空资源是正常停用状态，同样推进 handledRevision 进入条件等待。
            handledRevision = requestRevision;
            continue;
        }

        // 解码发生在共享锁外；期间 UI 可以覆盖 latest-wins 请求。
        const Utils::VideoFrame* decodedFrame =
            decoder.decodeFrameAt(requestedTime);
        const bool reachedEnd = decoder.info().duration > 0.0 &&
                                requestedTime >= decoder.info().duration;
        // duration 未知或非正时不猜测末尾，交由解码器返回值决定是否有帧。
        const bool frameChanged =
            // 同时间戳在新代际或末尾状态变化时仍需重新发布。
            decodedFrame && (!hasPublishedTimestamp ||
                             std::abs(decodedFrame->timestamp -
                                      lastPublishedTimestamp) > 1e-6 ||
                             requestGeneration != lastPublishedGeneration ||
                             reachedEnd != lastPublishedReachedEnd);
        bool requestMayPublish = false;
        {
            // 首次短锁检查耗时解码期间资源或 Seek 代际是否已经变化。
            std::lock_guard lock(m_mutex);
            requestMayPublish = sourceRevision == m_sourceRevision &&
                                requestGeneration == m_requestGeneration;
        }
        if ( requestMayPublish && frameChanged ) {
            // 先在锁外复制解码帧，缩短最终发布锁持有时间。
            BackgroundVideoFrame completedFrame{
                // VideoFrame 按值复制出解码器内部结果，避免下一次 decode 覆盖。
                .frame             = *decodedFrame,
                .requestGeneration = requestGeneration,
                .reachedEnd        = reachedEnd,
            };
            std::lock_guard lock(m_mutex);
            // 取得发布锁后再次验证，封闭首次检查与提交之间的竞态窗口。
            if ( sourceRevision == m_sourceRevision &&
                 requestGeneration == m_requestGeneration ) {
                m_readyFrame = std::move(completedFrame);
                // 发布元数据只在帧成功提交后更新，失败请求不会抑制后续帧。
                lastPublishedTimestamp  = decodedFrame->timestamp;
                hasPublishedTimestamp   = true;
                lastPublishedGeneration = requestGeneration;
                lastPublishedReachedEnd = reachedEnd;
            }
        }
        // 若解码途中到达更新请求，本轮结果被丢弃，下轮直接处理最新修订。
        // 即使没有帧变化也标记请求已处理，等待下一修订而非重复解码。
        handledRevision = requestRevision;
    }

    // 在工作线程退出前关闭解码器，维持 FFmpeg 上下文的线程归属。
    // close 在空解码器上同样安全，使所有退出路径汇合到统一清理点。
    decoder.close();
}

}  // namespace MMM::Canvas
