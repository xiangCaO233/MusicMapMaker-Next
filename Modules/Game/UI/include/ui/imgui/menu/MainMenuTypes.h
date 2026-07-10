#pragma once

#include "ui/imgui/status/IStatusMessageSink.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace MMM::UI
{
class UIManager;

/// @brief 顶部主菜单的一级菜单标识。
enum class MainMenuId : std::uint8_t {
    /// @brief 文件菜单。
    File,
    /// @brief 编辑菜单。
    Edit,
    /// @brief 工具菜单。
    Tools,
    /// @brief 视图菜单。
    View,
    /// @brief 帮助菜单。
    Help,
    /// @brief 菜单数量哨兵值。
    Count,
};

/// @brief 顶部主菜单数量，用于保存菜单打开和关闭请求。
inline constexpr std::size_t MAIN_MENU_ID_COUNT =
    static_cast<std::size_t>(MainMenuId::Count);

/// @brief 将主菜单标识转换为数组索引。
/// @param id 菜单标识。
/// @return 与菜单标识对应的数组索引。
constexpr std::size_t mainMenuIdIndex(MainMenuId id)
{
    return static_cast<std::size_t>(id);
}

/// @brief 单帧主菜单上下文。
struct MainMenuContext {
    /// @brief 状态消息接收接口，由菜单 action 发布临时反馈。
    IStatusMessageSink& statusMessageSink;

    /// @brief 当前 UI 管理器，可为空。
    UIManager* sourceManager = nullptr;

    /// @brief 当前窗口内容缩放。
    float dpiScale = 1.0f;
};

/// @brief 菜单项文本来源。
enum class MainMenuItemTextKind : std::uint8_t {
    /// @brief 文本是翻译键，需要通过 TR 查询。
    TranslationKey,
    /// @brief 文本是直接显示的字面量。
    Literal,
};

/// @brief 菜单项被激活时携带的上下文载荷。
struct MainMenuItemActivation {
    /// @brief 可选文本载荷，例如最近项目路径。
    std::string textPayload;
};

}  // namespace MMM::UI
