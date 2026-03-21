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
    VKMemBuffer(const vk::PhysicalDevice& vkPhysicalDevice,
                vk::Device& vkLogicalDevice, size_t bufSize,
                vk::BufferUsageFlags    bufUsage,
                vk::MemoryPropertyFlags desireProperty);

    // 禁用拷贝和移动
    VKMemBuffer(VKMemBuffer&&)                 = delete;
    VKMemBuffer(const VKMemBuffer&)            = delete;
    VKMemBuffer& operator=(VKMemBuffer&&)      = delete;
    VKMemBuffer& operator=(const VKMemBuffer&) = delete;

    ~VKMemBuffer();

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
    void uploadData(const void* data, size_t size, size_t offset = 0);

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
    void uploadDataStaged(const vk::PhysicalDevice& vkPhysicalDevice,
                          vk::CommandPool cmdPool, vk::Queue queue,
                          const void* data, size_t size, size_t offset = 0);

    // --- Getters ---
    inline vk::Buffer getBuffer() const { return m_vkBuffer; }
    inline size_t     getSize() const { return m_bufSize; }
    inline void*      getMappedData() const { return m_mappedData; }

private:
    /// @brief 内部辅助结构，存储分配信息
    struct MemInfo final {
        size_t   size;   ///< 实际分配的内存大小（包含对齐补齐）
        uint32_t index;  ///< 选定的内存类型索引
    };

    /// @brief 内存分配信息
    MemInfo m_memInfo{};

    /// @brief 逻辑设备引用
    vk::Device& m_vkLogicalDevice;

    /// @brief Vulkan 缓冲区句柄
    vk::Buffer m_vkBuffer;

    /// @brief 分配的设备内存句柄
    vk::DeviceMemory m_vkDevMem;

    /// @brief 缓冲区大小 (字节)
    size_t m_bufSize;

    // --- 新增：持久化映射相关 ---
    void* m_mappedData{ nullptr };    ///< 持久化映射的 CPU 端指针
    bool  m_isHostCoherent{ false };  ///< 记录是否具有自动缓存一致性

    // 允许 Renderer 直接访问内部的 Buffer
    friend class VKRenderer;
    friend class VKOffScreenRenderer;
};

}  // namespace Graphic

}  // namespace MMM
