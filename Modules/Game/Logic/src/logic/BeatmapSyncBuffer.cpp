#include "logic/BeatmapSyncBuffer.h"

#include <concurrentqueue.h>

namespace MMM::Logic
{

/// @brief 同步缓冲区私有无锁队列，隔离第三方队列类型的头文件依赖。
struct BeatmapSyncBuffer::QueueState {
    /// @brief 可供逻辑线程重新写入的快照队列。
    moodycamel::ConcurrentQueue<RenderSnapshot*> freeQueue;

    /// @brief 等待 UI 线程消费的已完成快照队列。
    moodycamel::ConcurrentQueue<RenderSnapshot*> readyQueue;
};

BeatmapSyncBuffer::BeatmapSyncBuffer()
    : m_queueState(std::make_unique<QueueState>())
{
    // 初始化池：分配足够多的缓冲应对高速生产
    for ( int i = 0; i < 10; ++i ) {
        m_storage.push_back(std::make_unique<RenderSnapshot>());
        m_queueState->freeQueue.enqueue(m_storage.back().get());
    }

    // 初始化一个安全的读取帧防止刚开始时 nullptr 崩溃
    m_storage.push_back(std::make_unique<RenderSnapshot>());
    m_reading = m_storage.back().get();
}

BeatmapSyncBuffer::~BeatmapSyncBuffer() = default;

RenderSnapshot* BeatmapSyncBuffer::getWorkingSnapshot()
{
    // 关键修复：防止 m_working 被覆盖导致原指针丢失
    if ( m_working ) {
        return m_working;
    }

    // 尝试从空闲队列获取
    if ( !m_queueState->freeQueue.try_dequeue(m_working) ) {
        const size_t MAX_SNAPSHOTS = 64;  // 单个 Buffer 最大允许的快照数
        if ( m_storage.size() < MAX_SNAPSHOTS ) {
            m_storage.push_back(std::make_unique<RenderSnapshot>());
            m_working = m_storage.back().get();
        } else {
            // 已达上限，强行从就绪队列抢夺一个最旧的回来（虽然这会导致跳帧，但总比内存爆炸好）
            if ( !m_queueState->readyQueue.try_dequeue(m_working) ) {
                // 如果就绪队列也空（极罕见），则只能回退到第一个
                m_working = m_storage[0].get();
            }
        }
    }
    return m_working;
}

void BeatmapSyncBuffer::pushWorkingSnapshot()
{
    if ( m_working ) {
        // 积压保护：如果就绪队列太长（说明 UI
        // 线程卡住或没在读），丢弃最旧的快照以防内存膨胀
        const size_t MAX_READY = 16;
        if ( m_queueState->readyQueue.size_approx() > MAX_READY ) {
            RenderSnapshot* stale = nullptr;
            if ( m_queueState->readyQueue.try_dequeue(stale) ) {
                m_queueState->freeQueue.enqueue(stale);
            }
        }

        m_queueState->readyQueue.enqueue(m_working);
        m_working = nullptr;
    }
}

RenderSnapshot* BeatmapSyncBuffer::pullLatestSnapshot()
{
    RenderSnapshot* latest = nullptr;

    // 关键修正：从队列中拉取所有可用的快照，只保留最新的一个，其余丢弃回空闲队列。
    // 这能防止逻辑线程跑得比 UI 线程快时产生的巨大延迟累积。
    while ( m_queueState->readyQueue.try_dequeue(latest) ) {
        if ( m_reading ) {
            // 如果已经找到了一个（旧的），将其归还到空闲队列
            m_queueState->freeQueue.enqueue(m_reading);
        }
        m_reading = latest;
    }

    // 如果队列为空，则继续复用上一帧的数据 (m_reading)
    return m_reading;
}

void BeatmapSyncBuffer::reset()
{
    RenderSnapshot* item = nullptr;
    while ( m_queueState->readyQueue.try_dequeue(item) ) {
        m_queueState->freeQueue.enqueue(item);
    }
    if ( m_reading ) {
        m_reading->clear();
    }
}

}  // namespace MMM::Logic
