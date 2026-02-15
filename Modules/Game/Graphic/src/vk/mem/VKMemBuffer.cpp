#include "graphic/vk/mem/VKMemBuffer.h"
#include "log/colorful-log.h"

namespace MMM
{
namespace Graphic
{

/**
 * @brief 构造函数，创建VK内存缓冲区
 *
 * @param vkLogicalDevice 逻辑设备引用
 * @param bufSize 要创建的缓冲区大小(字节)
 * @param bufUsage 缓冲区用法
 */
VKMemBuffer::VKMemBuffer(vk::PhysicalDevice& vkPhysicalDevice,
                         vk::Device& vkLogicalDevice, size_t bufSize,
                         vk::BufferUsageFlags    bufUsage,
                         vk::MemoryPropertyFlags desireProperty)
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
    m_vkBuffer = vkLogicalDevice.createBuffer(bufferCreateInfo);

    XINFO("Created VK Memory Buffer.");

    // 3.查询内存需求
    vk::MemoryRequirements bufferMemoryRequirements =
        vkLogicalDevice.getBufferMemoryRequirements(m_vkBuffer);
    // 实际分配的大小 (通常 >= bufSize，因为有对齐要求)
    m_memInfo.size = bufferMemoryRequirements.size;
    // 初始化为无效值
    m_memInfo.index = uint32_t(~0);

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
            break;
        }
    }

    if ( m_memInfo.index == uint32_t(~0) ) {
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
    m_vkDevMem = vkLogicalDevice.allocateMemory(memoryAllocateInfo);
    XINFO("Allocated VK Device Memory.");

    // 6.绑定内存
    //  memoryOffset通常是 0（表示从这块内存的开头开始用）
    vkLogicalDevice.bindBufferMemory(m_vkBuffer, m_vkDevMem, 0);
    XINFO("Binded Memory to VKBuffer.");
}

VKMemBuffer::~VKMemBuffer()
{
    // 销毁内存
    m_vkLogicalDevice.freeMemory(m_vkDevMem);
    XINFO("Freed VK Device Memory.");

    // 销毁缓冲区
    m_vkLogicalDevice.destroyBuffer(m_vkBuffer);
    XINFO("Destroyd VK Memory Buffer.");
}

}  // namespace Graphic

}  // namespace MMM
