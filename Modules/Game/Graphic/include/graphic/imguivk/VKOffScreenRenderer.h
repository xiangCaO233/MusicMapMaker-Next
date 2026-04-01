#pragma once

#include "graphic/imguivk/VKRenderPipeline.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "graphic/imguivk/mem/VKMemBuffer.h"
#include "graphic/imguivk/mesh/VKBasicVertex.h"
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

    /// @brief 录制gpu指令
    void recordCmds(vk::CommandBuffer& cmdBuf);

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
    inline void setTargetSize(uint32_t w, uint32_t h)
    {
        if ( w != m_targetWidth || h != m_targetHeight ) {
            m_targetWidth     = w;
            m_targetHeight    = h;
            m_lastRequestTime = std::chrono::steady_clock::now();
            // 标记为“有变更待处理”
            m_need_reCreate.store(true);
        }
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
    virtual void onRecordDrawCmds(vk::CommandBuffer& cmdBuf,
                                  vk::PipelineLayout pipelineLayout,
                                  vk::DescriptorSet  defaultDescriptor) = 0;

private:
    // --- 1. 物理资源 (独占) ---
    vk::Image        m_image;        // 画布纹理
    vk::DeviceMemory m_imageMemory;  // 纹理内存
    vk::ImageView    m_imageView;    // 纹理视图
    vk::Framebuffer  m_framebuffer;  // 绑定到此纹理的帧缓冲
    vk::Sampler      m_sampler;      // 绑定到此纹理的采样器

    // --- 2. 几何资源 (独占) ---
    // 存 Brush 的顶点
    std::unique_ptr<VKMemBuffer> m_vertexBuffer;

    // 存 Brush 的索引
    std::unique_ptr<VKMemBuffer> m_indexBuffer;

    // 离屏用的 Uniform Buffer
    std::unique_ptr<VKMemBuffer> m_uniformBuffer;

    // 离屏用描述符池
    vk::DescriptorPool m_descriptorPool;

    // 离屏用描述符集
    vk::DescriptorSet m_offScreenDescriptorSet;

    // 离屏用白色纹理
    std::unique_ptr<VKTexture> m_whiteTexture;

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
                                 vk::ImageLayout newLayout);


    // --- 3. UI 集成句柄 (独占) ---
    vk::DescriptorSet m_imguiDescriptor{ VK_NULL_HANDLE };  // ImGui 用的贴图 ID

    // --- 4. 引用/外部注入 (非所有权) ---
    // 逻辑设备引用
    vk::Device m_device{ VK_NULL_HANDLE };

    // 离屏渲染流程
    std::unique_ptr<VKRenderPass> m_offScreenRenderPass{ nullptr };

    // 画笔管线
    std::unique_ptr<VKRenderPipeline> m_mainBrushRenderPipeline{ nullptr };

    /// @brief 释放持有的资源
    void releaseResources();
};

}  // namespace MMM::Graphic
