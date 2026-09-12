#pragma once

#include "event/input/MMMInput.h"
#include <imgui.h>

namespace MMM::Event::Translator::ImGui
{
/// @brief 从当前 ImGuiIO 快照读取统一修饰键状态。
/// @return 当前帧的 Shift、Ctrl、Alt 与 Super 状态。
/// @note ImGuiIO 不提供锁定键状态，因此 Caps Lock 和 Num Lock 固定为 false。
/// @note 返回值是调用时快照，后续 ImGui 帧不会反向更新已有事件。
/// @warning UI 输入路径可能每帧调用；实现必须保持无分配、无阻塞。
inline Input::Modifiers GetMods()
{
    Input::Modifiers m;
    ImGuiIO&         io = ::ImGui::GetIO();
    // KeyMods 是 ImGui 汇总后的修饰键位掩码，避免重复查询左右物理键。
    // Shift 位合并左右 Shift，统一输入事件另用 Key 保留物理侧别。
    m.shift = (io.KeyMods & ImGuiMod_Shift) != 0;
    // Ctrl 位合并左右 Ctrl，适合跨平台快捷键判定。
    m.ctrl = (io.KeyMods & ImGuiMod_Ctrl) != 0;
    // Alt 位合并左右 Alt，不在此解释 AltGr 键盘布局。
    m.alt = (io.KeyMods & ImGuiMod_Alt) != 0;
    // Super 位对应平台系统修饰键，具体名称由 UI 文案层决定。
    m.super = (io.KeyMods & ImGuiMod_Super) != 0;
    // 锁定键状态无法从当前 ImGuiIO 契约可靠获得，显式清零避免未初始化位。
    m.capsLock = false;
    m.numLock  = false;
    return m;
}

/// @brief 将 ImGui 鼠标按钮编号转换为统一按钮枚举。
/// @param button ImGuiMouseButton 对应的整数值。
/// @return 匹配的通用鼠标按钮；未知值回退为 Left。
/// @note 扩展鼠标按钮不在当前 ImGui 转换协议中，由 GLFW 来源事件处理。
/// @warning 输入路径可能高频调用；实现不得引入分配或外部状态查询。
inline Input::MouseButton GetMouseButton(int button)
{
    // 左按钮保持选择和拖拽的主按钮语义。
    switch ( button ) {
    case ImGuiMouseButton_Left: return Input::MouseButton::Left;
    // 右按钮保持上下文操作的次按钮语义。
    case ImGuiMouseButton_Right: return Input::MouseButton::Right;
    // 中按钮保持滚轮按压产生的按钮语义。
    case ImGuiMouseButton_Middle: return Input::MouseButton::Middle;
    // 自定义后端提供的扩展编号没有稳定语义，沿用历史回退值。
    default: return Input::MouseButton::Left;
    }
}

/// @brief 将 ImGui 逻辑键转换为项目统一键码。
/// @param key ImGui 输入系统报告的逻辑键。
/// @return 对应的统一键码；未覆盖的 ImGui 键返回 Unknown。
/// @note 显式 switch 隔离两套枚举的版本与数值差异，禁止直接强制转换。
/// @note 键码表示物理操作，不替代 ImGui 的文本与输入法字符队列。
/// @warning 键盘输入路径可能高频调用；实现保持常量时间、无分配和无阻塞。
inline Input::Key GetKey(ImGuiKey key)
{
    switch ( key ) {
    // 字母键按逻辑字符名称逐项映射，不依赖 ImGuiKey 数值连续性。
    // A 到 E 构成字母区第一段。
    case ImGuiKey_A: return Input::Key::A;
    case ImGuiKey_B: return Input::Key::B;
    case ImGuiKey_C: return Input::Key::C;
    case ImGuiKey_D: return Input::Key::D;
    case ImGuiKey_E: return Input::Key::E;
    // F 到 J 构成字母区第二段。
    case ImGuiKey_F: return Input::Key::F;
    case ImGuiKey_G: return Input::Key::G;
    case ImGuiKey_H: return Input::Key::H;
    case ImGuiKey_I: return Input::Key::I;
    case ImGuiKey_J: return Input::Key::J;
    // K 到 O 构成字母区第三段。
    case ImGuiKey_K: return Input::Key::K;
    case ImGuiKey_L: return Input::Key::L;
    case ImGuiKey_M: return Input::Key::M;
    case ImGuiKey_N: return Input::Key::N;
    case ImGuiKey_O: return Input::Key::O;
    // P 到 T 构成字母区第四段。
    case ImGuiKey_P: return Input::Key::P;
    case ImGuiKey_Q: return Input::Key::Q;
    case ImGuiKey_R: return Input::Key::R;
    case ImGuiKey_S: return Input::Key::S;
    case ImGuiKey_T: return Input::Key::T;
    // U 到 Z 构成字母区末段。
    case ImGuiKey_U: return Input::Key::U;
    case ImGuiKey_V: return Input::Key::V;
    case ImGuiKey_W: return Input::Key::W;
    case ImGuiKey_X: return Input::Key::X;
    case ImGuiKey_Y: return Input::Key::Y;
    case ImGuiKey_Z: return Input::Key::Z;
    // 主键盘数字使用 D 前缀，避免与小键盘数字混淆。
    // 0 到 4 构成数字区第一段。
    case ImGuiKey_0: return Input::Key::D0;
    case ImGuiKey_1: return Input::Key::D1;
    case ImGuiKey_2: return Input::Key::D2;
    case ImGuiKey_3: return Input::Key::D3;
    case ImGuiKey_4: return Input::Key::D4;
    // 5 到 9 构成数字区第二段。
    case ImGuiKey_5: return Input::Key::D5;
    case ImGuiKey_6: return Input::Key::D6;
    case ImGuiKey_7: return Input::Key::D7;
    case ImGuiKey_8: return Input::Key::D8;
    case ImGuiKey_9: return Input::Key::D9;
    // ImGui 当前映射范围覆盖常用功能键 F1 到 F12。
    // F1 到 F4 构成功能键第一段。
    case ImGuiKey_F1: return Input::Key::F1;
    case ImGuiKey_F2: return Input::Key::F2;
    case ImGuiKey_F3: return Input::Key::F3;
    case ImGuiKey_F4: return Input::Key::F4;
    // F5 到 F8 构成功能键第二段。
    case ImGuiKey_F5: return Input::Key::F5;
    case ImGuiKey_F6: return Input::Key::F6;
    case ImGuiKey_F7: return Input::Key::F7;
    case ImGuiKey_F8: return Input::Key::F8;
    // F9 到 F12 构成功能键末段。
    case ImGuiKey_F9: return Input::Key::F9;
    case ImGuiKey_F10: return Input::Key::F10;
    case ImGuiKey_F11: return Input::Key::F11;
    case ImGuiKey_F12: return Input::Key::F12;
    // Escape 用于取消或关闭交互，不产生文本码点。
    case ImGuiKey_Escape: return Input::Key::Escape;
    // Enter、Tab 与 Backspace 保留编辑控制语义。
    case ImGuiKey_Enter: return Input::Key::Enter;
    case ImGuiKey_Tab: return Input::Key::Tab;
    case ImGuiKey_Backspace: return Input::Key::Backspace;
    // Insert 和 Delete 表示编辑模式与删除操作。
    case ImGuiKey_Insert: return Input::Key::Insert;
    case ImGuiKey_Delete: return Input::Key::Delete;
    // 四个方向键保持屏幕方向语义。
    case ImGuiKey_RightArrow: return Input::Key::Right;
    case ImGuiKey_LeftArrow: return Input::Key::Left;
    case ImGuiKey_DownArrow: return Input::Key::Down;
    case ImGuiKey_UpArrow: return Input::Key::Up;
    // 翻页和首尾键用于大跨度导航。
    case ImGuiKey_PageUp: return Input::Key::PageUp;
    case ImGuiKey_PageDown: return Input::Key::PageDown;
    case ImGuiKey_Home: return Input::Key::Home;
    case ImGuiKey_End: return Input::Key::End;
    // Caps Lock、Scroll Lock 与 Num Lock 保留独立物理键名。
    case ImGuiKey_CapsLock: return Input::Key::CapsLock;
    case ImGuiKey_ScrollLock: return Input::Key::ScrollLock;
    case ImGuiKey_NumLock: return Input::Key::NumLock;
    case ImGuiKey_PrintScreen: return Input::Key::PrintScreen;
    case ImGuiKey_Pause: return Input::Key::Pause;
    // 左侧修饰键保持物理侧别，快捷键可按需区分。
    case ImGuiKey_LeftShift: return Input::Key::LeftShift;
    case ImGuiKey_LeftCtrl: return Input::Key::LeftControl;
    case ImGuiKey_LeftAlt: return Input::Key::LeftAlt;
    case ImGuiKey_LeftSuper: return Input::Key::LeftSuper;
    // 右侧修饰键同样保持物理侧别。
    case ImGuiKey_RightShift: return Input::Key::RightShift;
    case ImGuiKey_RightCtrl: return Input::Key::RightControl;
    case ImGuiKey_RightAlt: return Input::Key::RightAlt;
    case ImGuiKey_RightSuper: return Input::Key::RightSuper;
    // Menu 与 Space 分别表示菜单物理键和空格物理键。
    case ImGuiKey_Menu: return Input::Key::Menu;
    case ImGuiKey_Space: return Input::Key::Space;
    // 标点键映射物理键名，最终字符由修饰键和文本输入系统决定。
    // 单引号到斜杠构成标点区第一段。
    case ImGuiKey_Apostrophe: return Input::Key::Apostrophe;
    case ImGuiKey_Comma: return Input::Key::Comma;
    case ImGuiKey_Minus: return Input::Key::Minus;
    case ImGuiKey_Period: return Input::Key::Period;
    case ImGuiKey_Slash: return Input::Key::Slash;
    // 分号到重音符构成标点区第二段。
    case ImGuiKey_Semicolon: return Input::Key::Semicolon;
    case ImGuiKey_Equal: return Input::Key::Equal;
    case ImGuiKey_LeftBracket: return Input::Key::LeftBracket;
    case ImGuiKey_Backslash: return Input::Key::Backslash;
    case ImGuiKey_RightBracket: return Input::Key::RightBracket;
    case ImGuiKey_GraveAccent: return Input::Key::GraveAccent;
    // 小键盘数字与主键盘数字分开映射，保留设备位置语义。
    // Keypad0 到 Keypad4 构成小键盘数字第一段。
    case ImGuiKey_Keypad0: return Input::Key::KP_0;
    case ImGuiKey_Keypad1: return Input::Key::KP_1;
    case ImGuiKey_Keypad2: return Input::Key::KP_2;
    case ImGuiKey_Keypad3: return Input::Key::KP_3;
    case ImGuiKey_Keypad4: return Input::Key::KP_4;
    // Keypad5 到 Keypad9 构成小键盘数字第二段。
    case ImGuiKey_Keypad5: return Input::Key::KP_5;
    case ImGuiKey_Keypad6: return Input::Key::KP_6;
    case ImGuiKey_Keypad7: return Input::Key::KP_7;
    case ImGuiKey_Keypad8: return Input::Key::KP_8;
    case ImGuiKey_Keypad9: return Input::Key::KP_9;
    // 小键盘小数点、除法和乘法保留独立运算键名。
    case ImGuiKey_KeypadDecimal: return Input::Key::KP_Decimal;
    case ImGuiKey_KeypadDivide: return Input::Key::KP_Divide;
    case ImGuiKey_KeypadMultiply: return Input::Key::KP_Multiply;
    // 小键盘减法、加法、确认和等号完成剩余运算键映射。
    case ImGuiKey_KeypadSubtract: return Input::Key::KP_Subtract;
    case ImGuiKey_KeypadAdd: return Input::Key::KP_Add;
    case ImGuiKey_KeypadEnter: return Input::Key::KP_Enter;
    case ImGuiKey_KeypadEqual: return Input::Key::KP_Equal;
    // 未映射值不能回退到任意有效键，否则可能错误命中用户快捷键。
    // 后续扩展应同时更新统一 Key 枚举与各窗口后端翻译器。
    // 游戏手柄、鼠标别名和后续 ImGui 新增键暂不属于统一键盘协议。
    // Unknown 让调用方显式忽略事件，避免错误触发已有快捷键。
    default: return Input::Key::Unknown;
    }
}
}  // namespace MMM::Event::Translator::ImGui
