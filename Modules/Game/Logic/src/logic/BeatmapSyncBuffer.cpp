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
    m_storage.push_back(std::make_unique<RenderSnapshot>());
    m_reading = m_storage.back().get();
}

RenderSnapshot* BeatmapSyncBuffer::getWorkingSnapshot()
{
    // 关键修复：防止 m_working 被覆盖导致原指针丢失
    if ( m_working ) {
        return m_working;
    }

    // 尝试从空闲队列获取
    if ( !m_freeQueue.try_dequeue(m_working) ) {
        const size_t MAX_SNAPSHOTS = 64;  // 单个 Buffer 最大允许的快照数
        if ( m_storage.size() < MAX_SNAPSHOTS ) {
            m_storage.push_back(std::make_unique<RenderSnapshot>());
            m_working = m_storage.back().get();
        } else {
            // 已达上限，强行从就绪队列抢夺一个最旧的回来（虽然这会导致跳帧，但总比内存爆炸好）
            if ( !m_readyQueue.try_dequeue(m_working) ) {
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
        if ( m_readyQueue.size_approx() > MAX_READY ) {
            RenderSnapshot* stale = nullptr;
            if ( m_readyQueue.try_dequeue(stale) ) {
                m_freeQueue.enqueue(stale);
            }
        }

        m_readyQueue.enqueue(m_working);
        m_working = nullptr;
    }
}

RenderSnapshot* BeatmapSyncBuffer::pullLatestSnapshot()
{
    RenderSnapshot* latest = nullptr;

    // 关键修正：从队列中拉取所有可用的快照，只保留最新的一个，其余丢弃回空闲队列。
    // 这能防止逻辑线程跑得比 UI 线程快时产生的巨大延迟累积。
    while ( m_readyQueue.try_dequeue(latest) ) {
        if ( m_reading ) {
            // 如果已经找到了一个（旧的），将其归还到空闲队列
            m_freeQueue.enqueue(m_reading);
        }
        m_reading = latest;
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
