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

RenderSnapshotBuffer::RenderSnapshotBuffer()
    : m_queueState(std::make_unique<QueueState>())
{
    for ( int index = 0; index < 10; ++index ) {
        m_storage.push_back(std::make_unique<RenderSnapshot>());
        m_queueState->freeQueue.enqueue(m_storage.back().get());
    }
    m_storage.push_back(std::make_unique<RenderSnapshot>());
    m_reading = m_storage.back().get();
}

RenderSnapshotBuffer::~RenderSnapshotBuffer() = default;

RenderSnapshot* RenderSnapshotBuffer::getWorkingSnapshot()
{
    if ( m_working ) return m_working;
    if ( !m_queueState->freeQueue.try_dequeue(m_working) ) {
        constexpr std::size_t MAX_SNAPSHOTS = 64U;
        if ( m_storage.size() < MAX_SNAPSHOTS ) {
            m_storage.push_back(std::make_unique<RenderSnapshot>());
            m_working = m_storage.back().get();
        } else if ( !m_queueState->readyQueue.try_dequeue(m_working) ) {
            m_working = m_storage.front().get();
        }
    }
    return m_working;
}

void RenderSnapshotBuffer::pushWorkingSnapshot()
{
    if ( !m_working ) return;
    constexpr std::size_t MAX_READY = 16U;
    if ( m_queueState->readyQueue.size_approx() > MAX_READY ) {
        RenderSnapshot* stale = nullptr;
        if ( m_queueState->readyQueue.try_dequeue(stale) ) {
            m_queueState->freeQueue.enqueue(stale);
        }
    }
    m_queueState->readyQueue.enqueue(m_working);
    m_working = nullptr;
}

RenderSnapshot* RenderSnapshotBuffer::pullLatestSnapshot()
{
    RenderSnapshot* latest = nullptr;
    while ( m_queueState->readyQueue.try_dequeue(latest) ) {
        if ( m_reading ) m_queueState->freeQueue.enqueue(m_reading);
        m_reading = latest;
    }
    return m_reading;
}

void RenderSnapshotBuffer::reset()
{
    RenderSnapshot* item = nullptr;
    while ( m_queueState->readyQueue.try_dequeue(item) ) {
        m_queueState->freeQueue.enqueue(item);
    }
    if ( m_reading ) m_reading->clear();
}

}  // namespace MMM::Common::Render
