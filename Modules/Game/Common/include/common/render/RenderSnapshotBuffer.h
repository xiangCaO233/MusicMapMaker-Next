#pragma once

#include "common/render/RenderSnapshot.h"
#include <memory>
#include <vector>

namespace MMM::Common::Render
{

/// @brief 逻辑生产线程与画布消费线程之间的快照池。
class RenderSnapshotBuffer
{
public:
    /// @brief 构造预分配的渲染快照池。
    RenderSnapshotBuffer();
    /// @brief 销毁渲染快照池和私有队列。
    ~RenderSnapshotBuffer();

    RenderSnapshotBuffer(RenderSnapshotBuffer&&)                 = delete;
    RenderSnapshotBuffer(const RenderSnapshotBuffer&)            = delete;
    RenderSnapshotBuffer& operator=(RenderSnapshotBuffer&&)      = delete;
    RenderSnapshotBuffer& operator=(const RenderSnapshotBuffer&) = delete;

    /// @brief 逻辑线程获取可写工作快照。
    /// @return 当前工作快照。
    /// @warning 逻辑热路径：优先复用池对象，仅池耗尽且未达上限时分配。
    [[nodiscard]] RenderSnapshot* getWorkingSnapshot();

    /// @brief 逻辑线程提交当前工作快照。
    /// @warning 逻辑热路径：只执行无锁队列操作。
    void pushWorkingSnapshot();

    /// @brief UI 线程拉取并保留最新快照。
    /// @return 最新快照；无新快照时返回上一帧快照。
    /// @warning UI 热路径：只消费无锁队列并回收旧快照。
    [[nodiscard]] RenderSnapshot* pullLatestSnapshot();

    /// @brief UI 线程获取当前展示快照。
    /// @return 当前读取快照。
    [[nodiscard]] RenderSnapshot* getReadingSnapshot() const
    {
        return m_reading;
    }

    /// @brief 逻辑线程重置待读快照并清空当前读快照。
    void reset();

private:
    /// @brief 隐藏第三方无锁队列实现。
    struct QueueState;
    /// @brief 私有队列状态。
    std::unique_ptr<QueueState> m_queueState;
    /// @brief 仅在构造和池扩容时分配的快照所有权。
    std::vector<std::unique_ptr<RenderSnapshot>> m_storage;
    /// @brief 逻辑线程当前写入快照。
    RenderSnapshot* m_working{ nullptr };
    /// @brief UI 线程当前读取快照。
    RenderSnapshot* m_reading{ nullptr };
};

}  // namespace MMM::Common::Render
