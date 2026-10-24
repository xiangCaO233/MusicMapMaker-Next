#pragma once

namespace MMM::UI
{
/// @brief 音轨控制器与工作区恢复入口共用的视图音轨种类。
/// @details 保持原嵌套枚举的 int 底层类型与数值顺序。
/// 此类型描述控制器视图，不改变音频引擎的音轨分类或持久化文本。
enum class AudioTrackControllerType {
    /// @brief 主音轨控制器。
    Main,
    /// @brief 音效音轨控制器。
    Effect
};
}  // namespace MMM::UI
