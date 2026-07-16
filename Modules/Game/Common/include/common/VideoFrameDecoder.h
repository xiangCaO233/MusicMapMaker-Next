#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace MMM::Utils
{

/// @brief 可供视频背景加载流程使用的基础视频信息。
struct VideoInfo {
    /// @brief 编码视频帧宽度，单位像素。
    std::uint32_t width{ 0 };

    /// @brief 编码视频帧高度，单位像素。
    std::uint32_t height{ 0 };

    /// @brief 视频可播放时长，单位秒；容器未提供时为零。
    double duration{ 0.0 };
};

/// @brief 已转换为 RGBA8 的单帧视频数据。
struct VideoFrame {
    /// @brief 连续排列的 RGBA8 像素。
    std::vector<std::uint8_t> rgba;

    /// @brief 当前帧宽度，单位像素。
    std::uint32_t width{ 0 };

    /// @brief 当前帧高度，单位像素。
    std::uint32_t height{ 0 };

    /// @brief 当前帧相对视频起点的显示时间，单位秒。
    double timestamp{ 0.0 };
};

/// @brief 探测本地视频文件的尺寸与时长。
/// @param filePath 待探测视频路径。
/// @return 成功时返回视频信息，文件或视频流不可用时返回空值。
/// @warning 低频阻塞路径：会同步访问文件并初始化 FFmpeg
/// 解码器，不得在渲染热路径调用。
std::optional<VideoInfo> probeVideoInfo(const std::filesystem::path& filePath);

/// @brief 面向时间点请求的同步视频帧解码器。
/// @warning 解码器不提供内部线程安全，同一实例必须由单一调用方串行访问。
class VideoFrameDecoder final
{
public:
    /// @brief 创建尚未打开媒体的解码器。
    VideoFrameDecoder();

    /// @brief 关闭媒体并释放全部 FFmpeg 资源。
    ~VideoFrameDecoder();

    /// @brief 禁止复制 FFmpeg 解码状态。
    VideoFrameDecoder(const VideoFrameDecoder&) = delete;

    /// @brief 禁止复制赋值 FFmpeg 解码状态。
    VideoFrameDecoder& operator=(const VideoFrameDecoder&) = delete;

    /// @brief 禁止移动，避免调用方持有的同步解码状态突然失效。
    VideoFrameDecoder(VideoFrameDecoder&&) = delete;

    /// @brief 禁止移动赋值，避免调用方持有的同步解码状态突然失效。
    VideoFrameDecoder& operator=(VideoFrameDecoder&&) = delete;

    /// @brief 打开本地视频并初始化首个可解码视频流。
    /// @param filePath 视频文件路径。
    /// @return 初始化成功时返回 true。
    /// @warning 低频阻塞路径：会同步读取容器信息并打开解码器。
    bool open(const std::filesystem::path& filePath);

    /// @brief 关闭当前视频并清空顺序解码缓存。
    void close();

    /// @brief 查询当前是否持有可用视频流。
    /// @return 已成功打开视频时返回 true。
    bool isOpen() const;

    /// @brief 获取当前视频信息。
    /// @return 已打开视频的信息；关闭状态返回全零信息。
    const VideoInfo& info() const;

    /// @brief 选取目标时间点应显示的最近一帧，优先返回不晚于目标的帧。
    /// @param seconds 相对视频起点的目标时间，单位秒。
    /// @return 成功时返回解码器内部 RGBA8 帧的观察指针，失败时返回
    /// nullptr；指针在下次 open、close 或 decodeFrameAt 前有效。
    /// @warning 同步解码路径：可能执行文件读取、Seek
    /// 与像素转换，不得直接放入渲染命令录制热路径。
    const VideoFrame* decodeFrameAt(double seconds);

private:
    /// @brief 隐藏 FFmpeg 类型和顺序解码状态的实现。
    struct Impl;

    /// @brief FFmpeg 解码实现的独占所有权。
    std::unique_ptr<Impl> m_impl;
};

}  // namespace MMM::Utils
