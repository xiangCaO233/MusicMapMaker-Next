#pragma once

#include "ui/ISubView.h"

#include <array>
#include <memory>
#include <string>

namespace MMM::Network::Collaboration
{
class CollaborationRoom;
}

namespace MMM::UI
{
/// @brief 左侧栏中的协作房间创建、加入和成员管理视图。
class CollaborationView final : public ISubView
{
public:
    /// @brief 创建绑定到应用级协作房间的侧栏视图。
    /// @param subViewName 子视图标题。
    /// @param room 应用级协作房间。
    CollaborationView(
        const std::string&                                         subViewName,
        std::shared_ptr<Network::Collaboration::CollaborationRoom> room);

    /// @brief 在待加入配置类型完整的实现单元中销毁视图状态。
    ~CollaborationView() override;

    /// @copydoc ISubView::onUpdate
    /// @warning UI 热路径：子视图可见时每帧执行；只绘制内存状态，禁止加入
    /// 文件系统访问、网络等待或完整谱面遍历。
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

    /// @copydoc ISubView::getMinContentSize
    /// @warning UI 热路径：每帧可能查询多次，只允许常量尺寸计算。
    ImVec2 getMinContentSize(float dpiScale) const override;

private:
    /// @brief 绘制 Creator 身份和前置条件。
    /// @return Creator 有效时返回 true。
    /// @warning UI 热路径：只读取内存配置并绘制控件。
    bool drawIdentitySection(UIManager* sourceManager);
    /// @brief 绘制离线的开房/加入流程。
    /// @warning UI 热路径：仅在用户点击时启动网络连接。
    void drawOfflineFlow(UIManager* sourceManager, bool creatorValid);
    /// @brief 绘制当前房间状态、连接信息和成员列表。
    /// @warning UI 热路径：最多遍历 8 个内存成员记录。
    void drawActiveRoom(UIManager* sourceManager);
    /// @brief 将地址、信令端口和 TLS 控件应用到目录客户端。
    /// @return 服务器配置有效并成功应用时返回 true。
    bool applyServerEndpoint();
    /// @brief 请求显示独立协作日志窗口。
    void showLogWindow(UIManager* sourceManager) const;
    /// @brief 开始访客加入流程，必要时先请求关闭全部本机项目与谱面画布。
    void beginGuestJoin(std::string creator, std::string roomId,
                        std::string roomName, UIManager* sourceManager);
    /// @brief 推进等待本机关闭完成的访客加入流程。
    /// @warning UI 热路径低频分支：等待期间每帧只读取项目和 Session 快照；
    /// 完成后才启动一次网络连接。
    void advancePendingGuestJoin(UIManager* sourceManager);

    struct PendingGuestJoin;

    /// @brief 应用级协作房间。
    std::shared_ptr<Network::Collaboration::CollaborationRoom> m_room;
    /// @brief 中心服务器地址或域名输入。
    std::array<char, 256> m_serverAddress{};
    /// @brief 中心服务器信令端口输入。
    int m_signalingPort = 443;
    /// @brief 是否使用 TLS/WSS。
    bool m_useTls = true;
    /// @brief 公网目录展示的房间名称输入。
    std::array<char, 160> m_roomName{};
    /// @brief 是否已经从当前谱面初始化房间名称。
    bool m_roomNameInitialized = false;
    /// @brief 当前客户端向 P2P 房间发布主画布状态的频率。
    int m_viewportPublishRateHz = 10;
    /// @brief 等待全部本机编辑状态安全关闭后的访客加入配置。
    std::unique_ptr<PendingGuestJoin> m_pendingGuestJoin;
    /// @brief 上一次访客入房是否因本机关闭未完成或被取消而中止。
    bool m_guestJoinPreparationCancelled = false;
};
}  // namespace MMM::UI
