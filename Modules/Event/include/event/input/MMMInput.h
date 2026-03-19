#pragma once

namespace MMM::Event::Input
{

///@brief 统一键盘键码
enum class Key : int {
    Unknown = 0,
    // 字母
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
    // 数字
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
    // 功能键
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
    // 控制键
    Escape = 256,
    Enter,
    Tab,
    Backspace,
    Insert,
    Delete,
    Right,
    Left,
    Down,
    Up,
    PageUp,
    PageDown,
    Home,
    End,
    CapsLock,
    ScrollLock,
    NumLock,
    PrintScreen,
    Pause,
    // 修改键
    LeftShift,
    LeftControl,
    LeftAlt,
    LeftSuper,
    RightShift,
    RightControl,
    RightAlt,
    RightSuper,
    Menu,
    // 符号
    Space,
    Apostrophe,
    Comma,
    Minus,
    Period,
    Slash,
    Semicolon,
    Equal,
    LeftBracket,
    Backslash,
    RightBracket,
    GraveAccent,
    // 小键盘
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
    KP_Decimal,
    KP_Divide,
    KP_Multiply,
    KP_Subtract,
    KP_Add,
    KP_Enter,
    KP_Equal
};

///@brief 统一鼠标按键
enum class MouseButton : int {
    Left   = 0,
    Right  = 1,
    Middle = 2,
    Button4,
    Button5,
    Button6,
    Button7,
    Button8,
    Last = Button8
};

///@brief 统一交互状态
enum class Action : int { Release = 0, Press = 1, Repeat = 2 };

///@brief 统一修饰键位掩码
struct Modifiers {
    bool shift : 1;
    bool ctrl : 1;
    bool alt : 1;
    bool super : 1;
    bool capsLock : 1;
    bool numLock : 1;
};

}  // namespace MMM::Event::Input
