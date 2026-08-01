#pragma once

#include "event/input/MMMInput.h"

namespace MMM::Event::Translator::GLFW
{
/// @brief 将 GLFW 动作值转换为通用输入动作。
/// @param action GLFW 动作值。
/// @return 通用输入动作。
Input::Action GetAction(int action);

/// @brief 将 GLFW 修饰键掩码转换为通用修饰键状态。
/// @param mods GLFW 修饰键掩码。
/// @return 通用修饰键状态。
Input::Modifiers GetMods(int mods);

/// @brief 将 GLFW 鼠标按键转换为通用鼠标按键。
/// @param button GLFW 鼠标按键值。
/// @return 通用鼠标按键。
Input::MouseButton GetMouseButton(int button);

/// @brief 将 GLFW 键值转换为通用键值。
/// @param key GLFW 键值。
/// @return 通用键值；无对应键时返回 Unknown。
Input::Key GetKey(int key);
}  // namespace MMM::Event::Translator::GLFW
