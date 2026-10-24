#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string>

namespace MMM::Config
{

/// @brief 单个自定义快捷键绑定。
struct ShortcutBinding {
    /// @brief 是否启用该快捷键；关闭时该动作没有键盘入口。
    bool enabled{ true };

    /// @brief 稳定按键名称，由 UI 层映射到 ImGuiKey。
    std::string key;

    /// @brief 是否要求 Ctrl 修饰键。
    bool ctrl{ false };

    /// @brief 是否要求 Shift 修饰键。
    bool shift{ false };

    /// @brief 是否要求 Alt 修饰键。
    bool alt{ false };

    /// @brief 是否要求 Super/Command 修饰键。
    bool super{ false };
};

/// @brief 判断两个已启用快捷键绑定是否使用同一按键组合。
/// @param lhs 第一个快捷键绑定。
/// @param rhs 第二个快捷键绑定。
/// @return 两者均有效且按键及全部修饰键相同时返回 true。
/// @warning 设置页热路径：仅比较两个短字符串和四个修饰键标志。
[[nodiscard]] constexpr bool shortcutBindingsConflict(
    const ShortcutBinding& lhs, const ShortcutBinding& rhs)
{
    return lhs.enabled && rhs.enabled && !lhs.key.empty() && !rhs.key.empty() &&
           lhs.key == rhs.key && lhs.ctrl == rhs.ctrl &&
           lhs.shift == rhs.shift && lhs.alt == rhs.alt &&
           lhs.super == rhs.super;
}

/// @brief 将快捷键绑定序列化为 JSON。
void to_json(nlohmann::json& json, const ShortcutBinding& binding);
/// @brief 从 JSON 读取快捷键绑定。
void from_json(const nlohmann::json& json, ShortcutBinding& binding);

/// @brief 编辑器可自定义快捷键配置。
struct ShortcutConfig {
    /// @brief 切换到移动工具。
    ShortcutBinding toolMove{ true, "1", false, false, false, false };

    /// @brief 切换到框选工具。
    ShortcutBinding toolMarquee{ true, "2", false, false, false, false };

    /// @brief 切换到绘制工具。
    ShortcutBinding toolDraw{ true, "3", false, false, false, false };

    /// @brief 切换到配色笔刷工具。
    ShortcutBinding toolColorBrush{ true, "4", false, false, false, false };

    /// @brief 切换到配色橡皮工具。
    ShortcutBinding toolColorEraser{ true, "5", false, false, false, false };

    /// @brief 镜像当前选中物件。
    ShortcutBinding mirror{ true, "M", true, false, false, false };

    /// @brief 镜像粘贴剪贴板物件。
    ShortcutBinding mirrorPaste{ true, "V", true, true, false, false };

    /// @brief 打开选中物件批量音量编辑器。
    ShortcutBinding editSelectedVolume{ false, "", false, false, false, false };

    /// @brief 为当前选中的单个物件添加批注。
    ShortcutBinding addSelectedAnnotation{
        true, "R", true, false, false, false
    };

    /// @brief 删除当前选中物件。
    ShortcutBinding deleteSelected{
        true, "Delete", false, false, false, false
    };

    /// @brief 切换谱面播放与暂停状态。
    ShortcutBinding togglePlayback{ true, "Space", false, false, false, false };

    /// @brief BPM 测量工具聚焦时在视图中心添加段落。
    ShortcutBinding addBpmSegmentAtViewCenter{ true,  "B",  true,
                                               false, true, false };

    /// @brief 切换反转滚动方向。
    ShortcutBinding toggleReverseScroll{
        false, "", false, false, false, false
    };

    /// @brief 切换滚动磁吸。
    ShortcutBinding toggleScrollSnap{ false, "", false, false, false, false };

    /// @brief 切换吸附向下取整。
    ShortcutBinding toggleSnapFloor{ false, "", false, false, false, false };

    /// @brief 切换 SCROLLTIMING 视觉映射。
    ShortcutBinding toggleScrollTimingMapping{ false, "",    false,
                                               false, false, false };

    /// @brief 切换分拍线显示。
    ShortcutBinding toggleBeatLines{ false, "", false, false, false, false };

    /// @brief 切换播放时滚动停止播放。
    ShortcutBinding toggleStopPlaybackOnScroll{ false, "",    false,
                                                false, false, false };

    /// @brief 切换打击音效。
    ShortcutBinding toggleHitSfx{ false, "", false, false, false, false };

    /// @brief 切换打击特效。
    ShortcutBinding toggleHitEffects{ false, "", false, false, false, false };

    /// @brief 切换同主音轨画布同步。
    ShortcutBinding toggleSyncSameMainAudio{ false, "",    false,
                                             false, false, false };
};

/// @brief 将快捷键配置序列化为 JSON。
void to_json(nlohmann::json& json, const ShortcutConfig& config);
/// @brief 从 JSON 读取快捷键配置。
void from_json(const nlohmann::json& json, ShortcutConfig& config);

}  // namespace MMM::Config
