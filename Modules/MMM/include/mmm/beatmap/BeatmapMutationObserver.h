#pragma once

#include <cstdint>

namespace MMM
{
class BeatMap;

/// @brief 谱面领域数据发生变化的类别位掩码。
enum class BeatmapMutationFlags : std::uint8_t {
    None         = 0,
    Objects      = 1U << 0U,
    Timelines    = 1U << 1U,
    AudioSamples = 1U << 2U,
    Metadata     = 1U << 3U,
    Annotations  = 1U << 4U,
    All          = Objects | Timelines | AudioSamples | Metadata | Annotations,
};

/// @brief 合并两个谱面变化类别。
[[nodiscard]] constexpr BeatmapMutationFlags operator|(BeatmapMutationFlags lhs,
                                                       BeatmapMutationFlags rhs)
{
    return static_cast<BeatmapMutationFlags>(static_cast<std::uint8_t>(lhs) |
                                             static_cast<std::uint8_t>(rhs));
}

/// @brief 原位合并谱面变化类别。
constexpr BeatmapMutationFlags& operator|=(BeatmapMutationFlags& lhs,
                                           BeatmapMutationFlags  rhs)
{
    lhs = lhs | rhs;
    return lhs;
}

/// @brief 判断位掩码是否包含指定谱面变化类别。
[[nodiscard]] constexpr bool hasBeatmapMutationFlag(BeatmapMutationFlags value,
                                                    BeatmapMutationFlags flag)
{
    return (static_cast<std::uint8_t>(value) &
            static_cast<std::uint8_t>(flag)) != 0U;
}

/// @brief 接收已经同步到 BeatMap 领域对象的低频编辑变化。
class IBeatmapMutationObserver
{
public:
    /// @brief 释放谱面变化观察者。
    virtual ~IBeatmapMutationObserver() = default;

    /// @brief 处理一次已经物化到领域对象的谱面变化。
    /// @param beatmap 当前完整谱面数据。
    /// @param flags 本轮实际变化的数据类别。
    /// @return 观察者接受并排队后的本地变化序号；无需等待权威确认时返回 0。
    /// @warning 逻辑线程低频编辑分支调用；实现不得阻塞等待网络或访问文件系统。
    virtual std::uint64_t onBeatmapMutated(const BeatMap&       beatmap,
                                           BeatmapMutationFlags flags) = 0;

    /// @brief 同步远端权威合并后逻辑线程当前实际持有的谱面基线。
    /// @param beatmap 已按逻辑命令顺序完成合并的当前谱面。
    /// @warning 逻辑线程低频远端提交路径调用；实现不得访问网络或文件系统。
    virtual void onBeatmapSynchronized(const BeatMap& beatmap)
    {
        static_cast<void>(beatmap);
    }

    /// @brief 确认逻辑线程已经应用后台准备好的远端权威状态。
    /// @param revision 已应用的权威文档修订号。
    /// @param includedLocalMutationSequence 该状态包含的最新本地变化序号。
    /// @warning 逻辑线程远端提交路径调用；实现只能完成常量时间状态交接，
    /// 禁止重新扫描谱面、等待后台任务或访问网络。
    virtual void onAuthoritativeBeatmapApplied(
        std::uint64_t revision, std::uint64_t includedLocalMutationSequence)
    {
        static_cast<void>(revision);
        static_cast<void>(includedLocalMutationSequence);
    }
};
}  // namespace MMM
