#pragma once

#include "common/VideoFrameDecoder.h"
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <stop_token>

namespace MMM::Canvas
{

/// @brief 带请求修订号的视频解码帧。
struct BackgroundVideoFrame {
    /// @brief FFmpeg 输出的 RGBA 视频帧。
    Utils::VideoFrame frame;

    /// @brief 生成该帧时对应的资源/Seek 代际。
    std::uint64_t requestGeneration{ 0 };

    /// @brief 目标时间是否已到达已知视频末尾。
    bool reachedEnd{ false };
};

/// @brief 在专用工作线程中按谱面时钟解码背景视频。
class BackgroundVideoPlayer
{
public:
    /// @brief 创建视频解码工作线程。
    BackgroundVideoPlayer();

    /// @brief 停止工作线程并释放解码器。
    /// @warning 低频生命周期路径：可能等待正在读取的本地视频包完成。
    ~BackgroundVideoPlayer();

    /// @brief 禁止复制解码线程和请求状态。
    BackgroundVideoPlayer(const BackgroundVideoPlayer&) = delete;

    /// @brief 禁止复制赋值解码线程和请求状态。
    BackgroundVideoPlayer& operator=(const BackgroundVideoPlayer&) = delete;

    /// @brief 禁止移动，避免工作线程捕获的对象地址失效。
    BackgroundVideoPlayer(BackgroundVideoPlayer&&) = delete;

    /// @brief 禁止移动赋值，避免工作线程捕获的对象地址失效。
    BackgroundVideoPlayer& operator=(BackgroundVideoPlayer&&) = delete;

    /// @brief 切换待解码的背景视频。
    /// @param path 视频绝对路径；空路径表示关闭。
    /// @return 本次资源切换的请求修订号。
    /// @warning 低频资源路径：只允许在背景路径或类型变化时调用。
    std::uint64_t setSource(const std::filesystem::path& path);

    /// @brief 请求最接近目标时间的视频帧。
    /// @param targetTime 视频内部时间，单位秒。
    /// @param startsNewGeneration 是否将该请求作为新的资源/Seek 代际。
    /// @return 本次请求所在代际。
    /// @warning UI 热路径：只更新 latest-wins 请求并短暂持锁，不执行解码。
    std::uint64_t requestFrame(double targetTime, bool startsNewGeneration);

    /// @brief 尝试取出最新完成的视频帧。
    /// @param frame 成功时接收视频帧。
    /// @return 取到新帧时返回 true。
    /// @warning UI 热路径：使用 try_lock，工作线程正在发布时立即返回。
    bool tryTakeLatestFrame(BackgroundVideoFrame& frame);

private:
    /// @brief 解码线程主循环。
    /// @param stopToken 生命周期停止令牌。
    /// @warning 专用后台线程：所有 FFmpeg 上下文都只能在该线程访问。
    void workerLoop(std::stop_token stopToken);

    /// @brief 保护 latest-wins 请求和已完成帧。
    std::mutex m_mutex;

    /// @brief 唤醒解码工作线程的条件变量。
    std::condition_variable m_condition;

    /// @brief 当前请求的视频资源路径。
    std::filesystem::path m_requestedSource;

    /// @brief 当前 latest-wins 目标时间。
    double m_requestedTime{ 0.0 };

    /// @brief 每次资源切换或时间请求递增的唤醒修订号。
    std::uint64_t m_requestRevision{ 0 };

    /// @brief 同一路径重新设置时也会递增的资源修订号。
    std::uint64_t m_sourceRevision{ 0 };

    /// @brief 资源切换或非连续 Seek 时递增的解码代际。
    std::uint64_t m_requestGeneration{ 0 };

    /// @brief 最新完成且尚未被 UI 消费的帧。
    std::optional<BackgroundVideoFrame> m_readyFrame;

    /// @brief 发布到 Runtime 共享线程池的视频解码任务。
    std::future<void> m_workerFuture;

    /// @brief 视频解码任务的协作式停止源。
    std::stop_source m_stopSource;
};

}  // namespace MMM::Canvas
