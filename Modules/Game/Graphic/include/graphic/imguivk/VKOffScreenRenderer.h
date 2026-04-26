#pragma once

#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "graphic/imguivk/VKRenderPipeline.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "graphic/imguivk/mem/VKMemBuffer.h"
#include "graphic/imguivk/mesh/VKBasicVertex.h"

#ifndef VULKAN_HPP_NO_EXCEPTIONS
#    define VULKAN_HPP_NO_EXCEPTIONS
#endif
#include "vulkan/vulkan.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <unordered_map>

namespace MMM::Graphic
{
class VKTexture;

class VKOffScreenRenderer
{
public:
    VKOffScreenRenderer();
    VKOffScreenRenderer(VKOffScreenRenderer&&)                 = delete;
    VKOffScreenRenderer(const VKOffScreenRenderer&)            = delete;
    VKOffScreenRenderer& operator=(VKOffScreenRenderer&&)      = delete;
    VKOffScreenRenderer& operator=(const VKOffScreenRenderer&) = delete;
    virtual ~VKOffScreenRenderer();

    /// @brief 获取imgui图像描述符集用于贴窗口
    vk::DescriptorSet getDescriptorSet() const { return m_imguiDescriptor; }

    /// @brief 获取发光层遮罩描述符集
    vk::DescriptorSet getGlowDescriptorSet() const
    {
        return m_imguiGlowDescriptor;
    }

    /// @brief 录制gpu指令
    void recordCmds(vk::CommandBuffer& cmdBuf, uint32_t frameIndex);

    /// @brief 重建帧缓冲
    void reCreateFrameBuffer(vk::PhysicalDevice& phyDevice,
                             vk::Device& logicalDevice, VKSwapchain& swapchain,
                             vk::CommandPool commandPool, vk::Queue queue,
                             const std::filesystem::path& shaderModulePath = {},
                             size_t maxVertexCount = 81920);

    /// @brief 外部确认是否需要重建
    inline bool needReCreateFrameBuffer() const
    {
        if ( !m_need_reCreate.load() ) return false;

        // 核心消抖判断：当前时间 - 最后请求时间 > 阈值
        auto now = std::chrono::steady_clock::now();
        if ( now - m_lastRequestTime > m_debounceThreshold ) {
            return true;
        }

        return false;
    }

protected:
    /// @brief 画布尺寸
    uint32_t m_width{ 0 };
    uint32_t m_height{ 0 };

    // UI 请求的目标尺寸
    uint32_t m_targetWidth{ 0 };
    uint32_t m_targetHeight{ 0 };

    // 时间点记录
    std::chrono::steady_clock::time_point m_lastRequestTime;

    // 消抖阈值 (150ms 是肉眼感知和性能的平衡点)
    const std::chrono::milliseconds m_debounceThreshold{ 150 };

    // UI 只设置目标，不改实际尺寸
    inline void setTargetSize(uint32_t logicalW, uint32_t logicalH,
                              float dpiScale)
    {
        uint32_t physicalW = static_cast<uint32_t>(logicalW * dpiScale);
        uint32_t physicalH = static_cast<uint32_t>(logicalH * dpiScale);

        if ( physicalW != m_targetWidth || physicalH != m_targetHeight ) {
            // 核心修复：向逻辑线程同步逻辑尺寸而非物理尺寸
            // 因为逻辑线程的渲染系统 (NoteRenderSystem) 基于逻辑坐标生成几何体
            resizeCall(m_logicalWidth, m_logicalHeight, logicalW, logicalH);
            m_targetWidth     = physicalW;
            m_targetHeight    = physicalH;
            m_logicalWidth    = logicalW;
            m_logicalHeight   = logicalH;
            m_lastRequestTime = std::chrono::steady_clock::now();
            // 标记为“有变更待处理”
            m_need_reCreate.store(true);
        }
    }

    // --- 改变尺寸后的回调 ---
    virtual void resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                            uint32_t h) const = 0;

    /// @brief 逻辑画布尺寸 (ImGui 空间)
    uint32_t m_logicalWidth{ 0 };
    uint32_t m_logicalHeight{ 0 };

