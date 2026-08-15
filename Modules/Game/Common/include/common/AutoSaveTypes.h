#pragma once

#include <cstdint>

namespace MMM::Logic
{

/// @brief 自动保存调度器可接收的事件来源。
enum class AutoSaveTrigger : std::uint8_t {
    ObjectModified        = 1U << 0U,  ///< 谱面物件修改已经提交。
    BeatmapSwitch         = 1U << 1U,  ///< 活动谱面即将切换。
    ImGuiWindowFocusLost  = 1U << 2U,  ///< ImGui 根窗口失去焦点。
    NativeWindowFocusLost = 1U << 3U,  ///< 原生窗口失去焦点或最小化。
};

/// @brief 谱面保存请求的来源和反馈策略。
enum class BeatmapSaveKind : std::uint8_t {
    Manual,             ///< 用户显式请求保存。
    TimedAutoSave,      ///< 定时自动保存。
    TriggeredAutoSave,  ///< 事件触发自动保存。
    Internal,           ///< 打包等内部流程要求的静默保存。
};

/// @brief 将自动保存事件转换为原子位掩码。
/// @param trigger 自动保存事件。
/// @return 对应的单一事件位。
[[nodiscard]] constexpr std::uint8_t autoSaveTriggerBit(AutoSaveTrigger trigger)
{
    return static_cast<std::uint8_t>(trigger);
}

}  // namespace MMM::Logic
