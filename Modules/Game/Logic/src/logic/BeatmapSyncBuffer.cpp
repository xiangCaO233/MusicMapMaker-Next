#include "logic/BeatmapSyncBuffer.h"

namespace MMM::Logic
{

BeatmapSyncBuffer::BeatmapSyncBuffer()
{
    // 初始化指针分配
    m_working   = &m_buffers[0];
    m_completed = &m_buffers[1];
    m_reading   = &m_buffers[2];
}

RenderSnapshot* BeatmapSyncBuffer::getWorkingSnapshot()
{
    return m_working;
}

void BeatmapSyncBuffer::pushWorkingSnapshot()
{
    // 将写完的 working 指针与 completed 指针交换
    // std::memory_order_acq_rel 确保写操作在此交换前完成
    m_working = m_completed.exchange(m_working, std::memory_order_acq_rel);
    // 交换回来后，m_working 就指向了原本作为 completed
    // (可能是很早之前写好的或者是刚被UI退回来的) 缓冲区
    // 可以直接进行下一次清空和写入
}

RenderSnapshot* BeatmapSyncBuffer::pullLatestSnapshot()
{
    // 把当前 UI 用完的 reading 缓冲区交给 completed，换回最新的 completed
    // std::memory_order_acq_rel 确保交换是安全的
    RenderSnapshot* latest =
        m_completed.exchange(m_reading, std::memory_order_acq_rel);

    if ( latest != m_reading ) {
        // 确实拿到了一个新的快照，更新当前的 reading 指针
        m_reading = latest;
    }

    // 始终返回 m_reading (哪怕 exchange 没拿到新的，老的 m_reading
    // 里的内容依然可用)
    return m_reading;
}

}  // namespace MMM::Logic