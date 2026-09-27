#pragma once

#include "ui/ICanvasWorkspaceService.h"
#include "ui/IUIView.h"
#include <string>
#include <unordered_set>
#include <vector>

namespace MMM
{
struct ProjectWorkspaceState;
}

namespace MMM::UI
{

/**
 * @brief 画布 Tab 管理器 (系统级视图)
 *
 * 负责通过画布工作区端口同步逻辑会话与 UIManager 中的渲染画布。
 * 动态处理新画布的创建、动态停靠以及画布关闭事件。
 */
class CanvasTabManager : public IUIView
{
public:
    /// @brief 构造函数
    CanvasTabManager(const std::string& name = "CanvasTabManager");

    /// @brief 析构函数
    ~CanvasTabManager() override = default;

    /// @brief 每帧更新与同步。
    /// @warning UI 热路径：复用工作区快照容器；稳定状态不得引入文件系统访问、
    /// ECS 遍历、完整排序或无条件堆分配。
    void update(UIManager* sourceManager) override;

    /// @brief 准备新项目画布的已保存停靠节点，供首次绘制后核对恢复。
    /// @param workspace 已加载项目工作区；浮动画布不加入待核对集合。
    /// @param entries 实际恢复成功的画布；丢失的谱面和显式打开的单谱面不参与。
    /// @warning 项目打开低频路径：解析 ini，仅保存稳定 ID 和节点 ID。
    void prepareProjectWorkspaceDockRestore(
        const ProjectWorkspaceState&             workspace,
        const std::vector<CanvasWorkspaceEntry>& entries);

    /// @brief 查询项目画布是否仍在首次绘制或停靠核对阶段。
    /// @return 尚不能用当前 ImGui ini 覆盖项目布局时返回 true。
    /// @warning UI 热路径：只读取本地标志和向量是否为空。
    [[nodiscard]] bool projectWorkspaceDockRestorePending() const
    {
        return m_workspaceCanvasFirstFramePending ||
               !m_pendingWorkspaceDocks.empty();
    }

    /// @brief 查询主编辑画布是否已注册，供启动页确定创建顺序。
    /// @warning UI 线程读取既有集合，不扫描会话或创建画布。
    [[nodiscard]] bool hasInitializedCanvas() const
    {
        return !m_initializedCanvases.empty();
    }

    /// @brief 查询工作区是否至少包含一个真实谱面编辑器标签页。
    /// @warning UI 热路径：只读取本帧工作区快照归约出的布尔状态。
    [[nodiscard]] bool hasOpenBeatmapCanvas() const
    {
        return m_hasOpenBeatmapCanvas;
    }

    /// @brief 获取实际类型指针
    void* getActualInstance() override { return this; }

private:
    /// @brief 等待已恢复画布完成首帧绘制，再核对并补回其停靠关系。
    /// @param sourceManager 当前 UI 管理器。
    /// @param entries 本帧逻辑画布条目。
    /// @warning 恢复阶段最多对少量待处理画布执行，常态为空时立即返回。
    void reconcileProjectWorkspaceDocks(
        UIManager*                               sourceManager,
        const std::vector<CanvasWorkspaceEntry>& entries);

    /// @brief 一张需按项目 ini 重新关联 Dock 节点的画布。
    struct PendingWorkspaceDock {
        /// @brief 保存的稳定相机与 ImGui 窗口 ID。
        std::string cameraId;
        /// @brief 保存时的 Dock 节点 ID。
        ImGuiID dockId{ 0 };
        /// @brief 窗口已绘制后发起补停靠的次数，防止异常节点无限重试。
        int attempts{ 0 };
    };

    /// @brief 驱动挂起项目切换时的逐画布关闭流程
    void handlePendingProjectSwitch(
        UIManager*                               sourceManager,
        const std::vector<CanvasWorkspaceEntry>& entries);

    /// @brief 消费逻辑层的画布聚焦请求并转发给对应 Basic2DCanvas。
    void focusPendingSessionCanvas(
        UIManager*                               sourceManager,
        const std::vector<CanvasWorkspaceEntry>& entries);

    /// @brief 已初始化画布的 cameraId 集合
    std::unordered_set<std::string> m_initializedCanvases;

    /// @brief 项目切换关闭流程中当前等待关闭的画布 cameraId
    std::string m_projectSwitchClosingCanvas;

    /// @brief 当前项目切换流程是否已经捕获过工作区状态。
    bool m_capturedProjectSwitchWorkspace{ false };

    /// @brief 每帧复用的轻量画布工作区快照，避免稳定状态重复分配。
    std::vector<CanvasWorkspaceEntry> m_workspaceEntries;

    /// @brief 本帧工作区快照中是否存在非 Logo 占位谱面会话。
    bool m_hasOpenBeatmapCanvas{ false };

    /// @brief 仅在项目恢复期间保留的画布停靠核对列表。
    std::vector<PendingWorkspaceDock> m_pendingWorkspaceDocks;

    /// @brief 项目布局加载所在帧，旧窗口在此帧之前的状态不能作为恢复结果。
    int m_workspaceDockRestoreFrame{ -1 };

    /// @brief 首次绘制前保护已保存布局，避免慢速载图时被周期捕获覆盖。
    bool m_workspaceCanvasFirstFramePending{ false };

    /// @brief 当前项目是否提供可供恢复的 ini；旧项目没有布局时使用默认中心。
    bool m_workspaceHasDockLayout{ false };
};

}  // namespace MMM::UI
