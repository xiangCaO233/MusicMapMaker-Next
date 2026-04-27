#include "graphic/imguivk/mem/VKMemBuffer.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{

/**
 * @brief 构造函数，创建VK内存缓冲区
 *
 * @param vkPhysicalDevice 物理设备引用
 * @param vkLogicalDevice 逻辑设备引用
 * @param bufSize 要创建的缓冲区大小(字节)
 * @param bufUsage 缓冲区用法
 * @param desireProperty 期望的属性
 */
VKMemBuffer::VKMemBuffer(const vk::PhysicalDevice& vkPhysicalDevice,
                         vk::Device& vkLogicalDevice, const size_t bufSize,
                         const vk::BufferUsageFlags    bufUsage,
                         const vk::MemoryPropertyFlags desireProperty)
    : m_vkLogicalDevice(vkLogicalDevice), m_bufSize(bufSize)
{
    // 1.缓冲区创建信息
    vk::BufferCreateInfo bufferCreateInfo;
    bufferCreateInfo
        // 用传入信息创建
        .setSize(bufSize)
        .setUsage(bufUsage)
        // 无共享
        .setSharingMode(vk::SharingMode::eExclusive)
        // 若共享需要传入命令队列索引列表
        // .setQueueFamilyIndices(indices)
        ;

    // 2.创建缓冲区
    m_vkBuffer = vkLogicalDevice.createBuffer(bufferCreateInfo).value;

    XDEBUG("Created VK Memory Buffer.");

    // 3.查询内存需求
    const vk::MemoryRequirements bufferMemoryRequirements =
        vkLogicalDevice.getBufferMemoryRequirements(m_vkBuffer);
    // 实际分配的大小 (通常 >= bufSize，因为有对齐要求)
    m_memInfo.size = bufferMemoryRequirements.size;
    // 初始化为无效值
    m_memInfo.index = static_cast<uint32_t>(~0);

    // 3.1.查询物理设备内存属性
    vk::PhysicalDeviceMemoryProperties memoryProperties =
        vkPhysicalDevice.getMemoryProperties();
    for ( uint32_t i{ 0 }; i < memoryProperties.memoryTypeCount; ++i ) {
        // 条件1: bufferMemoryRequirements.memoryTypeBits
        // 的第 i 位为 1 (硬件支持)
        // 条件2: 该内存类型的属性包含所有期望的属性 (软件需求)
        if ( 1 << i & bufferMemoryRequirements.memoryTypeBits &&
             memoryProperties.memoryTypes[i].propertyFlags & desireProperty ) {
            m_memInfo.index = i;
            // 记录是否是 Host Coherent（如果是，就不需要手动 flush）
            m_isHostCoherent = (memoryProperties.memoryTypes[i].propertyFlags &
                                vk::MemoryPropertyFlagBits::eHostCoherent) ==
                               vk::MemoryPropertyFlagBits::eHostCoherent;
            break;
        }
    }

    if ( m_memInfo.index == static_cast<uint32_t>(~0) ) {
        XCRITICAL("Failed to find suitable memory type!");
    }

    // 4.内存分配信息
    vk::MemoryAllocateInfo memoryAllocateInfo;
    memoryAllocateInfo
        // 设置为上面查询到的索引
        .setMemoryTypeIndex(m_memInfo.index)
        // 设置为上面查询到的实际大小
        .setAllocationSize(m_memInfo.size);
    // 5.分配内存
    m_vkDevMem = vkLogicalDevice.allocateMemory(memoryAllocateInfo).value;
    XDEBUG("Allocated VK Device Memory.");

    // 6.绑定内存
    //  memoryOffset通常是 0（表示从这块内存的开头开始用）
    (void)vkLogicalDevice.bindBufferMemory(m_vkBuffer, m_vkDevMem, 0);
    XDEBUG("Binded Memory to VKBuffer.");

    // 4. 持久化映射 (Persistent Mapping)
    // 只要是 HOST_VISIBLE 的内存，我们在创建时就一直 Map 着，直到析构才 Unmap。
    // 这对于每帧都要更新的 Vertex/Index/Uniform 缓冲区性能提升极大。
    if ( desireProperty & vk::MemoryPropertyFlagBits::eHostVisible ) {
        m_mappedData =
            m_vkLogicalDevice.mapMemory(m_vkDevMem, 0, m_bufSize).value;
        XDEBUG("VKMemBuffer Host memory persistently mapped.");
    }
}

