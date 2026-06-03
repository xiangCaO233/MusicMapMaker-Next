#pragma once

#include "common/LogicCommands.h"
#include "config/EditorSettings.h"
#include "imgui.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace MMM::UI::ShortcutUtils
{

/// @brief UI 层可录制快捷键的稳定名称与 ImGuiKey 映射。
struct ShortcutKeyEntry {
    /// @brief ImGui 按键枚举。
    ImGuiKey key{ ImGuiKey_None };

    /// @brief 写入配置文件的稳定按键名称。
    std::string_view stableName;

    /// @brief 显示给用户的按键名称。
    std::string_view displayName;
};

/// @brief 获取可录制按键表。
/// @return 稳定按键映射表。
inline constexpr auto shortcutKeyEntries()
{
    return std::to_array<ShortcutKeyEntry>({
        { ImGuiKey_A, "A", "A" },
        { ImGuiKey_B, "B", "B" },
        { ImGuiKey_C, "C", "C" },
        { ImGuiKey_D, "D", "D" },
        { ImGuiKey_E, "E", "E" },
        { ImGuiKey_F, "F", "F" },
        { ImGuiKey_G, "G", "G" },
        { ImGuiKey_H, "H", "H" },
        { ImGuiKey_I, "I", "I" },
        { ImGuiKey_J, "J", "J" },
        { ImGuiKey_K, "K", "K" },
        { ImGuiKey_L, "L", "L" },
        { ImGuiKey_M, "M", "M" },
        { ImGuiKey_N, "N", "N" },
        { ImGuiKey_O, "O", "O" },
        { ImGuiKey_P, "P", "P" },
        { ImGuiKey_Q, "Q", "Q" },
        { ImGuiKey_R, "R", "R" },
        { ImGuiKey_S, "S", "S" },
        { ImGuiKey_T, "T", "T" },
        { ImGuiKey_U, "U", "U" },
        { ImGuiKey_V, "V", "V" },
        { ImGuiKey_W, "W", "W" },
        { ImGuiKey_X, "X", "X" },
        { ImGuiKey_Y, "Y", "Y" },
        { ImGuiKey_Z, "Z", "Z" },
        { ImGuiKey_0, "0", "0" },
        { ImGuiKey_1, "1", "1" },
        { ImGuiKey_2, "2", "2" },
        { ImGuiKey_3, "3", "3" },
        { ImGuiKey_4, "4", "4" },
        { ImGuiKey_5, "5", "5" },
        { ImGuiKey_6, "6", "6" },
        { ImGuiKey_7, "7", "7" },
        { ImGuiKey_8, "8", "8" },
        { ImGuiKey_9, "9", "9" },
        { ImGuiKey_F1, "F1", "F1" },
        { ImGuiKey_F2, "F2", "F2" },
        { ImGuiKey_F3, "F3", "F3" },
        { ImGuiKey_F4, "F4", "F4" },
        { ImGuiKey_F5, "F5", "F5" },
        { ImGuiKey_F6, "F6", "F6" },
        { ImGuiKey_F7, "F7", "F7" },
        { ImGuiKey_F8, "F8", "F8" },
        { ImGuiKey_F9, "F9", "F9" },
        { ImGuiKey_F10, "F10", "F10" },
        { ImGuiKey_F11, "F11", "F11" },
        { ImGuiKey_F12, "F12", "F12" },
        { ImGuiKey_Space, "Space", "Space" },
        { ImGuiKey_Enter, "Enter", "Enter" },
        { ImGuiKey_Tab, "Tab", "Tab" },
        { ImGuiKey_Backspace, "Backspace", "Backspace" },
        { ImGuiKey_Delete, "Delete", "Delete" },
        { ImGuiKey_Insert, "Insert", "Insert" },
        { ImGuiKey_Home, "Home", "Home" },
        { ImGuiKey_End, "End", "End" },
        { ImGuiKey_PageUp, "PageUp", "PageUp" },
        { ImGuiKey_PageDown, "PageDown", "PageDown" },
        { ImGuiKey_LeftArrow, "LeftArrow", "Left" },
        { ImGuiKey_RightArrow, "RightArrow", "Right" },
        { ImGuiKey_UpArrow, "UpArrow", "Up" },
        { ImGuiKey_DownArrow, "DownArrow", "Down" },
        { ImGuiKey_Minus, "Minus", "-" },
        { ImGuiKey_Equal, "Equal", "=" },
        { ImGuiKey_LeftBracket, "LeftBracket", "[" },
        { ImGuiKey_RightBracket, "RightBracket", "]" },
        { ImGuiKey_Backslash, "Backslash", "\\" },
        { ImGuiKey_Semicolon, "Semicolon", ";" },
        { ImGuiKey_Apostrophe, "Apostrophe", "'" },
        { ImGuiKey_Comma, "Comma", "," },
        { ImGuiKey_Period, "Period", "." },
        { ImGuiKey_Slash, "Slash", "/" },
        { ImGuiKey_GraveAccent, "GraveAccent", "`" },
    });
}

/// @brief 按稳定名称查找按键映射。
/// @param stableName 配置文件中的稳定按键名称。
/// @return 匹配到的按键映射；不存在时返回空。
inline const ShortcutKeyEntry* findKeyEntry(std::string_view stableName)
{
    static constexpr auto entries = shortcutKeyEntries();
    for ( const auto& entry : entries ) {
        if ( entry.stableName == stableName ) {
            return &entry;
        }
    }
    return nullptr;
}

/// @brief 判断当前修饰键是否与绑定完全一致。
/// @param binding 待匹配的快捷键绑定。
/// @return 完全一致时返回 true。
inline bool modifiersMatch(const Config::ShortcutBinding& binding)
{
    const ImGuiIO& io = ImGui::GetIO();
    return io.KeyCtrl == binding.ctrl && io.KeyShift == binding.shift &&
           io.KeyAlt == binding.alt && io.KeySuper == binding.super;
}

/// @brief 将快捷键绑定格式化为菜单和设置页显示文本。
/// @param binding 待显示的快捷键绑定。
/// @return 已格式化文本；未启用时返回空字符串。
inline std::string formatShortcut(const Config::ShortcutBinding& binding)
{
    if ( !binding.enabled || binding.key.empty() ) {
        return {};
    }

    std::string text;
    auto        appendPart = [&text](std::string_view part) {
        if ( !text.empty() ) {
            text += "+";
        }
        text += part;
    };

    if ( binding.ctrl ) {
        appendPart("Ctrl");
    }
    if ( binding.shift ) {
        appendPart("Shift");
    }
    if ( binding.alt ) {
        appendPart("Alt");
    }
    if ( binding.super ) {
        appendPart("Super");
    }

    if ( const auto* entry = findKeyEntry(binding.key) ) {
        appendPart(entry->displayName);
    } else {
        appendPart(binding.key);
    }
    return text;
}

/// @brief 判断指定快捷键是否在当前帧按下。
/// @param binding 待检查的快捷键绑定。
/// @param repeat 是否允许按住重复触发。
/// @return 本帧触发时返回 true。
/// @warning UI 热路径：菜单和画布每帧调用；只做少量按键查询和字符串比较。
inline bool isShortcutPressed(const Config::ShortcutBinding& binding,
                              bool                           repeat = false)
{
    if ( !binding.enabled || binding.key.empty() || !modifiersMatch(binding) ) {
        return false;
    }

    const auto* entry = findKeyEntry(binding.key);
    if ( !entry ) {
        return false;
    }
    return ImGui::IsKeyPressed(entry->key, repeat);
}

/// @brief 获取快捷键录制全局状态。
/// @return 当前进程内共享的录制状态。
inline bool& shortcutRecordingActiveState()
{
    static bool active = false;
    return active;
}

/// @brief 设置当前是否正在录制快捷键。
/// @param active 是否处于录制状态。
inline void setShortcutRecordingActive(bool active)
{
    shortcutRecordingActiveState() = active;
}

/// @brief 判断当前是否正在录制快捷键。
/// @return 正在录制时返回 true。
inline bool isShortcutRecordingActive()
{
    return shortcutRecordingActiveState();
}

/// @brief 录制当前帧刚按下的快捷键。
/// @return 捕获到的快捷键绑定；没有按键输入时为空。
inline std::optional<Config::ShortcutBinding> capturePressedShortcut()
{
    const ImGuiIO&        io      = ImGui::GetIO();
    static constexpr auto entries = shortcutKeyEntries();
    for ( const auto& entry : entries ) {
        if ( ImGui::IsKeyPressed(entry.key, false) ) {
            Config::ShortcutBinding binding;
            binding.enabled = true;
            binding.key     = std::string(entry.stableName);
            binding.ctrl    = io.KeyCtrl;
            binding.shift   = io.KeyShift;
            binding.alt     = io.KeyAlt;
            binding.super   = io.KeySuper;
            return binding;
        }
    }
    return std::nullopt;
}

/// @brief 获取指定编辑工具对应的快捷键绑定。
/// @param settings 编辑器设置。
/// @param tool 编辑工具类型。
/// @return 指定工具的快捷键绑定。
inline const Config::ShortcutBinding& getToolShortcut(
    const Config::EditorSettings& settings, Logic::EditTool tool)
{
    switch ( tool ) {
    case Logic::EditTool::Move: return settings.shortcutConfig.toolMove;
    case Logic::EditTool::Marquee: return settings.shortcutConfig.toolMarquee;
    case Logic::EditTool::Draw: return settings.shortcutConfig.toolDraw;
    case Logic::EditTool::ColorBrush:
        return settings.shortcutConfig.toolColorBrush;
    case Logic::EditTool::ColorEraser:
        return settings.shortcutConfig.toolColorEraser;
    }
    return settings.shortcutConfig.toolMove;
}

/// @brief 获取指定编辑工具对应的可写快捷键绑定。
/// @param settings 编辑器设置。
/// @param tool 编辑工具类型。
/// @return 指定工具的快捷键绑定。
inline Config::ShortcutBinding& getToolShortcut(
    Config::EditorSettings& settings, Logic::EditTool tool)
{
    switch ( tool ) {
    case Logic::EditTool::Move: return settings.shortcutConfig.toolMove;
    case Logic::EditTool::Marquee: return settings.shortcutConfig.toolMarquee;
    case Logic::EditTool::Draw: return settings.shortcutConfig.toolDraw;
    case Logic::EditTool::ColorBrush:
        return settings.shortcutConfig.toolColorBrush;
    case Logic::EditTool::ColorEraser:
        return settings.shortcutConfig.toolColorEraser;
    }
    return settings.shortcutConfig.toolMove;
}

}  // namespace MMM::UI::ShortcutUtils
