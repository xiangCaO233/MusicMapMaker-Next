#pragma once

#include "event/input/MMMInput.h"

namespace MMM::Event::Translator
{
/// @brief 按美式键盘基础布局将统一键码解析为单个 Unicode 码点。
/// @param key 经过窗口后端规范化的逻辑键码。
/// @param mods 输入发生时的修饰键状态快照。
/// @return 可直接输入的码点；非文本键或无映射键返回零。
/// @note 该函数只提供快捷键与简单文本回退，完整文本输入应使用输入法回调。
/// @note Ctrl、Alt 与 Super 不改变基础字符，组合快捷键由上层先行处理。
/// @warning 输入处理路径可能高频调用；实现必须保持无分配、无阻塞。
inline char32_t ResolveCodepoint(Input::Key key, const Input::Modifiers& mods)
{
    // 统一枚举中的连续区间用于计算字母、数字相对起点的偏移。
    // 这里只读取键码数值，不将计算结果反向解释为未声明的枚举成员。
    int k = static_cast<int>(key);

    // 字母大小写遵循 Shift 与 Caps Lock 异或关系。
    // 同时启用两者会恢复小写，这是常见桌面键盘的行为。
    if ( key >= Input::Key::A && key <= Input::Key::Z ) {
        bool uppercase = (mods.shift != mods.capsLock);
        return uppercase ? (U'A' + (k - static_cast<int>(Input::Key::A)))
                         : (U'a' + (k - static_cast<int>(Input::Key::A)));
    }

    // 主键盘数字区在未按 Shift 时直接映射为 ASCII 数字。
    if ( key >= Input::Key::D0 && key <= Input::Key::D9 ) {
        if ( !mods.shift ) return U'0' + (k - static_cast<int>(Input::Key::D0));
        // Shift 数字映射固定采用美式布局；其他布局交由文本输入回调处理。
        switch ( key ) {
        // 数字 1 到 4 对应第一组常用标点。
        case Input::Key::D1: return U'!';
        case Input::Key::D2: return U'@';
        case Input::Key::D3: return U'#';
        case Input::Key::D4: return U'$';
        // 数字 5 到 7 对应百分号、脱字符和与号。
        case Input::Key::D5: return U'%';
        case Input::Key::D6: return U'^';
        case Input::Key::D7: return U'&';
        // 数字 8 到 0 对应星号和左右圆括号。
        case Input::Key::D8: return U'*';
        case Input::Key::D9: return U'(';
        case Input::Key::D0: return U')';
        // 范围检查后原则上不可达，仍保留零值以防枚举协议未来出现空洞。
        default: return 0;
        }
    }

    // 其余可打印键需要逐项处理 Shift 后的配对字符。
    switch ( key ) {
    // 空格不受修饰键影响。
    case Input::Key::Space: return U' ';
    // 反引号、减号和等号构成主键盘上方的符号键。
    case Input::Key::GraveAccent: return mods.shift ? U'~' : U'`';
    case Input::Key::Minus: return mods.shift ? U'_' : U'-';
    case Input::Key::Equal: return mods.shift ? U'+' : U'=';
    // 方括号与反斜杠在 Shift 下转换为花括号和竖线。
    case Input::Key::LeftBracket: return mods.shift ? U'{' : U'[';
    case Input::Key::RightBracket: return mods.shift ? U'}' : U']';
    case Input::Key::Backslash: return mods.shift ? U'|' : U'\\';
    // 分号和引号键在 Shift 下转换为冒号和双引号。
    case Input::Key::Semicolon: return mods.shift ? U':' : U';';
    case Input::Key::Apostrophe: return mods.shift ? U'"' : U'\'';
    // 逗号、句点和斜杠在 Shift 下转换为尖括号和问号。
    case Input::Key::Comma: return mods.shift ? U'<' : U',';
    case Input::Key::Period: return mods.shift ? U'>' : U'.';
    case Input::Key::Slash: return mods.shift ? U'?' : U'/';
    // 小键盘数字直接产生数字字符，不在此处解释 Num Lock 导航模式。
    case Input::Key::KP_0: return U'0';
    case Input::Key::KP_1: return U'1';
    case Input::Key::KP_2: return U'2';
    case Input::Key::KP_3: return U'3';
    case Input::Key::KP_4: return U'4';
    case Input::Key::KP_5: return U'5';
    case Input::Key::KP_6: return U'6';
    case Input::Key::KP_7: return U'7';
    case Input::Key::KP_8: return U'8';
    case Input::Key::KP_9: return U'9';
    // 小键盘运算键产生与其标签一致的 ASCII 运算符。
    case Input::Key::KP_Decimal: return U'.';
    case Input::Key::KP_Divide: return U'/';
    case Input::Key::KP_Multiply: return U'*';
    case Input::Key::KP_Subtract: return U'-';
    case Input::Key::KP_Add: return U'+';
    case Input::Key::KP_Equal: return U'=';
    // 主键盘与小键盘确认键统一产生换行，Tab 产生水平制表符。
    case Input::Key::KP_Enter: return U'\n';
    case Input::Key::Enter: return U'\n';
    case Input::Key::Tab: return U'\t';
    // 功能键、导航键和修饰键均不是独立文本输入。
    // 返回零由调用方解释为“没有码点”，而不是 Unicode NUL 输入。
    default: return 0;
    }
}
}  // namespace MMM::Event::Translator