VKMemBuffer::~VKMemBuffer()
{
    // 如果映射过，析构时自动解除映射
    if ( m_mappedData ) {
        m_vkLogicalDevice.unmapMemory(m_vkDevMem);
        m_mappedData = nullptr;
    }

    // 销毁内存
    m_vkLogicalDevice.freeMemory(m_vkDevMem);
    XDEBUG("Freed VK Device Memory.");

    // 销毁缓冲区
    m_vkLogicalDevice.destroyBuffer(m_vkBuffer);
    XDEBUG("Destroyd VK Memory Buffer.");
}
/**
 * @brief 方式一：直接内存拷贝上传 (适用于 HOST_VISIBLE 类型)
 *
 * @note 这是最高效的每帧更新方式。如果内存不是 HOST_COHERENT，
 * 内部会自动调用 flushMappedMemoryRanges 同步 CPU 缓存到 GPU。
 *
 * @param data 数据指针
 * @param size 数据大小
 * @param offset 写入偏移量
 */
void VKMemBuffer::uploadData(const void* data, size_t size, size_t offset)
{
    if ( !m_mappedData ) {
        XCRITICAL("Trying to direct upload to a non-host-visible buffer!");
        return;
    }
    if ( offset + size > m_bufSize ) {
        XCRITICAL(
            "VKMemBuffer direct upload overflow! Size: {}, Offset: {}, Max: {}",
            size,
            offset,
            m_bufSize);
        return;
    }

    // 直接拷贝到持久化映射的内存指针中
    std::memcpy(static_cast<char*>(m_mappedData) + offset, data, size);

    // 如果内存类型不支持自动一致性 (Coherent)，必须手动 Flush，通知 GPU
    // 内存已修改
    if ( !m_isHostCoherent ) {
        vk::MappedMemoryRange mappedRange{};
        mappedRange.setMemory(m_vkDevMem)
            .setOffset(offset)
            .setSize(size);  // 也可以填 VK_WHOLE_SIZE
        vk::Result ret =
            m_vkLogicalDevice.flushMappedMemoryRanges(1, &mappedRange);
    }
}

/**
 * @brief 方式二：通过暂存缓冲区 (Staging Buffer) 上传 (适用于 DEVICE_LOCAL
 * 类型)
 *
 * @note 适用于只能由 GPU 访问的最佳性能内存。它会在内部创建一个临时的
 * HOST_VISIBLE 缓冲区，将数据拷贝进临时区，然后记录并提交一次 GPU
 * 拷贝指令，最后等待队列空闲。
 *
 * @warning 目标 Buffer 在创建时必须包含
 * vk::BufferUsageFlagBits::eTransferDst 用法！
 *
 * @param vkPhysicalDevice 物理设备
 * @param cmdPool 命令池 (用于分配临时命令缓冲)
 * @param queue 提交拷贝指令的队列 (通常是 Graphics 或 Transfer 队列)
 * @param data 数据指针
 * @param size 数据大小
 * @param offset 写入偏移量
 */
void VKMemBuffer::uploadDataStaged(const vk::PhysicalDevice& vkPhysicalDevice,
                                   vk::CommandPool cmdPool, vk::Queue queue,
                                   const void* data, size_t size, size_t offset)
{
    if ( offset + size > m_bufSize ) {
        XCRITICAL("VKMemBuffer staged upload overflow!");
        return;
    }

    // 1. 创建临时的 Staging Buffer (在 CPU 和 GPU 之间做中转)
    VKMemBuffer stagingBuffer(
        vkPhysicalDevice,
        m_vkLogicalDevice,
        size,
        vk::BufferUsageFlagBits::eTransferSrc,  // 重点：作为传输源
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent);

    // 2. 把数据用 memcpy 传给 Staging Buffer
    stagingBuffer.uploadData(data, size, 0);

    // 3. 分配一个一次性使用的 Command Buffer
    vk::CommandBufferAllocateInfo allocInfo;
    allocInfo.setLevel(vk::CommandBufferLevel::ePrimary)
        .setCommandPool(cmdPool)
        .setCommandBufferCount(1);
    vk::CommandBuffer cmdBuf =
        m_vkLogicalDevice.allocateCommandBuffers(allocInfo).value[0];

    // 4. 开始录制命令
    vk::CommandBufferBeginInfo beginInfo;
    beginInfo.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    (void)cmdBuf.begin(beginInfo);

    // 5. 录制拷贝指令
    vk::BufferCopy copyRegion;
    copyRegion.setSrcOffset(0).setDstOffset(offset).setSize(size);
    cmdBuf.copyBuffer(stagingBuffer.getBuffer(), m_vkBuffer, 1, &copyRegion);

    // 6. 结束录制并提交到队列
    (void)cmdBuf.end();

    vk::SubmitInfo submitInfo;
    submitInfo.setCommandBufferCount(1).setPCommandBuffers(&cmdBuf);
    vk::Result ret = queue.submit(1, &submitInfo, nullptr);

    // 7. 阻塞等待 GPU 把拷贝做完
    // (注意：对于最高性能的引擎，这里应该使用 Fence
    // 异步等待，但作为初始化阶段的数据上传，waitIdle 是标准的做法)
    (void)queue.waitIdle();

    // 8. 清理一次性 Command Buffer
    m_vkLogicalDevice.freeCommandBuffers(cmdPool, 1, &cmdBuf);
}

}  // namespace MMM::Graphic
