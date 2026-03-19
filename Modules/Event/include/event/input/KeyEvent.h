#pragma once

#include "InputEvent.h"

namespace MMM::Event
{

struct KeyEvent : public InputEvent {
    ///@brief 物理键值（用于快捷键判断：如 Ctrl + Z）
    ///@note 即使在德语布局下，
    /// 按下物理 Y 位置处的键，GLFW 也会根据布局返回 Z 或 Y 的 KeyCode
    Input::Key key{ Input::Key::Unknown };

    ///@brief 硬件扫描码（原始 ID，跨平台不统一，但在处理底层驱动时有用）
    int scancode{ 0 };

    ///@brief 动作 (Press, Release, Repeat)
    Input::Action action{ Input::Action::Release };

    ///@brief 该按键产生的 Unicode 字符 (UTF-32)
    ///@note 如果该键不产生字符（如 Ctrl, F1, Shift），此值为 0。
    ///@note 在德语布局下，按下物理位置的 Z 键，这里会正确填充 'y' 或 'Y'。
    char32_t codepoint{ 0 };

    ///@brief 辅助：是否产生可见字符
    inline bool hasText() const { return codepoint > 0; }

    // 辅助方法：转换为常用的 char (仅限 ASCII 范围)
    char asAscii() const
    {
        return (codepoint < 128) ? static_cast<char>(codepoint) : '\0';
    }
};

}  // namespace MMM::Event

// 注册parent关系
EVENT_REGISTER_PARENTS(KeyEvent, InputEvent);
