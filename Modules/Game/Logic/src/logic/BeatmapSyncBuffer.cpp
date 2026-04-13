#include "logic/BeatmapSyncBuffer.h"

namespace MMM::Logic
{

BeatmapSyncBuffer::BeatmapSyncBuffer()
{
    // 初始化池：分配足够多的缓冲应对高速生产
    for ( int i = 0; i < 10; ++i ) {
        m_storage.push_back(std::make_unique<RenderSnapshot>());
        m_freeQueue.enqueue(m_storage.back().get());
    }

    // 初始化一个安全的读取帧防止刚开始时 nullptr 崩溃
    m_reading = new RenderSnapshot();
    m_storage.push_back(std::unique_ptr<RenderSnapshot>(m_reading));
}

RenderSnapshot* BeatmapSyncBuffer::getWorkingSnapshot()
{
    // 如果没有空闲的缓冲，只能新建一个以保证1对1帧率不断裂
    if ( !m_freeQueue.try_dequeue(m_working) ) {
        m_working = new RenderSnapshot();
        m_storage.push_back(std::unique_ptr<RenderSnapshot>(m_working));
    }
    return m_working;
}

void BeatmapSyncBuffer::pushWorkingSnapshot()
{
    if ( m_working ) {
        m_readyQueue.enqueue(m_working);
        m_working = nullptr;
    }
}

RenderSnapshot* BeatmapSyncBuffer::pullLatestSnapshot()
{
    RenderSnapshot* latest = nullptr;
    bool            found  = false;

    // 关键修正：从队列中拉取所有可用的快照，只保留最新的一个，其余丢弃回空闲队列。
    // 这能防止逻辑线程跑得比 UI 线程快时产生的巨大延迟累积。
    while ( m_readyQueue.try_dequeue(latest) ) {
        if ( found && m_reading ) {
            // 如果已经找到了一个（旧的），将其归还到空闲队列
            m_freeQueue.enqueue(m_reading);
        }
        m_reading = latest;
        found     = true;
    }

    // 如果队列为空，则继续复用上一帧的数据 (m_reading)
    return m_reading;
}

void BeatmapSyncBuffer::reset()
{
    RenderSnapshot* item = nullptr;
    while ( m_readyQueue.try_dequeue(item) ) {
        m_freeQueue.enqueue(item);
    }
    if ( m_reading ) {
        m_reading->clear();
    }
}

}  // namespace MMM::Logic