#pragma once

#include <string_view>

namespace MMM::UI
{

/// @brief 协作房间字段采用自动默认值还是用户自定义值。
enum class CollaborationDefaultMode {
    Follow,
    Custom,
};

/// @brief 判断字段是否应随当前谱面默认值更新。
/// @param mode 当前字段模式。
/// @return 自动跟随模式返回 true。
/// @warning UI 热路径：每帧只执行一次枚举比较。
[[nodiscard]] constexpr bool shouldFollowCollaborationDefault(
    CollaborationDefaultMode mode)
{
    return mode == CollaborationDefaultMode::Follow;
}

/// @brief 根据文本编辑结果判断房间名是否仍等于默认值。
/// @param currentValue 用户编辑后的当前文本。
/// @param defaultValue 当前谱面提供的默认文本。
/// @return 与默认值相同则恢复自动跟随，否则进入自定义模式。
/// @warning UI 交互路径：仅在输入框内容发生变化时比较短字符串。
[[nodiscard]] constexpr CollaborationDefaultMode
resolveCollaborationTextDefaultMode(std::string_view currentValue,
                                    std::string_view defaultValue)
{
    return currentValue == defaultValue ? CollaborationDefaultMode::Follow
                                        : CollaborationDefaultMode::Custom;
}

}  // namespace MMM::UI
