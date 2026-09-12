#include "common/render/RenderSnapshotBuffer.h"
#include <concurrentqueue.h>

namespace MMM::Common::Render
{

/// @brief 同步缓冲区私有无锁队列。
struct RenderSnapshotBuffer::QueueState {
    /// @brief 可供逻辑线程重新写入的快照队列。
    moodycamel::ConcurrentQueue<RenderSnapshot*> freeQueue;
    /// @brief 等待 UI 线程消费的已完成快照队列。
    moodycamel::ConcurrentQueue<RenderSnapshot*> readyQueue;
};

/// @brief 预分配逻辑生产端和 UI 消费端之间复用的快照对象。
/// @warning 初始化会分配队列与快照，仅允许在画布创建的低频路径调用。
RenderSnapshotBuffer::RenderSnapshotBuffer()
    : m_queueState(std::make_unique<QueueState>())
{
    // 初始池提供十个可写对象，覆盖常规生产者领先消费者的短暂波动。
    for ( int index = 0; index < 10; ++index ) {
        // 所有权永久保留在 storage，跨线程队列只传递稳定观察指针。
        m_storage.push_back(std::make_unique<RenderSnapshot>());
        m_queueState->freeQueue.enqueue(m_storage.back().get());
    }
    // 单独保留初始读取对象，保证首帧在尚无生产结果时仍返回有效快照。
    m_storage.push_back(std::make_unique<RenderSnapshot>());
    m_reading = m_storage.back().get();
}

/// @brief 在队列实现完整可见的位置销毁全部快照与队列状态。
RenderSnapshotBuffer::~RenderSnapshotBuffer() = default;

/// @brief 获取逻辑生产线程当前可写的快照。
/// @return 可重复写入直至 pushWorkingSnapshot 的稳定指针。
/// @warning 逻辑更新热路径；通常只出队复用，池扩容时才允许分配。
RenderSnapshot* RenderSnapshotBuffer::getWorkingSnapshot()
{
    // 同一生产周期重复请求时返回现有对象，防止丢失尚未提交的数据。
    if ( m_working ) return m_working;
    // 优先复用 UI 已归还的空闲对象，避免每帧分配快照及其内部容器。
    if ( !m_queueState->freeQueue.try_dequeue(m_working) ) {
        // 硬上限约束生产者长期领先时的内存增长。
        constexpr std::size_t MAX_SNAPSHOTS = 64U;
        if ( m_storage.size() < MAX_SNAPSHOTS ) {
            // 只有空闲池耗尽且仍低于上限时才扩充稳定所有权存储。
            m_storage.push_back(std::make_unique<RenderSnapshot>());
            m_working = m_storage.back().get();
        } else if ( !m_queueState->readyQueue.try_dequeue(m_working) ) {
            // 所有队列都为空的防御分支复用首个对象，避免返回空指针。
            m_working = m_storage.front().get();
        }
    }
    return m_working;
}

/// @brief 将逻辑线程完成的工作快照发布给 UI 消费线程。
/// @warning 逻辑更新热路径；只执行无锁队列操作，不等待消费者。
void RenderSnapshotBuffer::pushWorkingSnapshot()
{
    // 没有取得工作对象时提交为空操作，避免向队列写入空指针。
    if ( !m_working ) return;
    // 仅保留有限数量的待显示帧，UI 落后时优先丢弃旧快照降低延迟。
    constexpr std::size_t MAX_READY = 16U;
    if ( m_queueState->readyQueue.size_approx() > MAX_READY ) {
        RenderSnapshot* stale = nullptr;
        if ( m_queueState->readyQueue.try_dequeue(stale) ) {
            // 被淘汰的旧帧立即回到空闲池，供生产线程下一周期复用。
            m_queueState->freeQueue.enqueue(stale);
        }
    }
    // 发布后清空工作指针，下一次写入必须重新取得独占快照。
    m_queueState->readyQueue.enqueue(m_working);
    m_working = nullptr;
}

/// @brief 消费全部已发布快照并保留时间上最新的一份。
/// @return 最新快照；没有新帧时继续返回上一份读取快照。
/// @warning UI 每帧热路径；循环只处理当前积压，不等待生产线程。
RenderSnapshot* RenderSnapshotBuffer::pullLatestSnapshot()
{
    RenderSnapshot* latest = nullptr;
    // 清空当前积压以直接追上生产端，避免逐帧展示已经过时的状态。
    while ( m_queueState->readyQueue.try_dequeue(latest) ) {
        // 替换前把旧读取对象归还空闲池；当前 latest 尚未对外暴露。
        if ( m_reading ) m_queueState->freeQueue.enqueue(m_reading);
        m_reading = latest;
    }
    return m_reading;
}

/// @brief 回收所有待消费快照并清空当前展示内容。
/// @warning 项目切换低频路径；调用方必须保证不会与 UI 读取同一快照并发执行。
void RenderSnapshotBuffer::reset()
{
    RenderSnapshot* item = nullptr;
    // 待消费帧全部回收到空闲池，切换后不会显示旧项目内容。
    while ( m_queueState->readyQueue.try_dequeue(item) ) {
        m_queueState->freeQueue.enqueue(item);
    }
    // 保留读取对象本身以继续复用，仅清除其中的业务数据和批次容器。
    if ( m_reading ) m_reading->clear();
}

}  // namespace MMM::Common::Render
