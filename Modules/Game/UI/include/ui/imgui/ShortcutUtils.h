#pragma once

#include "common/EditTool.h"
#include "config/ShortcutConfig.h"
#include "imgui.h"
#include "ui/imgui/WindowIdUtils.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace MMM::Config
{
/// @brief 工具映射仅借用设置，配置成员的读取实现在源文件。
struct EditorSettings;
}  // namespace MMM::Config

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
const ShortcutKeyEntry* findKeyEntry(std::string_view stableName);

/// @brief 判断当前修饰键是否与绑定完全一致。
/// @param binding 待匹配的快捷键绑定。
/// @return 完全一致时返回 true。
/// @pre 必须存在有效 ImGui 上下文。
bool modifiersMatch(const Config::ShortcutBinding& binding);

/// @brief 将快捷键绑定格式化为菜单和设置页显示文本。
/// @param binding 待显示的快捷键绑定。
/// @return 已格式化文本；未启用时返回空字符串。
/// @note 格式化只用于展示，配置持久化仍保存各修饰键字段和稳定主键名。
std::string formatShortcut(const Config::ShortcutBinding& binding);

/// @brief 判断指定快捷键是否在当前帧按下。
/// @param binding 待检查的快捷键绑定。
/// @param repeat 是否允许按住重复触发。
/// @return 本帧触发时返回 true。
/// @warning UI 热路径：菜单和画布每帧调用；只做少量按键查询和字符串比较。
/// @pre 必须存在有效 ImGui 上下文。
bool isShortcutPressed(const Config::ShortcutBinding& binding,
                       bool                           repeat = false);

/// @brief 通过稳定动作身份查询绑定，菜单、画布和新方案共用同一个入口。
/// @details
/// 仅在默认硬编码组合未改动时保留其宽松修饰键语义，用户重绑定严格匹配。
/// @warning 每帧只读配置和固定目录，不分配或保存配置。
/// @param action 固定动作字段名，目录查找失败即不触发任何编辑操作。
/// @param repeat 调用者决定是否重复，工具切换和菜单通常只消费首帧。
/// @param legacyModifiers
/// 当前表面对旧默认组合的额外修饰键要求；表格用零保持严格匹配。
/// @return 当前方案解析后的绑定在这一帧命中时返回 true。
/// @details 动作身份与主键不同，重映射复制不能顺带改掉所有字母 C 的操作。
/// 配置引用由 AppConfig 持有，方案返回的引用同样必须稳定。
/// 禁用或空键立即拒绝，不以其他动作的默认键补齐。
/// 只有完整等于旧默认组合时才启用历史修饰键通配规则。
/// 新绑定采用严格匹配，避免额外 Ctrl 或 Shift 同时触发多个编辑动作。
/// 主键查询也经过表面输入映射，按下、保持与释放使用同一键身份。
/// 设置页录制仍读取物理键，不把已经解释后的键再次保存为配置。
/// 调用者必须先检查弹窗、文本焦点和录制隔离，本 helper 不取得输入所有权。
/// BPM 和主画布继续各自决定动作归属，不能单凭绑定命中转交到后台谱面。
/// 目录是固定小表，不在热路径生成哈希字典或扫描文件系统。
/// 逻辑命令与保存流程在命中后单独执行，此函数本身没有编辑副作用。
bool isActionShortcutPressed(
    std::string_view action, bool repeat = false,
    std::optional<unsigned> legacyModifiers = std::nullopt);

/// @brief 获取快捷键录制全局状态。
/// @return 当前进程内共享的录制状态。
/// @note 状态只由 UI 线程访问，不使用原子同步。
bool& shortcutRecordingActiveState();

/// @brief 设置当前是否正在录制快捷键。
/// @param active 是否处于录制状态。
/// @note 录制期间上层应暂停普通快捷键派发，避免新绑定同时执行命令。
void setShortcutRecordingActive(bool active);

/// @brief 判断当前是否正在录制快捷键。
/// @return 正在录制时返回 true。
/// @warning UI 热路径：只读取 UI 线程函数静态布尔值。
bool isShortcutRecordingActive();

using WindowIdUtils::stableWindowId;

/// @brief 判断稳定窗口 ID 是否为主谱面画布。
/// @param stableId ImGui 窗口稳定 ID。
/// @return 主谱面画布窗口返回 true。
/// @note Canvas_ 前缀覆盖多谱面标签，Basic2DCanvas 保留旧单画布名称。
bool isMainCanvasWindowId(std::string_view stableId);

/// @brief 判断当前键盘焦点是否允许触发谱面物件编辑快捷键。
/// @return 当前没有焦点窗口或焦点在主谱面画布时返回 true。
/// @warning UI 热路径：只读取当前 ImGui 导航窗口名称。
bool focusedWindowAllowsCanvasEditingShortcuts();

/// @brief 判断谱面物件编辑快捷键是否应让位给当前 UI 焦点。
/// @return 文本输入、弹窗、活跃控件或非主画布窗口聚焦时返回 true。
/// @warning UI 热路径：每帧快捷键派发前调用；只读取 ImGui 当前输入状态。
/// @note 此门禁用于物件编辑命令，不替代播放等具有独立路由规则的快捷键。
bool shouldBlockCanvasEditingShortcuts();

/// @brief 录制当前帧刚按下的快捷键。
/// @return 捕获到的快捷键绑定；没有按键输入时为空。
/// @note Ctrl、Shift、Alt 与 Super 只作为修饰键记录，不会单独成为主键。
/// @warning UI 热路径：录制弹窗打开时每帧扫描固定映射表，不进行文件访问。
/// @pre 必须存在已开始输入帧的 ImGui 上下文。
std::optional<Config::ShortcutBinding> capturePressedShortcut();

/// @brief 获取指定编辑工具对应的快捷键绑定。
/// @param settings 编辑器设置。
/// @param tool 编辑工具类型。
/// @return 指定工具的快捷键绑定。
/// @note 返回引用与 settings 生命周期绑定。
/// @warning 热路径查询：只执行枚举 switch，不复制绑定字符串。
const Config::ShortcutBinding& getToolShortcut(
    const Config::EditorSettings& settings, Logic::EditTool tool);

/// @brief 获取指定编辑工具对应的可写快捷键绑定。
/// @param settings 编辑器设置。
/// @param tool 编辑工具类型。
/// @return 指定工具的快捷键绑定。
/// @note 设置页通过该引用原地更新绑定，保存由外层统一执行。
/// @warning 返回可写引用，调用方必须在修改后触发配置同步和持久化。
Config::ShortcutBinding& getToolShortcut(Config::EditorSettings& settings,
                                         Logic::EditTool         tool);

}  // namespace MMM::UI::ShortcutUtils
