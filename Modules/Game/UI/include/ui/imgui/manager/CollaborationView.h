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

    /// @copydoc ISubView::onUpdate
    /// @warning UI 热路径：子视图可见时每帧执行；只绘制内存状态，禁止加入
    /// 文件系统访问、网络等待或完整谱面遍历。
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

    /// @copydoc ISubView::getMinContentSize
    /// @warning UI 热路径：每帧可能查询多次，只允许常量尺寸计算。
    ImVec2 getMinContentSize(float dpiScale) const override;

private:
    /// @brief 离线页面当前选择的操作模式。
    enum class EntryMode {
        Host,
        Join,
    };

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
    /// @brief 请求显示独立协作日志窗口。
    void showLogWindow(UIManager* sourceManager) const;

    /// @brief 应用级协作房间。
    std::shared_ptr<Network::Collaboration::CollaborationRoom> m_room;
    /// @brief 离线入口模式。
    EntryMode m_entryMode = EntryMode::Host;
    /// @brief 房主或访客使用的信令端口输入。
    int m_port = 24864;
    /// @brief 访客输入的房主地址。
    std::array<char, 256> m_hostAddress{};
    /// @brief 访客输入的房间码。
    std::array<char, 16> m_roomCode{};
};
}  // namespace MMM::UI
