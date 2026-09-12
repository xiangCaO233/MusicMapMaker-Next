#pragma once

#include <cstdint>

namespace MMM::Logic
{

/// @brief 自动保存调度器可接收的事件来源。
/// @note 每个值占用独立位，可组合表示同一更新周期内的多个触发原因。
enum class AutoSaveTrigger : std::uint8_t {
    ObjectModified        = 1U << 0U,  ///< 谱面物件修改已经提交。
    BeatmapSwitch         = 1U << 1U,  ///< 活动谱面即将切换。
    ImGuiWindowFocusLost  = 1U << 2U,  ///< ImGui 根窗口失去焦点。
    NativeWindowFocusLost = 1U << 3U,  ///< 原生窗口失去焦点或最小化。
};

/// @brief 谱面保存请求的来源和反馈策略。
/// @note 保存种类影响调度与 UI 反馈，不改变最终文件格式。
enum class BeatmapSaveKind : std::uint8_t {
    Manual,             ///< 用户显式请求保存。
    TimedAutoSave,      ///< 定时自动保存。
    TriggeredAutoSave,  ///< 事件触发自动保存。
    Internal,           ///< 打包等内部流程要求的静默保存。
};

/// @brief 将自动保存事件转换为原子位掩码。
/// @param trigger 自动保存事件。
/// @return 对应的单一事件位。
/// @note 调用方可对返回值执行按位或，累计尚未处理的触发原因。
/// @warning 该函数可能在逻辑 update 路径调用，必须保持 constexpr 且无副作用。
[[nodiscard]] constexpr std::uint8_t autoSaveTriggerBit(AutoSaveTrigger trigger)
{
    return static_cast<std::uint8_t>(trigger);
}

}  // namespace MMM::Logic
