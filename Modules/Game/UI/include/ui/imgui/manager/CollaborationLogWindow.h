#pragma once

#include "ui/IUIView.h"

#include <cstddef>
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
/// @brief 持续驱动协作房间并实时展示所有客户端动态的独立窗口。
class CollaborationLogWindow final : public IUIView
{
public:
    /// @brief 创建应用级协作日志窗口。
    /// @param name 稳定视图名。
    /// @param room 应用级协作房间。
    CollaborationLogWindow(
        const std::string&                                         name,
        std::shared_ptr<Network::Collaboration::CollaborationRoom> room);
    /// @brief 解除会话观察者和网络回灌回调。
    ~CollaborationLogWindow() override;

    /// @brief 每帧驱动协作房间，并在可见时绘制日志。
    /// @warning UI 热路径：每帧执行一次有界网络队列轮询；只在窗口可见时
    /// 遍历最多 1000 条日志，不执行文件系统访问或阻塞等待。
    void update(UIManager* sourceManager) override;

    /// @brief 该常驻控制视图始终保留在 UIManager 中。
    [[nodiscard]] bool isOpen() const override;
    /// @brief 设置日志窗口可见性，不销毁后台协作控制器。
    void setOpen(bool open) override;
    /// @brief 显示协作日志窗口。
    void show();

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
    /// @brief 独立日志窗口当前是否可见。
    bool m_windowVisible = false;
    /// @brief 上一帧房间是否处于活动状态。
    bool m_wasRoomActive = false;
    /// @brief 上一帧已经展示的日志条数，用于新日志自动滚动。
    std::size_t m_lastLogCount = 0;
};
}  // namespace MMM::UI
