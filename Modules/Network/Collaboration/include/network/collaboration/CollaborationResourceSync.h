#pragma once

#include "network/collaboration/CollaborationResourceTypes.h"

#include "network/collaboration/CollaborationProtocol.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace MMM
{
class BeatMap;
class Project;
}  // namespace MMM

namespace MMM::Network::Collaboration
{
/// @brief 资源后台状态机向房间网络循环发布的非阻塞事件。
struct CollaborationResourceSyncEvent {
    /// @brief 事件类别。
    enum class Type {
        ManifestReady,
        SendRequest,
        SendChunk,
        BundleReady,
        Error,
    };

    /// @brief 事件类别。
    Type type{ Type::Error };
    /// @brief 资源请求或分块对应的远端 Peer。
    PeerId peerId = 0;
    /// @brief 待发送的资源协议消息。
    CollaborationMessage message;
    /// @brief 访客完成校验后的会话资源。
    CollaborationResourceBundle bundle;
    /// @brief 日志附加说明。
    std::string detail;
};

/// @brief 在后台执行资源哈希、缓存比对、分块读写和完整性验证。
/// @par 调用边界
/// 前台通过快照和事件消费结果，准备和落盘由后台任务推进。
class CollaborationResourceSync
{
public:
    /// @brief 创建空闲资源同步器。
    CollaborationResourceSync();
    /// @brief 停止后台任务并释放文件状态。
    ~CollaborationResourceSync();

    CollaborationResourceSync(const CollaborationResourceSync&) = delete;
    CollaborationResourceSync& operator=(const CollaborationResourceSync&) =
        delete;

    /// @brief 准备房主当前谱面实际引用的项目文件。
    /// @param project 房主当前项目快照。
    /// @param beatmap 房间绑定谱面快照。
    void startHost(const ::MMM::Project& project,
                   const ::MMM::BeatMap& beatmap);

    /// @brief 让访客等待清单并使用指定内容缓存根目录。
    /// @param cacheRoot 协作资源缓存根目录。
    void startGuest(std::filesystem::path cacheRoot);

    /// @brief 清空当前任务和状态。
    void reset();

    /// @brief 排队处理房主资源清单。
    void receiveManifest(ResourceManifest manifest);
    /// @brief 排队读取访客请求的房主文件分块。
    void receiveRequest(PeerId peerId, ResourceRequest request);
    /// @brief 排队写入房主返回的访客文件分块。
    void receiveChunk(ResourceChunk chunk);

    /// @brief 非阻塞取出一个后台完成事件。
    [[nodiscard]] bool pollEvent(CollaborationResourceSyncEvent& event);

    /// @brief 获取线程安全的资源同步进度快照。
    [[nodiscard]] CollaborationResourceSyncProgress progress() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}  // namespace MMM::Network::Collaboration
