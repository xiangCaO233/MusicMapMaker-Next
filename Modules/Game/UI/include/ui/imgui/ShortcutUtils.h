#pragma once

#include "common/EditTool.h"
#include "config/EditorSettings.h"
#include "imgui.h"
#include "ui/imgui/WindowIdUtils.h"
#include <imgui_internal.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace MMM::UI::ShortcutUtils
{

/// @file ShortcutUtils.h
/// @brief UI 快捷键的稳定配置名称、显示文本、输入匹配和录制辅助函数。
/// @details 配置只保存平台无关稳定名称，运行时通过固定表映射 ImGuiKey；画布
/// 编辑快捷键还需通过文本输入、弹窗、活动控件和焦点窗口门禁。

/// @brief UI 层可录制快捷键的稳定名称与 ImGuiKey 映射。
/// @note string_view 均指向静态字符串字面量，可安全存入 constexpr 数组。
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
/// @note 条目顺序同时决定同帧多键按下时录制器优先捕获的按键。
/// @note 修饰键不在表中，它们作为 ShortcutBinding 的独立布尔字段录制。
inline constexpr auto shortcutKeyEntries()
{
    // 字母键使用相同稳定名称与显示名称。
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
        // 主键盘数字同样以单字符名称持久化。
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
        // 功能键覆盖 F1 到 F12，名称直接对应 ImGui 语义。
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
        // 常用导航和编辑控制键使用可读英文稳定名称。
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
        // 标点稳定名称描述物理按键，显示名称使用实际符号。
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
/// @warning UI 热路径：线性扫描固定表，不分配字符串。
/// @note 稳定名称区分大小写，避免配置中出现多个等价写法。
inline const ShortcutKeyEntry* findKeyEntry(std::string_view stableName)
{
    // 函数内静态 constexpr 避免每次查询重新构造映射数组。
    static constexpr auto entries = shortcutKeyEntries();
    // 表规模固定且较小，线性扫描保持代码和序列化映射简单。
    for ( const auto& entry : entries ) {
        if ( entry.stableName == stableName ) {
            // 返回地址指向静态数组，生命周期覆盖整个进程。
            return &entry;
        }
    }
    // 未知名称可能来自较新配置，调用方决定回退显示或拒绝触发。
    return nullptr;
}

/// @brief 判断当前修饰键是否与绑定完全一致。
/// @param binding 待匹配的快捷键绑定。
/// @return 完全一致时返回 true。
/// @pre 必须存在有效 ImGui 上下文。
inline bool modifiersMatch(const Config::ShortcutBinding& binding)
{
    // ImGuiIO 聚合左右修饰键，本配置同样不区分左右侧。
    const ImGuiIO& io = ImGui::GetIO();
    // 使用完全匹配，额外按住的修饰键也会阻止快捷键触发。
    return io.KeyCtrl == binding.ctrl && io.KeyShift == binding.shift &&
           io.KeyAlt == binding.alt && io.KeySuper == binding.super;
}

/// @brief 将快捷键绑定格式化为菜单和设置页显示文本。
/// @param binding 待显示的快捷键绑定。
/// @return 已格式化文本；未启用时返回空字符串。
/// @note 格式化只用于展示，配置持久化仍保存各修饰键字段和稳定主键名。
inline std::string formatShortcut(const Config::ShortcutBinding& binding)
{
    // 禁用或缺少主键的绑定不在菜单右侧显示。
    if ( !binding.enabled || binding.key.empty() ) {
        return {};
    }

    // appendPart 只在已有内容后插入加号，避免前导或尾随分隔符。
    std::string text;
    auto        appendPart = [&text](std::string_view part) {
        if ( !text.empty() ) {
            // 修饰键与主键统一使用紧凑的加号连接格式。
            text += "+";
        }
        text += part;
    };

    // 修饰键按 Ctrl、Shift、Alt、Super 的稳定顺序显示。
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
        // 已知按键使用面向用户的 displayName，例如方向键显示 Left。
        appendPart(entry->displayName);
    } else {
        // 未知未来按键仍显示原配置文本，避免设置页呈现空值。
        appendPart(binding.key);
    }
    return text;
}

/// @brief 判断指定快捷键是否在当前帧按下。
/// @param binding 待检查的快捷键绑定。
/// @param repeat 是否允许按住重复触发。
/// @return 本帧触发时返回 true。
/// @warning UI 热路径：菜单和画布每帧调用；只做少量按键查询和字符串比较。
/// @pre 必须存在有效 ImGui 上下文。
inline bool isShortcutPressed(const Config::ShortcutBinding& binding,
                              bool                           repeat = false)
{
    // 在查询主键前排除禁用、空键和修饰键不完全匹配的绑定。
    if ( !binding.enabled || binding.key.empty() || !modifiersMatch(binding) ) {
        return false;
    }

    // 配置中的稳定名称必须存在于当前版本映射表才能触发。
    const auto* entry = findKeyEntry(binding.key);
    if ( !entry ) {
        return false;
    }
    // repeat 由调用方决定按住时是否按 ImGui 重复率持续触发。
    return ImGui::IsKeyPressed(entry->key, repeat);
}

/// @brief 获取快捷键录制全局状态。
/// @return 当前进程内共享的录制状态。
/// @note 状态只由 UI 线程访问，不使用原子同步。
inline bool& shortcutRecordingActiveState()
{
    // 单一函数静态变量供设置页和全局快捷键派发共同访问。
    static bool active = false;
    return active;
}

/// @brief 设置当前是否正在录制快捷键。
/// @param active 是否处于录制状态。
/// @note 录制期间上层应暂停普通快捷键派发，避免新绑定同时执行命令。
inline void setShortcutRecordingActive(bool active)
{
    shortcutRecordingActiveState() = active;
}

/// @brief 判断当前是否正在录制快捷键。
/// @return 正在录制时返回 true。
/// @warning UI 热路径：只读取 UI 线程函数静态布尔值。
inline bool isShortcutRecordingActive()
{
    return shortcutRecordingActiveState();
}

using WindowIdUtils::stableWindowId;

/// @brief 判断稳定窗口 ID 是否为主谱面画布。
/// @param stableId ImGui 窗口稳定 ID。
/// @return 主谱面画布窗口返回 true。
/// @note Canvas_ 前缀覆盖多谱面标签，Basic2DCanvas 保留旧单画布名称。
inline bool isMainCanvasWindowId(std::string_view stableId)
{
    return stableId.starts_with("Canvas_") || stableId == "Basic2DCanvas";
}

/// @brief 判断当前键盘焦点是否允许触发谱面物件编辑快捷键。
/// @return 当前没有焦点窗口或焦点在主谱面画布时返回 true。
/// @warning UI 热路径：只读取当前 ImGui 导航窗口名称。
inline bool focusedWindowAllowsCanvasEditingShortcuts()
{
    // 启动、测试或未建立导航焦点时允许画布级快捷键继续判断。
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if ( !context || !context->NavWindow || !context->NavWindow->Name ) {
        return true;
    }

    // 动态窗口标题先提取最后一个 ### 后的稳定 ID。
    return isMainCanvasWindowId(stableWindowId(context->NavWindow->Name));
}

/// @brief 判断谱面物件编辑快捷键是否应让位给当前 UI 焦点。
/// @return 文本输入、弹窗、活跃控件或非主画布窗口聚焦时返回 true。
/// @warning UI 热路径：每帧快捷键派发前调用；只读取 ImGui 当前输入状态。
/// @note 此门禁用于物件编辑命令，不替代播放等具有独立路由规则的快捷键。
inline bool shouldBlockCanvasEditingShortcuts()
{
    // WantTextInput 覆盖文本控件，AnyItemActive 覆盖拖动和键盘编辑控件。
    const ImGuiIO& io = ImGui::GetIO();
    if ( io.WantTextInput || ImGui::IsAnyItemActive() ) {
        return true;
    }
    // 任意层级弹窗打开时保留键盘给弹窗，即使焦点仍报告主画布。
    if ( ImGui::IsPopupOpen(
             nullptr,
             ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ) {
        return true;
    }
    // 最后以导航焦点窗口限制物件编辑命令只在主画布触发。
    return !focusedWindowAllowsCanvasEditingShortcuts();
}

/// @brief 录制当前帧刚按下的快捷键。
/// @return 捕获到的快捷键绑定；没有按键输入时为空。
/// @note Ctrl、Shift、Alt 与 Super 只作为修饰键记录，不会单独成为主键。
/// @warning UI 热路径：录制弹窗打开时每帧扫描固定映射表，不进行文件访问。
/// @pre 必须存在已开始输入帧的 ImGui 上下文。
inline std::optional<Config::ShortcutBinding> capturePressedShortcut()
{
    // 修饰键状态与捕获主键使用同一帧 ImGuiIO 快照。
    const ImGuiIO&        io      = ImGui::GetIO();
    static constexpr auto entries = shortcutKeyEntries();
    // 按表顺序选择本帧第一个刚按下且不重复触发的主键。
    for ( const auto& entry : entries ) {
        if ( ImGui::IsKeyPressed(entry.key, false) ) {
            // 新录制绑定始终启用，并使用稳定名称而非显示文本。
            Config::ShortcutBinding binding;
            binding.enabled = true;
            binding.key     = std::string(entry.stableName);
            // 四个聚合修饰键完整复制，保证后续完全匹配语义一致。
            binding.ctrl  = io.KeyCtrl;
            binding.shift = io.KeyShift;
            binding.alt   = io.KeyAlt;
            binding.super = io.KeySuper;
            // 捕获首个主键后立即返回，不组合多个非修饰键。
            return binding;
        }
    }
    // 当前帧没有支持的主键边沿。
    return std::nullopt;
}

/// @brief 获取指定编辑工具对应的快捷键绑定。
/// @param settings 编辑器设置。
/// @param tool 编辑工具类型。
/// @return 指定工具的快捷键绑定。
/// @note 返回引用与 settings 生命周期绑定。
/// @warning 热路径查询：只执行枚举 switch，不复制绑定字符串。
inline const Config::ShortcutBinding& getToolShortcut(
    const Config::EditorSettings& settings, Logic::EditTool tool)
{
    // 只读重载直接返回 EditorSettings 内部绑定，调用方不得延长其生命周期。
    switch ( tool ) {
    case Logic::EditTool::Move: return settings.shortcutConfig.toolMove;
    case Logic::EditTool::Marquee: return settings.shortcutConfig.toolMarquee;
    case Logic::EditTool::Draw: return settings.shortcutConfig.toolDraw;
    case Logic::EditTool::ColorBrush:
        return settings.shortcutConfig.toolColorBrush;
    case Logic::EditTool::ColorEraser:
        return settings.shortcutConfig.toolColorEraser;
    case Logic::EditTool::Layout: break;
    }
    // Layout 当前没有独立绑定，回退 Move 保持返回引用始终有效。
    return settings.shortcutConfig.toolMove;
}

/// @brief 获取指定编辑工具对应的可写快捷键绑定。
/// @param settings 编辑器设置。
/// @param tool 编辑工具类型。
/// @return 指定工具的快捷键绑定。
/// @note 设置页通过该引用原地更新绑定，保存由外层统一执行。
/// @warning 返回可写引用，调用方必须在修改后触发配置同步和持久化。
inline Config::ShortcutBinding& getToolShortcut(
    Config::EditorSettings& settings, Logic::EditTool tool)
{
    // 可写重载与只读重载保持完全相同的工具到字段映射。
    switch ( tool ) {
    case Logic::EditTool::Move: return settings.shortcutConfig.toolMove;
    case Logic::EditTool::Marquee: return settings.shortcutConfig.toolMarquee;
    case Logic::EditTool::Draw: return settings.shortcutConfig.toolDraw;
    case Logic::EditTool::ColorBrush:
        return settings.shortcutConfig.toolColorBrush;
    case Logic::EditTool::ColorEraser:
        return settings.shortcutConfig.toolColorEraser;
    case Logic::EditTool::Layout: break;
    }
    // 未单独配置的 Layout 返回可写 Move 绑定作为兼容回退。
    return settings.shortcutConfig.toolMove;
}

}  // namespace MMM::UI::ShortcutUtils
