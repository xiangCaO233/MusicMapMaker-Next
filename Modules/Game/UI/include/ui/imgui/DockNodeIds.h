#pragma once

#include <imgui.h>

namespace MMM::UI
{
/// @brief 当前工作区的中心与工具停靠节点编号。
/// @note UI 线程独占。编号由创建 DockBuilder 节点的展示策略发布，
/// 节点不存在时发布零，查询端据此跳过定向停靠。
class DockNodeIds
{
public:
    /// @brief 借用当前中心节点的编号，零表示还没有可用节点。
    static ImGuiID getCenterDockId() { return s_center; }

    /// @brief 创建、还原或清空中心节点后同步发布编号。
    static void setCenterDockId(ImGuiID id) { s_center = id; }

    /// @brief 返回工具停靠节点；移动端可以没有独立工具节点。
    static ImGuiID getToolDockId() { return s_tools; }

    /// @brief 更新工具节点编号，由布局策略而非普通查询者调用。
    static void setToolDockId(ImGuiID id) { s_tools = id; }

private:
    /// @brief 以值保存中心节点身份，无需借用 ImGuiContext 或视图对象。
    static inline ImGuiID s_center{ 0 };
    /// @brief 工具节点尚未建立或移除时以零标记。
    static inline ImGuiID s_tools{ 0 };
};
}  // namespace MMM::UI
