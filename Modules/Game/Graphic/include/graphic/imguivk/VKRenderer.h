#pragma once

#include "graphic/CursorManager.h"
#include "graphic/glfw/GLFWHeader.h"
#include "graphic/imguivk/VKRenderPipeline.h"
#include "graphic/imguivk/mem/VKMemBuffer.h"
#include "vulkan/vulkan.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace MMM::Graphic
{

class NativeWindow;
class IGraphicUserHook;

/**
 * @brief Vulkan 渲染器类
 *
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
     * @param renderPass 渲染流程引用
     * @param logicDeviceGraphicsQueue 图形队列引用
     * @param logicDevicePresentQueue 呈现队列引用
     */
    VKRenderer(vk::PhysicalDevice& vkPhysicalDevice, vk::Device& logicalDevice,
               VKSwapchain& swapchain, VKRenderPass& renderPass,
               vk::Queue& logicDeviceGraphicsQueue,
               vk::Queue& logicDevicePresentQueue);

    // 禁用拷贝和移动
    VKRenderer(VKRenderer&&)                 = delete;
    VKRenderer(const VKRenderer&)            = delete;
    VKRenderer& operator=(VKRenderer&&)      = delete;
    VKRenderer& operator=(const VKRenderer&) = delete;

    ~VKRenderer();

    // clang-format off
    /**
     * @brief 执行单帧渲染
     * 包含等待 Fence、获取图像、录制命令、提交队列、呈现图像等步骤。
     *
     * @param window 原生窗口
     * @param 图形用户接口钩子列表
     */
    /// @warning 热路径：主线程每帧执行；Fence/Acquire/Present 不可中断。
    /// 禁止文件系统访问、完整 ECS 遍历、完整排序和每帧堆分配。
    // clang-format on
    void render(NativeWindow& window, std::span<IGraphicUserHook*> uiManagers);

    void triggerRecreate(NativeWindow& window);

    /**
     * @brief 当交换链重建后调用，用于同步内部缓存的图像数量并重建相关同步对象
     */
    void onSwapchainChanged();
    /// @brief 设置软件光标烟雾寿命覆盖值
    inline void setCursorSmokeLifeOverride(float life)
    {
        m_cursorSmokeLifeOverride = life;
    }

    /**
     * @brief 获取描述符池
     */
    inline vk::DescriptorPool getDescriptorPool() const
    {
        return m_vkDescriptorPool;
    }

    /**
     * @brief 获取画笔纹理专用的共享描述符集布局
     */
    inline vk::DescriptorSetLayout getBrushTextureLayout() const
    {
        return m_brushTextureLayout;
    }

    /// @brief 重新加载渲染器内部持有的皮肤纹理。
    /// @warning 低频资源重载路径：皮肤热切换时调用，会等待设备空闲并替换
    /// 软件光标等渲染器自持有纹理，禁止放入每帧渲染路径。
    void reloadSkinTextures();

private:
    /// @brief 单个可并行录制的离屏任务。
    struct OffscreenRecordTask {
        /// @brief 拥有该任务的图形用户钩子，生命周期由渲染循环外部保证。
        IGraphicUserHook* hook{ nullptr };

        /// @brief 钩子内部的任务索引。
        uint32_t taskIndex{ 0 };
    };

    /// @brief 离屏录制任务独占的 Vulkan 命令资源槽。
    struct OffscreenRecordSlot {
        /// @brief 该任务槽独占的命令池，避免多线程同时访问同一个 command pool。
        vk::CommandPool commandPool{};

        /// @brief 按并发帧索引分配的主命令缓冲。
        std::vector<vk::CommandBuffer> commandBuffers;
    };

    /// @brief 清屏颜色
    static std::array<float, 4> s_clear_color;

    /// @brief 物理设备引用
    vk::PhysicalDevice& m_vkPhysicalDevice;

    /// @brief 逻辑设备引用
    vk::Device& m_vkLogicalDevice;

    /// @brief 渲染流程引用
    VKRenderPass& m_vkRenderPass;

    /// @brief 交换链引用
    VKSwapchain& m_vkSwapChain;

    /// @brief 逻辑设备图形队列引用
    vk::Queue& m_LogicDeviceGraphicsQueue;

    /// @brief 逻辑设备呈现队列引用
    vk::Queue& m_LogicDevicePresentQueue;

    // =========================================================================
    // 内存 - 显存相关资源
    // =========================================================================

    /// @brief 主机Uniform缓冲区封装 - 每帧都需要
    std::vector<std::unique_ptr<VKMemBuffer>> m_vkHostUniformMemBuffers;

    /// @brief GPUUniform缓冲区封装 - 每帧都需要
    std::vector<std::unique_ptr<VKMemBuffer>> m_vkGPUUniformMemBuffers;

    /// @brief 描述符池
    vk::DescriptorPool m_vkDescriptorPool;

    /// @brief 共享的画笔纹理布局 (Set 0: CombinedImageSampler)
    vk::DescriptorSetLayout m_brushTextureLayout;

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
     * @brief 渲染完成信号量列表 (大小为 MAX_FRAMES_IN_FLIGHT)
     * 用于同步 Submit 和 Present (Display 等待 Render Finished)
     * 注意：这里使用的是 并发帧 索引 (m_currentFrameIndex)
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

    // =========================================================================
    // 光标管理
    // =========================================================================
    std::unique_ptr<CursorManager> m_cursorManager{ nullptr };
    float                          m_cursorSmokeLifeOverride{ -1.0f };

    /// @brief 当前帧收集到的离屏录制任务列表，跨帧复用容量。
    std::vector<OffscreenRecordTask> m_offscreenRecordTasks;

    /// @brief 离屏录制任务槽，每个槽拥有独立 command pool。
    std::vector<OffscreenRecordSlot> m_offscreenRecordSlots;

    /// @brief 当前帧提交到图形队列的命令缓冲列表，跨帧复用容量。
    std::vector<vk::CommandBuffer> m_frameSubmitCommandBuffers;

    void initCursorManager(vk::PhysicalDevice& vkPhysicalDevice,
                           vk::Device&         logicalDevice);

    void releaseCursorManager();

    /// @brief 确保离屏录制任务槽数量足够。
    /// @param taskCount 当前帧需要的任务槽数量。
    /// @warning 渲染热路径低频分支：只有可渲染视图数量增加时才创建 Vulkan
    /// command pool。
    void ensureOffscreenRecordSlots(size_t taskCount);

    /// @brief 释放离屏命令录制任务槽资源。
    /// @warning 不可中断操作：只能在 GPU idle 后的渲染器销毁路径调用。
    void releaseOffscreenRecordResources();

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
     * @brief 创建描述符池
     */
    void createDescriptPool();

    friend class VKContext;
};

}  // namespace MMM::Graphic
