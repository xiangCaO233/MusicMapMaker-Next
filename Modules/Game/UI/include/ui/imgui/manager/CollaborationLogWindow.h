#pragma once

#include "ui/IUIView.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace MMM::Network::Collaboration
{
class CollaborationRoom;
struct CollaborationLogEntry;
struct CollaborationResourceBundle;
}  // namespace MMM::Network::Collaboration

namespace MMM
{
class BeatMap;
class Project;
}  // namespace MMM

namespace MMM::Logic
{
class BeatmapSession;
}

namespace MMM::UI
{
/// @brief 持续驱动协作房间，并为协作侧栏提供内嵌日志。
class CollaborationLogWindow final : public IUIView
{
public:
    /// @brief 创建应用级协作控制器与日志绘制器。
    /// @param name 稳定视图名。
    /// @param room 应用级协作房间。
    CollaborationLogWindow(
        const std::string&                                         name,
        std::shared_ptr<Network::Collaboration::CollaborationRoom> room);
    /// @brief 解除会话观察者和网络回灌回调。
    ~CollaborationLogWindow() override;

    /// @brief 每帧驱动协作房间与会话绑定。
    /// @warning UI 热路径：每帧执行一次有界网络队列轮询，不执行
    /// 文件系统访问或阻塞等待。
    void update(UIManager* sourceManager) override;

    /// @brief 在当前协作侧栏中绘制固定日志区域。
    /// @warning UI 热路径：仅在协作侧栏可见时遍历最多 1000 条内存日志。
    void renderInline();

    /// @brief 该常驻协作控制器始终保留在 UIManager 中。
    [[nodiscard]] bool isOpen() const override;
    /// @brief 忽略通用窗口开关，避免销毁后台协作控制器。
    void setOpen(bool open) override;

private:
    /// @brief 格式化一条结构化日志。
    [[nodiscard]] std::string formatEntry(
        const Network::Collaboration::CollaborationLogEntry& entry) const;

    /// @brief 根据房间生命周期绑定或解除本次协作使用的固定谱面会话。
    void updateSessionBinding();
    /// @brief 房主切换项目或谱面后重新生成并下发资源清单。
    /// @warning UI 热路径：普通帧只比较两个观察指针；仅身份变化的低频分支
    /// 会遍历当前谱面资源引用并启动后台清单生成。
    void refreshHostResources();
    /// @brief 将已经完成的资源包排队绑定到固定协作会话。
    void bindPendingResources();

    /// @brief 应用级协作房间。
    std::shared_ptr<Network::Collaboration::CollaborationRoom> m_room;
    /// @brief 房间启动时固定绑定的谱面会话，切换标签不会改变协作目标。
    std::weak_ptr<Logic::BeatmapSession> m_boundSession;
    /// @brief 早于首个谱面快照完成的访客资源包。
    std::shared_ptr<Network::Collaboration::CollaborationResourceBundle>
        m_pendingResourceBundle;
    /// @brief 最近一次生成房主资源清单时使用的项目观察指针，仅用于身份比较。
    const ::MMM::Project* m_hostResourceProject{ nullptr };
    /// @brief 最近一次生成房主资源清单时使用的谱面观察指针，仅用于身份比较。
    const ::MMM::BeatMap* m_hostResourceBeatmap{ nullptr };
    /// @brief 当前固定会话是否由访客收到的房主快照创建。
    bool m_boundSessionIsGuest{ false };
    /// @brief 当前控制器是否已接管访客在线期间的本机项目打开门闩。
    bool m_guestProjectGateHeld{ false };
    /// @brief 最近一次应用到固定会话的谱面类别权限位。
    std::uint8_t m_lastAppliedPermissionFlags{ 0xFFU };
    /// @brief 上一次内嵌日志已经展示的条数，用于新日志自动滚动。
    std::size_t m_lastLogCount = 0;
};
}  // namespace MMM::UI
