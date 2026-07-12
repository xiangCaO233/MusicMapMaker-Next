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
    /// @warning 热路径：每个可渲染 UI
    /// 视图在每帧命令录制阶段执行；禁止文件系统访问、完整排序、try/catch
    /// 和共享指针所有权复制。
    void recordCmds(vk::CommandBuffer& cmdBuf, uint32_t frameIndex);

    /// @brief 重建帧缓冲
    void reCreateFrameBuffer(vk::PhysicalDevice& phyDevice,
                             vk::Device& logicalDevice, VKSwapchain& swapchain,
                             vk::CommandPool commandPool, vk::Queue queue,
                             const std::filesystem::path& shaderModulePath = {},
                             size_t maxVertexCount = 81920);

    /// @brief 请求皮肤资源变更后的 shader 和离屏帧缓冲重建。
    /// @warning 低频资源重载路径：皮肤热切换时由 UIManager 调用，只置脏位并
    /// 清派生类 shader 缓存，禁止在每帧交互路径中调用。
    void requestSkinResourceReload()
    {
        invalidateShaderSourceCache();
        m_lastRequestTime =
            std::chrono::steady_clock::now() - m_debounceThreshold;
        m_need_reCreate.store(true, std::memory_order_relaxed);
    }

    /// @brief 外部确认是否需要重建
    /// @warning 热路径/原子：渲染准备阶段每帧轮询；resize
    /// 回调可能写入，只读取脏位和消抖时间，不承载资源同步。
    inline bool needReCreateFrameBuffer() const
    {
        if ( !m_need_reCreate.load(std::memory_order_relaxed) ) return false;

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

    /// @brief 发光后处理缓冲的物理宽度。
    uint32_t m_glowWidth{ 0 };

    /// @brief 发光后处理缓冲的物理高度。
    uint32_t m_glowHeight{ 0 };

    // UI 请求的目标尺寸
    uint32_t m_targetWidth{ 0 };
    uint32_t m_targetHeight{ 0 };

    // 时间点记录
    std::chrono::steady_clock::time_point m_lastRequestTime;

    // 消抖阈值 (150ms 是肉眼感知和性能的平衡点)
    const std::chrono::milliseconds m_debounceThreshold{ 150 };

    // UI 只设置目标，不改实际尺寸
    /// @brief UI 只设置目标，不改实际尺寸
    /// @warning 热路径/原子：ImGui update
    /// 期间可能每帧调用；只有尺寸变化时才写入重建脏位。
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
            m_need_reCreate.store(true, std::memory_order_relaxed);
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
        float scaleX = m_scissorScaleX > 0.0f ? m_scissorScaleX : getDpiScale();
        float scaleY = m_scissorScaleY > 0.0f ? m_scissorScaleY : getDpiScale();
        vk::Rect2D physical;
        physical.offset.x =
            static_cast<int32_t>(logicalScissor.offset.x * scaleX);
        physical.offset.y =
            static_cast<int32_t>(logicalScissor.offset.y * scaleY);
        physical.extent.width =
            static_cast<uint32_t>(logicalScissor.extent.width * scaleX);
        physical.extent.height =
            static_cast<uint32_t>(logicalScissor.extent.height * scaleY);
        return physical;
    }

    /// @brief 当前命令录制阶段临时覆盖的裁剪 X 轴缩放。
    float m_scissorScaleX{ 0.0f };

    /// @brief 当前命令录制阶段临时覆盖的裁剪 Y 轴缩放。
    float m_scissorScaleY{ 0.0f };

    /// @brief 是否需要重建
    /// @warning 热路径/原子：渲染准备阶段读取、UI 尺寸变化写入；仅为离屏
    /// framebuffer 脏位。
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

    /// @brief 清空派生视图缓存的 shader 源码。
    /// @warning 低频资源重载路径：皮肤热切换时执行；不得在命令录制热路径调用。
    virtual void invalidateShaderSourceCache() {}

    // --- 获取数据供 Vulkan 使用 ---
    virtual const std::vector<Vertex::VKBasicVertex>& getVertices() const = 0;

    virtual const std::vector<uint32_t>& getIndices() const = 0;

    /// @brief 在开始离屏 RenderPass 前记录派生视图的资源上传命令。
    /// @param cmdBuf 当前帧独占的离屏命令缓冲。
    /// @param frameIndex 当前并发帧索引。
    /// @warning 渲染命令录制热路径：每帧至多调用一次；仅允许记录已准备资源的
    /// Vulkan 命令，禁止分配、submit、waitIdle、文件访问或锁等待。
    virtual void onRecordResourceUploads(vk::CommandBuffer& cmdBuf,
                                         uint32_t           frameIndex)
    {
        (void)cmdBuf;
        (void)frameIndex;
    }

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

    /// @brief 记录最终覆盖层绘制命令，覆盖层会在普通层与发光合成之后绘制。
    /// @warning
    /// 热路径：每帧离屏命令录制末尾执行；只允许遍历已经生成的覆盖层命令。
    virtual void onRecordOverlayCmds(vk::CommandBuffer&      cmdBuf,
                                     vk::PipelineLayout      pipelineLayout,
                                     vk::DescriptorSetLayout setLayout,
                                     vk::DescriptorSet       defaultDescriptor,
                                     uint32_t                frameIndex)
    {
    }

    /// @brief 判断当前帧是否存在需要发光后处理的绘制命令。
    /// @return 存在发光命令时返回 true。
    /// @warning
    /// 渲染热路径：每帧离屏命令录制时执行，只能读取已生成快照中的缓存状态。
    virtual bool hasGlowDrawCmds() const { return false; }

    /// @brief 判断当前帧是否存在需要最终覆盖绘制的命令。
    /// @return 存在覆盖层命令时返回 true。
    /// @warning 渲染热路径：每帧离屏命令录制时执行，只能读取快照中的缓存状态。
    virtual bool hasOverlayDrawCmds() const { return false; }

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
    vk::Sampler                    m_glowSampler{ VK_NULL_HANDLE };
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