    /// @brief 亚帧时间补偿 Y 轴偏移 (逻辑像素)
    /// 当逻辑线程生成快照的时间落后于渲染帧时，通过偏移正交投影矩阵
    /// 来补偿时间差对应的像素移动量，消除因采样混叠导致的周期性停顿
    float m_yOffset{ 0.0f };

    /// @brief 获取 DPI 缩放倍率
    inline float getDpiScale() const
    {
        if ( m_logicalWidth == 0 ) return 1.0f;
        return static_cast<float>(m_targetWidth) /
               static_cast<float>(m_logicalWidth);
    }

    /// @brief 将逻辑裁剪矩形转换为物理裁剪矩形 (Vulkan Scissor 使用物理坐标)
    inline vk::Rect2D getPhysicalScissor(const vk::Rect2D& logicalScissor) const
    {
        float      scale = getDpiScale();
        vk::Rect2D physical;
        physical.offset.x =
            static_cast<int32_t>(logicalScissor.offset.x * scale);
        physical.offset.y =
            static_cast<int32_t>(logicalScissor.offset.y * scale);
        physical.extent.width =
            static_cast<uint32_t>(logicalScissor.extent.width * scale);
        physical.extent.height =
            static_cast<uint32_t>(logicalScissor.extent.height * scale);
        return physical;
    }

    /// @brief 是否需要重建
    std::atomic<bool> m_need_reCreate{ true };

    /**
     * @brief 获取 Shader 源码接口 (在固定时刻需要创建)
     */
    virtual std::vector<std::string> getShaderSources(
        const std::string& shader_module_name) = 0;

    /**
     * @brief 获取 Shader 名称(需要按唯一名称名称储存和销毁)
     */
    virtual std::string getShaderName(
        const std::string& shader_module_name) = 0;

    // --- 获取数据供 Vulkan 使用 ---
    virtual const std::vector<Vertex::VKBasicVertex>& getVertices() const = 0;

    virtual const std::vector<uint32_t>& getIndices() const = 0;

    /**
     * @brief 录制具体的绘制指令 (抽象方法，由 UI 层实现)
     */
    virtual void onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                                  vk::PipelineLayout      pipelineLayout,
                                  vk::DescriptorSetLayout setLayout,
                                  vk::DescriptorSet       defaultDescriptor,
                                  uint32_t                frameIndex) = 0;

    virtual void onRecordGlowCmds(vk::CommandBuffer&      cmdBuf,
                                  vk::PipelineLayout      pipelineLayout,
                                  vk::DescriptorSetLayout setLayout,
                                  vk::DescriptorSet       defaultDescriptor,
                                  uint32_t                frameIndex)
    {
    }

