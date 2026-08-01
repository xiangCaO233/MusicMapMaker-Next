#pragma once

namespace MMM::Logic
{
struct CmdPanCanvas;
struct CmdScroll;
struct CmdSeek;
struct CmdSetBgmKeySoundAreaMute;
struct CmdSetKeySoundEffectGroupGain;
struct CmdSetKeySoundTrackGain;
struct CmdSetKeySoundTrackMute;
struct CmdSetPlaybackSpeed;
struct CmdSetPlayState;
struct SessionContext;

/// @brief 播放控制器，负责处理音频播放、时间轴滚动以及视听同步相关逻辑。
class PlaybackController
{
public:
    /// @brief 构造函数
    /// @param ctx 会话上下文引用
    PlaybackController(SessionContext& ctx) : m_ctx(ctx) {}

    /// @brief 处理设置播放状态的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSetPlayState& cmd);

    /// @brief 处理时间轴跳转的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSeek& cmd);

    /// @brief 处理设置播放速度的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdSetPlaybackSpeed& cmd);

    /// @brief 处理单条玩家或 BGM 轨道的 Key 音静音命令。
    /// @param cmd 目标区域、轨道索引和静音状态。
    void handleCommand(const CmdSetKeySoundTrackMute& cmd);

    /// @brief 处理单条玩家或 BGM 轨道的 Key 音增益命令。
    /// @param cmd 目标区域、轨道索引和线性增益。
    void handleCommand(const CmdSetKeySoundTrackGain& cmd);

    /// @brief 处理绑定或未绑定打击音效类别的实时增益命令。
    /// @param cmd 目标类别和线性增益。
    void handleCommand(const CmdSetKeySoundEffectGroupGain& cmd);

    /// @brief 处理整个 BGM 轨道区的 Key 音静音命令。
    /// @param cmd BGM 区静音状态。
    void handleCommand(const CmdSetBgmKeySoundAreaMute& cmd);

    /// @brief 处理鼠标滚轮滚动的命令
    /// @param cmd 命令数据
    void handleCommand(const CmdScroll& cmd);

    /// @brief 处理主画布二维平移命令。
    /// @param cmd 以逻辑像素表达的平移增量。
    /// @warning 逻辑输入热路径：中键拖动期间每次 update
    /// 调用；只做相机状态更新与 ScrollCache 对数级坐标换算。
    void handleCommand(const CmdPanCanvas& cmd);

    /// @brief 同步当前的打击索引，用于打击特效和音效判断
    void syncHitIndex();

    /// @brief 重新构建所有的打击事件列表
    void rebuildHitEvents();

    /// @brief 每帧更新逻辑
    /// @param dt 帧间隔时间 (秒)
    void onUpdate(double dt);

private:
    SessionContext& m_ctx;  ///< 全局会话上下文引用

    /// @brief 上一条 seek 是否来自仍在持续的进度条拖动。
    bool m_isSeekScrubbing{ false };
};

}  // namespace MMM::Logic
