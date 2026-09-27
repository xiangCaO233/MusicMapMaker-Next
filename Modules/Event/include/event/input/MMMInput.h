#pragma once

namespace MMM::Event::Input
{

/// @brief 与具体窗口后端无关的统一键盘键码。
/// @note 这是内部输入协议；除显式锚点外不得假设数值等同于原生键码。
/// @note 未识别的原生键必须映射为 Unknown，不能强制转换为任意枚举值。
enum class Key : int {
    /// @brief 未知或当前翻译器不支持的按键。
    Unknown = 0,
    // 字母键沿用大写 ASCII 编码，并依靠连续枚举值覆盖 A 到 Z。
    // 该布局允许翻译器对标准字母键执行范围映射。
    A = 65,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    // 主键盘数字键紧随字母区声明，表示 D0 到 D9 的逻辑键名。
    // D 前缀用于避免枚举成员以数字开头，并与小键盘数字区分。
    D0,
    D1,
    D2,
    D3,
    D4,
    D5,
    D6,
    D7,
    D8,
    D9,
    // 功能键按编号连续排列，当前公共输入协议覆盖 F1 到 F15。
    // 高于 F15 的后端键码在扩展协议前应保持 Unknown。
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    F13,
    F14,
    F15,
    // 控制键从 GLFW 的 Escape 数值 256 重新建立显式连续区间。
    // 箭头、翻页和首尾键用于编辑器导航，不代表文本字符。
    Escape = 256,
    /// @brief 确认或换行键。
    Enter,
    /// @brief 制表与焦点导航键。
    Tab,
    /// @brief 向前删除输入内容的退格键。
    Backspace,
    /// @brief 切换插入与覆盖模式的 Insert 键。
    Insert,
    /// @brief 删除光标后内容或当前选择的 Delete 键。
    Delete,
    // 方向键使用屏幕方向命名，不受编辑器时间轴方向影响。
    /// @brief 向右导航键。
    Right,
    /// @brief 向左导航键。
    Left,
    /// @brief 向下导航键。
    Down,
    /// @brief 向上导航键。
    Up,
    /// @brief 向前翻页键。
    PageUp,
    /// @brief 向后翻页键。
    PageDown,
    /// @brief 跳转到起始位置的 Home 键。
    Home,
    /// @brief 跳转到结束位置的 End 键。
    End,
    // 锁定键描述键盘状态切换，修饰状态另由 Modifiers 快照携带。
    CapsLock,
    ScrollLock,
    NumLock,
    PrintScreen,
    Pause,
    // 左右修饰键保持独立键码，便于快捷键编辑器区分物理按键。
    // 聚合的 shift、ctrl 等状态由 Modifiers 提供，无需订阅者自行合并。
    LeftShift,
    LeftControl,
    LeftAlt,
    LeftSuper,
    RightShift,
    RightControl,
    RightAlt,
    RightSuper,
    Menu,
    // 符号键名称采用美式键盘未按 Shift 时的基础字符。
    // 实际文本输入必须使用 codepoint 事件，不能由这些键名推断字符。
    Space,
    /// @brief 单引号与双引号共用的物理键。
    Apostrophe,
    Comma,
    Minus,
    Period,
    Slash,
    /// @brief 分号与冒号共用的物理键。
    Semicolon,
    Equal,
    LeftBracket,
    Backslash,
    RightBracket,
    GraveAccent,
    // 小键盘键使用 KP 前缀，避免与主键盘数字和运算符混淆。
    // NumLock 对输入含义的影响由窗口后端解析，逻辑键码保持稳定。
    KP_0,
    KP_1,
    KP_2,
    KP_3,
    KP_4,
    KP_5,
    KP_6,
    KP_7,
    KP_8,
    KP_9,
    /// @brief 小键盘的小数点或区域设置对应的分隔键。
    KP_Decimal,
    /// @brief 小键盘除法键。
    KP_Divide,
    /// @brief 小键盘乘法键。
    KP_Multiply,
    /// @brief 小键盘减法键。
    KP_Subtract,
    /// @brief 小键盘加法键。
    KP_Add,
    /// @brief 小键盘独立确认键。
    KP_Enter,
    /// @brief 部分完整键盘提供的小键盘等号键。
    KP_Equal
};

/// @brief 与具体窗口后端无关的统一鼠标按钮。
/// @note 前三个值固定为常用主按钮，其余值保留额外物理按钮顺序。
enum class MouseButton : int {
    /// @brief 通常用于选择和拖拽的主按钮。
    Left = 0,
    /// @brief 通常用于上下文菜单的次按钮。
    Right = 1,
    /// @brief 通常由滚轮按下产生的中按钮。
    Middle = 2,
    // Button4 到 Button8 表示侧键等设备扩展按钮，不预设业务含义。
    Button4,
    Button5,
    Button6,
    Button7,
    Button8,
    /// @brief 当前协议支持的最后一个鼠标按钮值。
    Last = Button8
};

/// @brief 离散输入事件的统一动作状态。
/// @note Repeat 由窗口系统的按键重复机制产生，不等同于新的 Press。
enum class Action : int { Release = 0, Press = 1, Repeat = 2 };

/// @brief 输入事件发生时的修饰键与锁定键状态快照。
/// @note 位字段用于紧凑传值，不应依赖其底层内存布局进行序列化。
struct Modifiers {
    /// @brief 任一 Shift 修饰键处于激活状态。
    bool shift : 1;
    /// @brief 任一 Control 修饰键处于激活状态。
    bool ctrl : 1;
    /// @brief 任一 Alt 修饰键处于激活状态。
    bool alt : 1;
    /// @brief 任一 Super 或系统修饰键处于激活状态。
    bool super : 1;
    /// @brief Caps Lock 锁定状态处于激活状态。
    bool capsLock : 1;
    /// @brief Num Lock 锁定状态处于激活状态。
    bool numLock : 1;
};

}  // namespace MMM::Event::Input