private:
    // --- 1. 物理资源 (独占) ---
    vk::Image        m_image;        // 画布纹理
    vk::DeviceMemory m_imageMemory;  // 纹理内存
    vk::ImageView    m_imageView;    // 纹理视图
    vk::Framebuffer  m_framebuffer;  // 绑定到此纹理的帧缓冲
    vk::Sampler      m_sampler;      // 绑定到此纹理的采样器

    // --- 2. 几何资源 (多帧并发) ---
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    // 存 Brush 的顶点
    std::vector<std::unique_ptr<VKMemBuffer>> m_vertexBuffers;

    // 存 Brush 的索引
    std::vector<std::unique_ptr<VKMemBuffer>> m_indexBuffers;

    // 离屏用的 Uniform Buffer
    std::vector<std::unique_ptr<VKMemBuffer>> m_uniformBuffers;

    // 离屏用描述符池
    vk::DescriptorPool m_descriptorPool;

    // 离屏用描述符集 (每帧独立，因为它们指向不同的 Uniform Buffer)
    std::vector<vk::DescriptorSet> m_offScreenDescriptorSets;

    // 离屏用白色纹理
    std::unique_ptr<VKTexture> m_whiteTexture;

    // --- 离屏模糊发光相关 ---
    vk::Image m_glowImage{ VK_NULL_HANDLE }, m_pingImage{ VK_NULL_HANDLE },
        m_pongImage{ VK_NULL_HANDLE };
    vk::DeviceMemory m_glowImageMemory{ VK_NULL_HANDLE },
        m_pingImageMemory{ VK_NULL_HANDLE },
        m_pongImageMemory{ VK_NULL_HANDLE };
    vk::ImageView m_glowImageView{ VK_NULL_HANDLE },
        m_pingImageView{ VK_NULL_HANDLE }, m_pongImageView{ VK_NULL_HANDLE };
    vk::Framebuffer m_glowFramebuffer{ VK_NULL_HANDLE },
        m_pingFramebuffer{ VK_NULL_HANDLE },
        m_pongFramebuffer{ VK_NULL_HANDLE };
    vk::Sampler       m_glowSampler{ VK_NULL_HANDLE };
    std::vector<vk::DescriptorSet> m_pingDescriptorSets;
    std::vector<vk::DescriptorSet> m_pongDescriptorSets;
    std::vector<vk::DescriptorSet> m_glowDescriptorSets;

    std::unique_ptr<VKRenderPass>     m_glowRenderPass{ nullptr };
    std::unique_ptr<VKRenderPass>     m_blurRenderPass{ nullptr };
    std::unique_ptr<VKRenderPass>     m_compositeRenderPass{ nullptr };
    std::unique_ptr<VKRenderPipeline> m_glowBrushRenderPipeline{ nullptr };
    std::unique_ptr<VKRenderPipeline> m_blurRenderPipeline{ nullptr };
    std::unique_ptr<VKRenderPipeline> m_compositeRenderPipeline{ nullptr };

    void createOffscreenBuffer(
        vk::PhysicalDevice& phyDevice, vk::Device& logicalDevice,
        VKSwapchain& swapchain, vk::CommandPool commandPool, vk::Queue queue,
        uint32_t width, uint32_t height, vk::Image& image,
        vk::DeviceMemory& memory, vk::ImageView& imageView,
        vk::Framebuffer& framebuffer, vk::Sampler& sampler, VKRenderPass* pass);

    /// @brief 编译好的 Shader 模块映射表 (Name -> Shader)
    std::unordered_map<std::string, std::unique_ptr<VKShader>> m_vkShaders;

    /**
     * @brief 创建描述符池
     */
    void createDescriptPool();

    /**
     * @brief 创建描述符集列表
     */
    void createDescriptSets();

    /**
     * @brief 更新描述符集
     */
    void updateDescriptorSets();

    /**
     * @brief 上传uniform缓冲区到GPU
     */
    void uploadUniformBuffer2GPU();

    /**
     * @brief 创建所有着色器
     */
    void createShaderModules();

    /**
     * @brief 瞬发布局转换
     */
    void transitionImageInternal(vk::CommandPool pool, vk::Queue queue,
                                 vk::ImageLayout oldLayout,
                                 vk::ImageLayout newLayout,
                                 vk::Image       image = VK_NULL_HANDLE);


    // --- 3. UI 集成句柄 (独占) ---
    vk::DescriptorSet m_imguiDescriptor{ VK_NULL_HANDLE };  // ImGui 用的贴图 ID
    vk::DescriptorSet m_imguiGlowDescriptor{
        VK_NULL_HANDLE
    };  // ImGui 用的发光贴图 ID

    // --- 4. 引用/外部注入 (非所有权) ---
    // 逻辑设备引用
    vk::Device m_device{ VK_NULL_HANDLE };

    // 缓存分配时的最大顶点数，用于动态扩容判断
    size_t m_lastAllocatedCount{ 0 };

    // 物理设备句柄，用于动态扩容
    vk::PhysicalDevice m_physicalDevice{ VK_NULL_HANDLE };

    // 离屏渲染流程
    std::unique_ptr<VKRenderPass> m_offScreenRenderPass{ nullptr };

    // 画笔管线
    std::unique_ptr<VKRenderPipeline> m_mainBrushRenderPipeline{ nullptr };

    /// @brief 释放持有的资源
    void releaseResources();
};

}  // namespace MMM::Graphic
