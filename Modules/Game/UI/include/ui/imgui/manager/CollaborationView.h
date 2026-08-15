#pragma once

#include "ui/ISubView.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace MMM::Network::Collaboration
{
class CollaborationRoom;
}

namespace MMM::Graphic
{
class VKTexture;
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

    /// @copydoc ISubView::needsTextureReload
    bool needsTextureReload() const override;

    /// @copydoc ISubView::reloadTextures
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& commandPool,
                        vk::Queue& queue) override;

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
    void drawActiveRoom();
    /// @brief 绘制当前联机会话的有界聊天记录与输入框。
    /// @warning UI 热路径：仅遍历房间层最多保留的 200 条内存消息；发送操作
    /// 只投递一条小型可靠消息，不执行文件系统访问或网络等待。
    void drawChatSection();
    /// @brief 在协作侧栏内绘制可收起的实时日志。
    /// @warning UI 热路径：仅在区域展开时委托常驻控制器遍历日志。
    void drawLogSection(UIManager* sourceManager) const;
    /// @brief 开始访客加入流程，必要时先请求关闭全部本机项目与谱面画布。
    void beginGuestJoin(std::string creator, std::string roomId,
                        std::string roomName, UIManager* sourceManager);
    /// @brief 推进等待本机关闭完成的访客加入流程。
    /// @warning UI 热路径低频分支：等待期间每帧只读取项目和 Session 快照；
    /// 完成后才启动一次网络连接。
    void advancePendingGuestJoin(UIManager* sourceManager);
    /// @brief 在后台构建指纹就绪后继续用户请求的开房流程。
    /// @warning UI 热路径低频分支：每帧只读取原子状态并移动至多一份小型配置，
    /// 同时复查当前项目与谱面快照；不读取文件或等待后台任务。
    void advancePendingHostStart();
    /// @brief 打开房卡封面图片选择器。
    /// @warning 用户低频入口：原生模式会同步打开系统文件选择器。
    void openRoomCoverFilePicker();
    /// @brief 绘制统一文件选择器并消费选择结果。
    /// @warning UI 热路径：仅在对话框打开期间绘制 ImGuiFileDialog。
    void renderRoomCoverFilePicker();
    /// @brief 读取图片并更新待发布房卡封面。
    /// @param path 用户选择或当前谱面引用的图片绝对路径。
    /// @param customized 是否由用户显式选择。
    /// @warning 用户低频路径：会同步读取并生成一张 320x180 JPEG 缩略图。
    void setRoomCoverPath(const std::filesystem::path& path, bool customized);
    /// @brief 将一份 Base64 封面排入下一次 GPU 资源准备。
    void queueRoomCoverTexture(std::string key, std::string_view base64);
    /// @brief 绘制固定比例封面或跟随当前 UI 主题的中性占位图。
    /// @param textureKey 纹理缓存键。
    /// @param size 绘制尺寸。
    /// @warning UI 热路径：仅提交一个 hit zone 与少量 ImDrawList 命令。
    void drawRoomCover(std::string_view textureKey, ImVec2 size);

    struct PendingHostStart;
    struct PendingGuestJoin;

    /// @brief 应用级协作房间。
    std::shared_ptr<Network::Collaboration::CollaborationRoom> m_room;
    /// @brief 公网目录展示的房间名称输入。
    std::array<char, 160> m_roomName{};
    /// @brief 是否已经从当前谱面初始化房间名称。
    bool m_roomNameInitialized = false;
    /// @brief 当前谱面默认封面解析到的绝对路径。
    std::filesystem::path m_defaultRoomCoverPath;
    /// @brief 当前准备发布的房卡封面源文件。
    std::filesystem::path m_roomCoverPath;
    /// @brief 当前准备发布的固定尺寸 Base64 JPEG 缩略图。
    std::string m_roomCoverImage;
    /// @brief 当前封面生成错误对应的翻译键；为空表示没有错误。
    std::string m_roomCoverErrorKey;
    /// @brief 当前封面是否由用户显式选择，而非跟随谱面默认值。
    bool m_roomCoverCustomized = false;
    /// @brief 上次初始化封面的谱面路径键，用于切谱后恢复默认封面。
    std::filesystem::path m_roomCoverBeatmapKey;
    /// @brief 等待 GPU 资源准备阶段解码上传的 Base64 图片。
    std::map<std::string, std::string, std::less<>> m_pendingRoomCoverTextures;
    /// @brief 已上传的房卡封面纹理。
    std::map<std::string, std::unique_ptr<Graphic::VKTexture>, std::less<>>
        m_roomCoverTextures;
    /// @brief 等待在 GPU 资源准备阶段移除的纹理键。
    std::set<std::string, std::less<>> m_roomCoverTextureRemovals;
    /// @brief 已确认无法解码的远端封面，避免每帧重复上传。
    std::set<std::string, std::less<>> m_failedRoomCoverTextures;
    /// @brief 开房时是否只允许主程序构建指纹与房主一致的访客。
    bool m_requireMatchingBuildFingerprint = true;
    /// @brief 当前客户端向 P2P 房间发布主画布状态的频率。
    int m_viewportPublishRateHz = 10;
    /// @brief 聊天输入缓冲区字节数，包含末尾空字符。
    static constexpr std::size_t CHAT_INPUT_BUFFER_BYTES = 1025U;
    /// @brief 单条协作聊天消息输入缓冲区。
    std::array<char, CHAT_INPUT_BUFFER_BYTES> m_chatInput{};
    /// @brief 上一次完成绘制的聊天记录序号，用于新消息自动滚动。
    std::uint64_t m_lastRenderedChatSequence{ 0 };
    /// @brief 上一次发送聊天消息是否被协议或传输拒绝。
    bool m_chatSendFailed{ false };
    /// @brief 等待后台构建指纹完成的房主开房配置。
    std::unique_ptr<PendingHostStart> m_pendingHostStart;
    /// @brief 等待全部本机编辑状态安全关闭后的访客加入配置。
    std::unique_ptr<PendingGuestJoin> m_pendingGuestJoin;
    /// @brief 上一次访客入房是否因本机关闭未完成或被取消而中止。
    bool m_guestJoinPreparationCancelled = false;
};
}  // namespace MMM::UI
