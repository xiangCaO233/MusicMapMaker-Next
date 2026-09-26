#pragma once

#include <cstddef>
#include <cstdint>

namespace MMM::Logic
{
struct CmdPanCanvas;
struct CmdScroll;
struct CmdSeek;
struct CmdSetBgmKeySoundAreaMute;
struct CmdSetDraftKeySoundAreaMute;
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

    /// @brief 在用户启用、开始播放或切换皮肤时预加载编辑器节拍音。
    /// @param gain 编辑器节拍器初始线性增益。
    /// @return 两种拍声均已载入时返回 true。
    /// @warning 低频交互路径：可能等待文件解码，禁止在逐轮 update 中调用。
    [[nodiscard]] static bool preloadMetronomeSounds(float gain);

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

    /// @brief 处理整个草稿轨道区的 Key 音静音命令。
    /// @param cmd 草稿区静音状态。
    void handleCommand(const CmdSetDraftKeySoundAreaMute& cmd);

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

    /// @brief 更新编辑器播放节拍器的提前预约。
    /// @param playbackJumped 本轮播放时钟是否跳转并清空预约音效。
    /// @warning
    /// 逻辑热路径：只对已加载音效进行常量级预约；资源加载仅在启用或皮肤失效的低频分支执行。
    void updateMetronome(bool playbackJumped);

private:
    /// @brief 重建当前 BPM 段的下一拍游标。
    /// @param time 当前音频时间，单位秒。
    /// @warning 跳转或 BPM 编辑时执行一次二分查找，不在稳定播放时重复扫描。
    void resetMetronomeCursor(double time);

    SessionContext& m_ctx;  ///< 全局会话上下文引用
    /// @brief 两种皮肤节拍音是否已载入当前音频管理器。
    bool m_metronomeResourcesReady{ false };
    /// @brief 上轮是否作为播放源排定节拍，停止时仅清理自己的两个音效池。
    bool m_metronomeWasActive{ false };
    /// @brief 下一拍游标是否与当前 BPM 缓存和播放位置对应。
    bool m_metronomeCursorReady{ false };
    /// @brief 最后一次用于预约节拍的 BPM 缓存版本。
    std::uint64_t m_seenBpmEventsRevision{ 0U };
    /// @brief 已生效 BPM 事件数量，零代表使用谱面偏好 BPM。
    std::size_t m_metronomeSegmentIndex{ 0U };
    /// @brief 当前 BPM 段的下一拍整数索引。
    std::int64_t m_nextMetronomeBeatIndex{ 0 };
    /// @brief 下一拍在音频时间轴上的秒数。
    double m_nextMetronomeBeatTime{ 0.0 };
    /// @brief 已应用到音效池的节拍器增益；负值表示需要首次同步。
    float m_appliedMetronomeGain{ -1.0F };
};

}  // namespace MMM::Logic
