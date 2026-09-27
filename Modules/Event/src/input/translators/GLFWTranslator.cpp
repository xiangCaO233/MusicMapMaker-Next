#include "event/input/translators/GLFWTranslator.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace MMM::Event::Translator::GLFW
{
/// @brief 将 GLFW 按键动作值转换为统一输入动作。
/// @param action GLFW_PRESS、GLFW_RELEASE 或 GLFW_REPEAT。
/// @return 对应的统一动作；未知值沿用历史行为回退为 Release。
/// @note 返回值仅描述本次回调动作，不查询按键当前持续状态。
/// @warning GLFW 回调路径会直接调用；实现必须保持无分配、无阻塞。
Input::Action GetAction(int action)
{
    // Press 表示从未按下到按下的离散转换。
    if ( action == GLFW_PRESS ) return Input::Action::Press;
    // Release 表示从按下到释放的离散转换。
    if ( action == GLFW_RELEASE ) return Input::Action::Release;
    // Repeat 表示 GLFW 键盘重复，不等同于新的 Press。
    if ( action == GLFW_REPEAT ) return Input::Action::Repeat;
    // GLFW 正常回调不会进入此分支，Release 是兼容既有调用方的安全回退。
    return Input::Action::Release;
}

/// @brief 将 GLFW 修饰键位掩码转换为统一状态快照。
/// @param mods GLFW 回调提供的 GLFW_MOD 位组合。
/// @return 六类修饰键和锁定键的布尔状态。
/// @note 返回值是回调时快照，不持有 GLFW 窗口或输入状态引用。
/// @warning 输入回调可能高频调用；实现仅执行位运算，不得查询窗口状态。
Input::Modifiers GetMods(int mods)
{
    // 所有位字段均显式赋值，避免返回对象携带未初始化状态。
    Input::Modifiers modifiers;
    // Shift 位合并左右 Shift，物理侧别仍由具体 Key 事件保留。
    modifiers.shift = (mods & GLFW_MOD_SHIFT) != 0;
    // Control 位合并左右 Ctrl，适合统一快捷键判定。
    modifiers.ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    // Alt 位合并左右 Alt，不在此解释 AltGr 键盘布局。
    modifiers.alt = (mods & GLFW_MOD_ALT) != 0;
    // Super 位对应平台系统修饰键，显示名称由 UI 层决定。
    modifiers.super = (mods & GLFW_MOD_SUPER) != 0;
    // 锁定键只有在 GLFW 回调启用相应修饰位时才会报告。
    modifiers.capsLock = (mods & GLFW_MOD_CAPS_LOCK) != 0;
    modifiers.numLock  = (mods & GLFW_MOD_NUM_LOCK) != 0;
    return modifiers;
}

/// @brief 将 GLFW 鼠标按钮值转换为统一按钮枚举。
/// @param button GLFW_MOUSE_BUTTON 常量。
/// @return 对应的统一鼠标按钮；未知值回退为 Left。
/// @note 返回枚举不携带动作与坐标，这些数据由 MouseButtonEvent 补充。
/// @warning 鼠标回调路径会直接调用；实现必须保持常量时间和无分配。
Input::MouseButton GetMouseButton(int button)
{
    switch ( button ) {
    // 主按钮保持左、右、中三个通用语义。
    case GLFW_MOUSE_BUTTON_LEFT: return Input::MouseButton::Left;
    case GLFW_MOUSE_BUTTON_RIGHT: return Input::MouseButton::Right;
    case GLFW_MOUSE_BUTTON_MIDDLE: return Input::MouseButton::Middle;
    // 扩展按钮按 GLFW 的编号顺序映射到 Button4 到 Button8。
    case GLFW_MOUSE_BUTTON_4: return Input::MouseButton::Button4;
    case GLFW_MOUSE_BUTTON_5: return Input::MouseButton::Button5;
    case GLFW_MOUSE_BUTTON_6: return Input::MouseButton::Button6;
    case GLFW_MOUSE_BUTTON_7: return Input::MouseButton::Button7;
    case GLFW_MOUSE_BUTTON_8: return Input::MouseButton::Button8;
    // GLFW 正常按钮回调不会超出已声明范围，保留 Left 作为兼容回退。
    default: return Input::MouseButton::Left;
    }
}

/// @brief 将 GLFW 原生键码转换为项目统一键码。
/// @param key GLFW_KEY 常量或 GLFW_KEY_UNKNOWN。
/// @return 对应的统一键码；未覆盖键返回 Unknown。
/// @note 显式 switch 隔离 GLFW 与内部枚举数值，禁止直接强制转换。
/// @note GLFW_KEY_UNKNOWN 和世界键不推断字符，文本输入应使用字符回调。
/// @warning 键盘回调路径可能高频调用；实现保持无分配、无阻塞。
Input::Key GetKey(int key)
{
    switch ( key ) {
    // 字母键按名称逐项映射，不依赖 GLFW 常量连续性。
    // A 到 E 构成字母区第一段。
    case GLFW_KEY_A: return Input::Key::A;
    case GLFW_KEY_B: return Input::Key::B;
    case GLFW_KEY_C: return Input::Key::C;
    case GLFW_KEY_D: return Input::Key::D;
    case GLFW_KEY_E: return Input::Key::E;
    // F 到 J 构成字母区第二段。
    case GLFW_KEY_F: return Input::Key::F;
    case GLFW_KEY_G: return Input::Key::G;
    case GLFW_KEY_H: return Input::Key::H;
    case GLFW_KEY_I: return Input::Key::I;
    case GLFW_KEY_J: return Input::Key::J;
    // K 到 O 构成字母区第三段。
    case GLFW_KEY_K: return Input::Key::K;
    case GLFW_KEY_L: return Input::Key::L;
    case GLFW_KEY_M: return Input::Key::M;
    case GLFW_KEY_N: return Input::Key::N;
    case GLFW_KEY_O: return Input::Key::O;
    // P 到 T 构成字母区第四段。
    case GLFW_KEY_P: return Input::Key::P;
    case GLFW_KEY_Q: return Input::Key::Q;
    case GLFW_KEY_R: return Input::Key::R;
    case GLFW_KEY_S: return Input::Key::S;
    case GLFW_KEY_T: return Input::Key::T;
    // U 到 Z 构成字母区末段。
    case GLFW_KEY_U: return Input::Key::U;
    case GLFW_KEY_V: return Input::Key::V;
    case GLFW_KEY_W: return Input::Key::W;
    case GLFW_KEY_X: return Input::Key::X;
    case GLFW_KEY_Y: return Input::Key::Y;
    case GLFW_KEY_Z: return Input::Key::Z;
    // 主键盘数字使用 D 前缀，与小键盘数字保持区别。
    // 0 到 4 构成数字区第一段。
    case GLFW_KEY_0: return Input::Key::D0;
    case GLFW_KEY_1: return Input::Key::D1;
    case GLFW_KEY_2: return Input::Key::D2;
    case GLFW_KEY_3: return Input::Key::D3;
    case GLFW_KEY_4: return Input::Key::D4;
    // 5 到 9 构成数字区第二段。
    case GLFW_KEY_5: return Input::Key::D5;
    case GLFW_KEY_6: return Input::Key::D6;
    case GLFW_KEY_7: return Input::Key::D7;
    case GLFW_KEY_8: return Input::Key::D8;
    case GLFW_KEY_9: return Input::Key::D9;
    // GLFW 映射覆盖统一协议当前支持的 F1 到 F15。
    // F1 到 F5 构成功能键第一段。
    case GLFW_KEY_F1: return Input::Key::F1;
    case GLFW_KEY_F2: return Input::Key::F2;
    case GLFW_KEY_F3: return Input::Key::F3;
    case GLFW_KEY_F4: return Input::Key::F4;
    case GLFW_KEY_F5: return Input::Key::F5;
    // F6 到 F10 构成功能键第二段。
    case GLFW_KEY_F6: return Input::Key::F6;
    case GLFW_KEY_F7: return Input::Key::F7;
    case GLFW_KEY_F8: return Input::Key::F8;
    case GLFW_KEY_F9: return Input::Key::F9;
    case GLFW_KEY_F10: return Input::Key::F10;
    // F11 到 F15 构成功能键末段。
    case GLFW_KEY_F11: return Input::Key::F11;
    case GLFW_KEY_F12: return Input::Key::F12;
    case GLFW_KEY_F13: return Input::Key::F13;
    case GLFW_KEY_F14: return Input::Key::F14;
    case GLFW_KEY_F15: return Input::Key::F15;
    // Escape、确认、制表和退格构成基础编辑控制键。
    case GLFW_KEY_ESCAPE: return Input::Key::Escape;
    case GLFW_KEY_ENTER: return Input::Key::Enter;
    case GLFW_KEY_TAB: return Input::Key::Tab;
    case GLFW_KEY_BACKSPACE: return Input::Key::Backspace;
    // 插入和删除键保持独立编辑语义。
    case GLFW_KEY_INSERT: return Input::Key::Insert;
    case GLFW_KEY_DELETE: return Input::Key::Delete;
    // 方向键使用屏幕方向命名，不解释编辑器的时间轴方向。
    case GLFW_KEY_RIGHT: return Input::Key::Right;
    case GLFW_KEY_LEFT: return Input::Key::Left;
    case GLFW_KEY_DOWN: return Input::Key::Down;
    case GLFW_KEY_UP: return Input::Key::Up;
    // 翻页与首尾键用于大跨度导航。
    case GLFW_KEY_PAGE_UP: return Input::Key::PageUp;
    case GLFW_KEY_PAGE_DOWN: return Input::Key::PageDown;
    case GLFW_KEY_HOME: return Input::Key::Home;
    case GLFW_KEY_END: return Input::Key::End;
    // 锁定键和系统控制键保留独立键码。
    case GLFW_KEY_CAPS_LOCK: return Input::Key::CapsLock;
    case GLFW_KEY_SCROLL_LOCK: return Input::Key::ScrollLock;
    case GLFW_KEY_NUM_LOCK: return Input::Key::NumLock;
    case GLFW_KEY_PRINT_SCREEN: return Input::Key::PrintScreen;
    case GLFW_KEY_PAUSE: return Input::Key::Pause;
    // 左侧修饰键保留物理侧别，便于快捷键配置精确匹配。
    case GLFW_KEY_LEFT_SHIFT: return Input::Key::LeftShift;
    case GLFW_KEY_LEFT_CONTROL: return Input::Key::LeftControl;
    case GLFW_KEY_LEFT_ALT: return Input::Key::LeftAlt;
    case GLFW_KEY_LEFT_SUPER: return Input::Key::LeftSuper;
    // 右侧修饰键同样保留物理侧别。
    case GLFW_KEY_RIGHT_SHIFT: return Input::Key::RightShift;
    case GLFW_KEY_RIGHT_CONTROL: return Input::Key::RightControl;
    case GLFW_KEY_RIGHT_ALT: return Input::Key::RightAlt;
    case GLFW_KEY_RIGHT_SUPER: return Input::Key::RightSuper;
    // 菜单键和空格键单独列出，避免被视作普通标点。
    case GLFW_KEY_MENU: return Input::Key::Menu;
    case GLFW_KEY_SPACE: return Input::Key::Space;
    // 标点键映射物理键名，实际字符由文本输入或码点解析器确定。
    // 单引号到斜杠构成标点区第一段。
    case GLFW_KEY_APOSTROPHE: return Input::Key::Apostrophe;
    case GLFW_KEY_COMMA: return Input::Key::Comma;
    case GLFW_KEY_MINUS: return Input::Key::Minus;
    case GLFW_KEY_PERIOD: return Input::Key::Period;
    case GLFW_KEY_SLASH: return Input::Key::Slash;
    // 分号到重音符构成标点区第二段。
    case GLFW_KEY_SEMICOLON: return Input::Key::Semicolon;
    case GLFW_KEY_EQUAL: return Input::Key::Equal;
    case GLFW_KEY_LEFT_BRACKET: return Input::Key::LeftBracket;
    case GLFW_KEY_BACKSLASH: return Input::Key::Backslash;
    case GLFW_KEY_RIGHT_BRACKET: return Input::Key::RightBracket;
    case GLFW_KEY_GRAVE_ACCENT: return Input::Key::GraveAccent;
    // 小键盘数字保留设备位置语义，不与主键盘数字合并。
    // KP 0 到 KP 4 构成小键盘数字第一段。
    case GLFW_KEY_KP_0: return Input::Key::KP_0;
    case GLFW_KEY_KP_1: return Input::Key::KP_1;
    case GLFW_KEY_KP_2: return Input::Key::KP_2;
    case GLFW_KEY_KP_3: return Input::Key::KP_3;
    case GLFW_KEY_KP_4: return Input::Key::KP_4;
    // KP 5 到 KP 9 构成小键盘数字第二段。
    case GLFW_KEY_KP_5: return Input::Key::KP_5;
    case GLFW_KEY_KP_6: return Input::Key::KP_6;
    case GLFW_KEY_KP_7: return Input::Key::KP_7;
    case GLFW_KEY_KP_8: return Input::Key::KP_8;
    case GLFW_KEY_KP_9: return Input::Key::KP_9;
    // 小键盘小数点、除法和乘法保留独立运算键名。
    case GLFW_KEY_KP_DECIMAL: return Input::Key::KP_Decimal;
    case GLFW_KEY_KP_DIVIDE: return Input::Key::KP_Divide;
    case GLFW_KEY_KP_MULTIPLY: return Input::Key::KP_Multiply;
    // 小键盘减法、加法、确认和等号完成剩余运算键映射。
    case GLFW_KEY_KP_SUBTRACT: return Input::Key::KP_Subtract;
    case GLFW_KEY_KP_ADD: return Input::Key::KP_Add;
    case GLFW_KEY_KP_ENTER: return Input::Key::KP_Enter;
    case GLFW_KEY_KP_EQUAL: return Input::Key::KP_Equal;
    // 新增统一键码时需同步核对 ImGui 翻译器，保持两种来源语义一致。
    // 世界键和未来新增的 GLFW 键在协议扩展前保持 Unknown。
    // 显式未知值让上层跳过快捷键匹配，避免误用默认键位。
    default: return Input::Key::Unknown;
    }
}
}  // namespace MMM::Event::Translator::GLFW
