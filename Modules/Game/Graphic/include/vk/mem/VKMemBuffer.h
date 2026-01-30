#pragma once

#include "vulkan/vulkan.hpp"

namespace MMM
{
namespace Graphic
{

/**
 * @brief Vulkan 内存缓冲区封装类
 *
 * 管理 vk::Buffer 和对应的 vk::DeviceMemory 的生命周期。
 * 负责缓冲区的创建、内存分配和绑定。
 */
class VKMemBuffer final
{
public:
    /**
     * @brief 构造函数，创建并绑定 VK 内存缓冲区
     *
     * 该构造函数会完成以下步骤：
     * 1. 创建 vk::Buffer 对象
     * 2. 查询该 Buffer 的内存需求
     * 3. 在物理设备中查找符合需求且具备指定属性（如 HostVisible）的内存类型
     * 4. 分配 vk::DeviceMemory
     * 5. 将内存绑定到缓冲区
     *
     * @param vkPhysicalDevice 物理设备引用 (用于查询内存类型属性)
     * @param vkLogicalDevice 逻辑设备引用 (用于创建资源)
     * @param bufSize 要创建的缓冲区大小(字节)
     * @param bufUsage 缓冲区用途
     * (如 VertexBuffer, IndexBuffer, UniformBuffer 等)
     * @param desireProperty 期望的内存属性
     * (如 HostVisible | HostCoherent 用于 CPU 写入)
     */
    VKMemBuffer(vk::PhysicalDevice& vkPhysicalDevice,
                vk::Device& vkLogicalDevice, size_t bufSize,
                vk::BufferUsageFlags    bufUsage,
                vk::MemoryPropertyFlags desireProperty);

    // 禁用拷贝和移动
    VKMemBuffer(VKMemBuffer&&)                 = delete;
    VKMemBuffer(const VKMemBuffer&)            = delete;
    VKMemBuffer& operator=(VKMemBuffer&&)      = delete;
    VKMemBuffer& operator=(const VKMemBuffer&) = delete;

    ~VKMemBuffer();

private:
    /// @brief 内部辅助结构，存储分配信息
    struct MemInfo final {
        size_t   size;   ///< 实际分配的内存大小（包含对齐补齐）
        uint32_t index;  ///< 选定的内存类型索引
    };

    /// @brief 内存分配信息
    MemInfo m_memInfo;

    /// @brief 逻辑设备引用
    vk::Device& m_vkLogicalDevice;

    /// @brief Vulkan 缓冲区句柄
    vk::Buffer m_vkBuffer;

    /// @brief 分配的设备内存句柄
    vk::DeviceMemory m_vkDevMem;

    /// @brief 缓冲区大小 (字节)
    size_t m_bufSize;

    // 允许 Renderer 直接访问内部的 Buffer
    friend class VKRenderer;
};

}  // namespace Graphic

}  // namespace MMM
