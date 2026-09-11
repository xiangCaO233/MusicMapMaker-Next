#include "graphic/theme/ImGuiTheme.h"

#include <utility>

/// @file
/// @brief 实现 ImGui 主题元数据的值语义持有与样式回调分发。

namespace MMM::Graphic
{

/// @brief 构造一个可注册的 ImGui 主题实例。
/// @param id 持久化选择使用的稳定 ID。
/// @param displayName 设置界面展示名称。
/// @param origin 主题来自内置定义或外部插件。
/// @param baseThemeId 插件应用前继承的内置主题 ID。
/// @param sourcePath 插件入口路径；内置主题为空路径。
/// @param applyFunction 把主题字段写入目标样式的回调。
ImGuiTheme::ImGuiTheme(std::string id, std::string displayName,
                       ImGuiThemeOrigin origin, std::string baseThemeId,
                       std::filesystem::path sourcePath,
                       ApplyFunction         applyFunction)
    : m_id(std::move(id))
    , m_displayName(std::move(displayName))
    , m_origin(origin)
    , m_baseThemeId(std::move(baseThemeId))
    , m_sourcePath(std::move(sourcePath))
    , m_applyFunction(std::move(applyFunction))
{
    // 所有字符串、路径和回调都转移为实例所有，注册表无需保留构造参数。
}

/// @brief 将主题定义写入调用方提供的 ImGui 样式。
/// @param style 待修改的样式对象。
void ImGuiTheme::apply(ImGuiStyle& style) const
{
    // 空回调表示仅携带元数据的无操作主题，调用保持安全且不修改样式。
    if ( m_applyFunction ) {
        // 回调同步执行，应用顺序和基础主题组合由上层注册表控制。
        m_applyFunction(style);
    }
}

}  // namespace MMM::Graphic
