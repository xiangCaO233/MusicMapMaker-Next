#pragma once

#include "graphic/vk/mem/VKMemBuffer.h"
#include "graphic/vk/VKRenderPipeline.h"
#include "mem/VKUniforms.h"
#include "mesh/VKVertex.h"
#include "vulkan/vulkan.hpp"
#include <memory>
#include <vector>

namespace MMM::Graphic
{

/**
 * @brief Vulkan 渲染器类
 *
 * 负责管理渲染循环的核心逻辑，包括命令录制、同步控制（Semaphore/Fence）、
 * 队列提交（Submit）以及呈现（Present）。
 * 实现了基于多帧并发（Frames In Flight）的渲染架构。
 */
class VKRenderer final
{

public:
    /**
     * @brief 构造函数，初始化渲染所需的同步对象和命令资源
     *
     * @param vkPhysicalDevice 物理设备引用 (用于创建内存缓冲区)
     * @param logicalDevice 逻辑设备引用
     * @param swapchain 交换链引用
     * @param pipeline 渲染管线引用
     * @param renderPass 渲染流程引用
     * @param logicDeviceGraphicsQueue 图形队列引用
     * @param logicDevicePresentQueue 呈现队列引用
     */
    VKRenderer(vk::PhysicalDevice& vkPhysicalDevice, vk::Device& logicalDevice,
               VKSwapchain& swapchain, VKRenderPipeline& pipeline,
               VKRenderPass& renderPass, vk::Queue& logicDeviceGraphicsQueue,
               vk::Queue& logicDevicePresentQueue);

    // 禁用拷贝和移动
    VKRenderer(VKRenderer&&)                 = delete;
    VKRenderer(const VKRenderer&)            = delete;
    VKRenderer& operator=(VKRenderer&&)      = delete;
    VKRenderer& operator=(const VKRenderer&) = delete;

    ~VKRenderer();

    /**
     * @brief 执行单帧渲染
     *
     * 包含等待 Fence、获取图像、录制命令、提交队列、呈现图像等步骤。
     */
    void render();

private:
    /// @brief 顶点数据
    static std::array<VKVertex, 3> s_vertices;

    /// @brief 逻辑设备引用
    vk::Device& m_vkLogicalDevice;

    /// @brief 渲染流程引用
    VKRenderPass& m_vkRenderPass;

    /// @brief 交换链引用
    VKSwapchain& m_vkSwapChain;

    /// @brief 渲染管线引用
    VKRenderPipeline& m_vkRenderPipeline;

    /// @brief 逻辑设备图形队列引用
    vk::Queue& m_LogicDeviceGraphicsQueue;

    /// @brief 逻辑设备呈现队列引用
    vk::Queue& m_LogicDevicePresentQueue;

    // =========================================================================
    // 内存 - 显存相关资源
    // =========================================================================

    /// @brief 主机内存缓冲区封装
    std::unique_ptr<VKMemBuffer> m_vkHostMemBuffer;

    /// @brief GPU内存缓冲区封装
    std::unique_ptr<VKMemBuffer> m_vkGPUMemBuffer;

    /// @brief 主机Uniform缓冲区封装 - 每帧都需要
    std::vector<std::unique_ptr<VKMemBuffer>> m_vkHostUniformMemBuffers;

    /// @brief GPUUniform缓冲区封装 - 每帧都需要
    std::vector<std::unique_ptr<VKMemBuffer>> m_vkGPUUniformMemBuffers;

    /// @brief 描述符池
    vk::DescriptorPool m_vkDescriptorPool;

    /// @brief 描述符集列表 - 每帧都需要
    std::vector<vk::DescriptorSet> m_vkDescriptorSets;

    // =========================================================================
    // 命令相关资源
    // =========================================================================

    /// @brief Vulkan 命令池对象，用于分配 Command Buffer
    vk::CommandPool m_vkCommandPool;

    /// @brief 命令缓冲区列表 (大小为 MAX_FRAMES_IN_FLIGHT)
    std::vector<vk::CommandBuffer> m_vkCommandBuffers;

    // =========================================================================
    // 同步相关资源
    // =========================================================================

    /**
     * @brief 图像可用信号量列表 (大小为 MAX_FRAMES_IN_FLIGHT)
     * 用于同步 AcquireNextImage 和 Submit (GPU 等待 Image Ready)
     */
    std::vector<vk::Semaphore> m_imageAvailableSems;

    /**
     * @brief 渲染完成信号量列表 (大小为 Swapchain Image Count)
     * 用于同步 Submit 和 Present (Display 等待 Render Finished)
     * 注意：这里使用的是 Swapchain Image 索引
     */
    std::vector<vk::Semaphore> m_renderFinishedSems;

    /**
     * @brief CPU 等待栅栏列表 (大小为 MAX_FRAMES_IN_FLIGHT)
     * 用于同步 CPU 和 GPU，防止 CPU 覆写正在执行的 Command Buffer
     */
    std::vector<vk::Fence> m_cmdAvailableFences;

    // =========================================================================
    // 状态控制
    // =========================================================================

    /// @brief 可用图像缓冲数量 (Swapchain Image Count)
    size_t m_avalableImageBufferCount;

    /**
     * @brief 最大并发帧数 (Frames In Flight)
     * 决定了 CPU 可以领先 GPU多少帧进行录制。
     * 通常设置为 2 (双重缓冲) 或 3 (三重缓冲)。
     */
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    /// @brief 当前并发帧索引 (0 ~ MAX_FRAMES_IN_FLIGHT-1)
    size_t m_currentFrameIndex{ 0 };

    /// @brief 测试用当前时间的uniform变量
    VKTestTimeUniform m_testCurrentTime{ 0.f };

    /// @brief 用于上传uniform数据的指令缓冲区
    std::vector<vk::CommandBuffer> m_uniformUploadCmdBuffers;

private:
    /**
     * @brief 创建命令池
     */
    void createCommandPool();

    /**
     * @brief 分配命令缓冲区
     */
    void allocateCommandBuffers();

    /**
     * @brief 创建信号量和栅栏
     */
    void createSemsWithFences();

    /**
     * @brief 创建缓冲区
     *
     * @param vkPhysicalDevice 物理设备引用 (用于创建内存缓冲区)
     */
    void createMemBuffers(vk::PhysicalDevice& vkPhysicalDevice);

    /**
     * @brief 传输数据到GPU
     */
    void uploadBuffer2GPU(vk::CommandBuffer&            cmdBuffer,
                          const std::unique_ptr<VKMemBuffer>& hostBuffer,
                          const std::unique_ptr<VKMemBuffer>& gpuBuffer) const;

    /**
     * @brief 上传顶点缓冲区到GPU
     */
    void uploadVertexBuffer2GPU() const;

    /**
     * @brief 创建描述符池
     */
    void createDescriptPool();

    /**
     * @brief 创建描述符集列表
     */
    void createDescriptSets();

    /**
     * @brief 映射uniformbuffer到对应描述符集
     */
    void mapUniformBuffer2DescriptorSet() const;

    /**
     * @brief 上传uniform缓冲区到GPU
     */
    void uploadUniformBuffer2GPU(uint32_t current_image_index);
};

} // namespace MMM::Graphic


