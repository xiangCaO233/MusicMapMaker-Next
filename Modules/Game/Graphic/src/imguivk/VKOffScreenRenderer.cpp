#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "graphic/imguivk/mem/VKUniforms.h"
#include "graphic/imguivk/mesh/VKVertex.h"
#include "imgui_impl_vulkan.h"
#include "log/colorful-log.h"
#include "vulkan/vulkan.hpp"
#include <filesystem>
#include <fstream>

namespace MMM::Graphic
{

// 顶点信息
std::array<VKVertex, 3> g_vertices{
    VKVertex{ .pos   = { .x = 0.f, .y = -.5f },
              .color = { .r = 1.f, .g = 0.f, .b = 0.f, .a = 1.f } },
    VKVertex{ .pos   = { .x = .5f, .y = .5f },
              .color = { .r = 0.f, .g = 1.f, .b = 0.f, .a = 1.f } },
    VKVertex{ .pos   = { .x = -.5f, .y = .5f },
              .color = { .r = 0.f, .g = 0.f, .b = 1.f, .a = 1.f } },

};

VKOffScreenRenderer::VKOffScreenRenderer() {}

VKOffScreenRenderer::~VKOffScreenRenderer()
{
    // 1. 先等待设备空闲，防止正在渲染时销毁
    if ( m_device ) m_device.waitIdle();

    releaseResources();
    XINFO("VKOffScreenRenderer destroyed.");
}

/// @brief 重建帧缓冲
void VKOffScreenRenderer::reCreateFrameBuffer(vk::PhysicalDevice& phyDevice,
                                              vk::Device&         logicalDevice,
                                              VKSwapchain&        swapchain,
                                              size_t maxVertexCount)
{
    // 1. 确认是真的需要重建（再次检查消抖，防止并发问题）
    if ( !needReCreateFrameBuffer() ) return;

    // 确保不为 0
    if ( m_targetWidth == 0 || m_targetHeight == 0 ) return;

    // 1. 先等待设备空闲，防止正在渲染时销毁
    if ( m_device ) m_device.waitIdle();

    // 先释放
    releaseResources();

    // 赋值引用
    m_device = logicalDevice;

    // 赋值实际尺寸
    uint32_t creationW = m_targetWidth;
    uint32_t creationH = m_targetHeight;

    // 创建离屏渲染流程
    m_offScreenRenderPass = std::make_unique<VKRenderPass>(
        logicalDevice, swapchain, vk::ImageLayout::eShaderReadOnlyOptimal);

    createShader();

    // 创建画笔渲染管线(2DCanvas)
    m_brushRenderPipeline =
        std::make_unique<VKRenderPipeline>(logicalDevice,
                                           *m_vkShaders["testShader"],
                                           *m_offScreenRenderPass,
                                           swapchain,
                                           true);

    // 再重建
    // ==========================================
    // 1. 创建 vk::Image (离屏画布)
    // ==========================================
    vk::ImageCreateInfo imageInfo;
    imageInfo.setImageType(vk::ImageType::e2D)
        .setFormat(swapchain.info().imageFormat)  // 与SwapChain中保持一致
        .setExtent(vk::Extent3D{ creationW, creationH, 1 })
        .setMipLevels(1)
        .setArrayLayers(1)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal)
        // 关键：作为颜色附件输出，并且可以被采样器读取
        .setUsage(vk::ImageUsageFlagBits::eColorAttachment |
                  vk::ImageUsageFlagBits::eSampled)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);

    m_image = m_device.createImage(imageInfo);

    // ==========================================
    // 2. 为 Image 分配物理显存 (DeviceLocal)
    // ==========================================
    vk::MemoryRequirements memRequirements =
        m_device.getImageMemoryRequirements(m_image);

    // 辅助 Lambda：查找合适的内存类型
    auto findMemoryType = [&](uint32_t                typeFilter,
                              vk::MemoryPropertyFlags properties) -> uint32_t {
        vk::PhysicalDeviceMemoryProperties memProperties =
            phyDevice.getMemoryProperties();
        for ( uint32_t i = 0; i < memProperties.memoryTypeCount; i++ ) {
            if ( (typeFilter & (1 << i)) &&
                 (memProperties.memoryTypes[i].propertyFlags & properties) ==
                     properties ) {
                return i;
            }
        }
        XCRITICAL("Failed to find suitable memory type for OffScreen Image!");
        return 0;
    };
    vk::MemoryAllocateInfo allocInfo;
    allocInfo.setAllocationSize(memRequirements.size)
        .setMemoryTypeIndex(
            findMemoryType(memRequirements.memoryTypeBits,
                           vk::MemoryPropertyFlagBits::eDeviceLocal));

    m_imageMemory = m_device.allocateMemory(allocInfo);
    // 绑定显存到 Image
    m_device.bindImageMemory(m_image, m_imageMemory, 0);

    // ==========================================
    // 3. 创建 ImageView 和 Sampler
    // ==========================================
    vk::ImageViewCreateInfo viewInfo;
    viewInfo.setImage(m_image)
        .setViewType(vk::ImageViewType::e2D)
        .setFormat(swapchain.info().imageFormat)  // 与SwapChain中保持一致
        .setSubresourceRange(vk::ImageSubresourceRange(
            vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

    m_imageView = m_device.createImageView(viewInfo);

    vk::SamplerCreateInfo samplerInfo;
    samplerInfo.setMagFilter(vk::Filter::eLinear)
        .setMinFilter(vk::Filter::eLinear)
        .setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
        .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
        .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
        .setAnisotropyEnable(VK_FALSE)
        .setBorderColor(vk::BorderColor::eIntOpaqueBlack)
        .setUnnormalizedCoordinates(VK_FALSE)
        .setCompareEnable(VK_FALSE)
        .setCompareOp(vk::CompareOp::eAlways)
        .setMipmapMode(vk::SamplerMipmapMode::eLinear);

    m_sampler = m_device.createSampler(samplerInfo);

    // ==========================================
    // 4. 创建 Framebuffer 供 RenderPass 渲染使用
    // ==========================================
    vk::FramebufferCreateInfo framebufferInfo;
    framebufferInfo.setRenderPass(m_offScreenRenderPass->getRenderPass())
        .setAttachments(m_imageView)
        .setWidth(creationW)
        .setHeight(creationH)
        .setLayers(1);

    m_framebuffer = m_device.createFramebuffer(framebufferInfo);

    // ==========================================
    // 5. 注册到 ImGui
    // ==========================================
    // ImGui_ImplVulkan_AddTexture 会将 ImageView 和 Sampler 封装到一个
    // DescriptorSet 中返回
    m_imguiDescriptor = (vk::DescriptorSet)ImGui_ImplVulkan_AddTexture(
        (VkSampler)m_sampler,
        (VkImageView)m_imageView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL  // ImGui读取时，期望它处于的布局状态
    );

    // ==========================================
    // 6. 创建顶点缓冲区和uniform缓冲区
    // ==========================================
    // 计算缓冲区大小
    size_t bufferSize = sizeof(VKVertex) * maxVertexCount;
    // 创建缓冲区
    m_vertexBuffer = std::make_unique<VKMemBuffer>(
        phyDevice,
        m_device,
        bufferSize,
        vk::BufferUsageFlagBits::eVertexBuffer,  // 作为顶点缓冲区
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent  // CPU 可见且自动同步
    );

    m_uniformBuffer = std::make_unique<VKMemBuffer>(
        phyDevice,
        m_device,
        sizeof(VKTestTimeUniform),
        vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent);

    // ==========================================
    // 7. 创建uniform需要的描述符池和描述符集
    // ==========================================
    createDescriptPool();
    createDescriptSets();

    // ==========================================
    // 8. 映射uniform内存到描述符集
    // ==========================================
    mapUniformBuffer2DescriptorSet();

    // ==========================================
    // 9. 上传顶点数据到 顶点缓冲区
    // ==========================================
    {
        // 计算实际数据大小
        size_t dataSize = sizeof(VKVertex) * g_vertices.size();

        // 映射内存
        // 注意：m_vertexBuffer 是 friend 类，所以可以直接访问 m_vkDevMem
        void* data =
            m_device.mapMemory(m_vertexBuffer->m_vkDevMem, 0, dataSize);

        // 拷贝数据
        memcpy(data, g_vertices.data(), dataSize);

        // 解除映射
        m_device.unmapMemory(m_vertexBuffer->m_vkDevMem);
    }

    m_width  = creationW;
    m_height = creationH;

    m_need_reCreate.store(false);

    XINFO(
        "VKOffScreenRenderer recreate successfully[{}x{}]", m_width, m_height);
}

/// @brief 录制gpu指令
void VKOffScreenRenderer::recordCmds(vk::CommandBuffer&   cmdBuf,
                                     UI::IRenderableView* renderable_view)
{
    // 1. 设置清除颜色 (Alpha 为 0，确保画布背景透明)
    vk::ClearValue clearValue;
    clearValue.setColor(
        vk::ClearColorValue(std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f }));
    uploadUniformBuffer2GPU();

    // 2. 开始渲染流程 (针对离屏 Framebuffer)
    vk::RenderPassBeginInfo rpBegin;
    rpBegin.setRenderPass(m_offScreenRenderPass->getRenderPass())
        .setFramebuffer(m_framebuffer)
        .setRenderArea(vk::Rect2D({ 0, 0 }, { m_width, m_height }))
        .setClearValues(clearValue);

    cmdBuf.beginRenderPass(rpBegin, vk::SubpassContents::eInline);
    {
        // 3. 因为开启了动态状态，必须手动设置 Viewport 和 Scissor
        vk::Viewport viewport(
            0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f);
        vk::Rect2D scissor({ 0, 0 }, { m_width, m_height });
        cmdBuf.setViewport(0, 1, &viewport);
        cmdBuf.setScissor(0, 1, &scissor);

        // 4. 绑定离屏专属管线
        cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                            m_brushRenderPipeline->m_graphicsPipeline);

        // 4. ★ 关键：绑定描述符集（解决报错）
        // 注意：这里使用的 Layout 必须是创建该 Pipeline 时用的那个 Layout
        cmdBuf.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            m_brushRenderPipeline->m_graphicsPipelineLayout,
            0,
            1,
            &m_offScreenDescriptorSet,
            0,
            nullptr);

        // 5. 绑定顶点缓冲区 (你的 VKMemBuffer)
        cmdBuf.bindVertexBuffers(0, m_vertexBuffer->m_vkBuffer, { 0 });

        // 6. 执行绘制 (根据 Brush 的数据)
        cmdBuf.draw(3, 1, 0, 0);
    }
    cmdBuf.endRenderPass();
}

/**
 * @brief 创建描述符池
 */
void VKOffScreenRenderer::createDescriptPool()
{
    // ==========================================
    // 7. 创建独立的描述符池 (只分配1个 Set)
    // ==========================================
    vk::DescriptorPoolSize poolSize(vk::DescriptorType::eUniformBuffer, 1);
    vk::DescriptorPoolCreateInfo poolInfo({}, 1, 1, &poolSize);
    m_descriptorPool = m_device.createDescriptorPool(poolInfo);
}

/**
 * @brief 创建描述符集列表
 */
void VKOffScreenRenderer::createDescriptSets()
{
    // ==========================================
    // 8. 分配并绑定描述符集
    // ==========================================
    // 使用离屏管线创建时自动生成的 Layout
    // (m_brushRenderPipeline->m_descriptorSetLayout)
    vk::DescriptorSetAllocateInfo allocInfo(
        m_descriptorPool, 1, &m_brushRenderPipeline->m_descriptorSetLayout);
    m_offScreenDescriptorSet = m_device.allocateDescriptorSets(allocInfo)[0];
}

/**
 * @brief 映射uniformbuffer到对应描述符集
 */
void VKOffScreenRenderer::mapUniformBuffer2DescriptorSet() const
{
    // 更新描述符集，指向我们的 m_uniformBuffer
    vk::DescriptorBufferInfo bufferInfo(
        m_uniformBuffer->m_vkBuffer, 0, sizeof(VKTestTimeUniform));
    vk::WriteDescriptorSet write(m_offScreenDescriptorSet,
                                 0,
                                 0,
                                 1,
                                 vk::DescriptorType::eUniformBuffer,
                                 nullptr,
                                 &bufferInfo);
    m_device.updateDescriptorSets(write, nullptr);
}

/**
 * @brief 上传uniform缓冲区到GPU
 */
void VKOffScreenRenderer::uploadUniformBuffer2GPU()
{
    static float currentTime{ 0.f };
    // 获取当前时间
    static auto start_time =
        std::chrono::high_resolution_clock::now().time_since_epoch();
    auto since_start =
        std::chrono::high_resolution_clock::now().time_since_epoch() -
        start_time;
    auto current_time_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(since_start);
    currentTime = static_cast<float>(current_time_ms.count()) / 1000.f;
    // 1. 更新 Uniform 数据 (时间)
    VKTestTimeUniform ubo{ currentTime };
    void*             data =
        m_device.mapMemory(m_uniformBuffer->m_vkDevMem, 0, sizeof(ubo));
    memcpy(data, &ubo, sizeof(ubo));
    m_device.unmapMemory(m_uniformBuffer->m_vkDevMem);
}


/// @brief 释放持有的资源
void VKOffScreenRenderer::releaseResources()
{
    // 如果还没被释放，先从 ImGui 注销 DescriptorSet
    // 避免 Descriptor Pool 提前释放导致的报错
    if ( m_imguiDescriptor ) {
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)m_imguiDescriptor);
        m_imguiDescriptor = nullptr;
    }

    // 释放 Vulkan 原生资源
    if ( m_device ) {
        // ★ 必须手动销毁描述符池
        if ( m_descriptorPool ) {
            m_device.destroyDescriptorPool(m_descriptorPool);
            m_descriptorPool = nullptr;
        }

        // 清理着色器程序
        m_vkShaders.clear();


        m_device.destroyFramebuffer(m_framebuffer);
        m_device.destroySampler(m_sampler);
        m_device.destroyImageView(m_imageView);
        m_device.destroyImage(m_image);
        m_device.freeMemory(m_imageMemory);

        // ★ 还要销毁管线和 RenderPass (虽然它们被 unique_ptr 包裹，但 reCreate
        // 时 reset 也会触发销毁，确保它们也执行了销毁逻辑)
        m_brushRenderPipeline.reset();
        m_offScreenRenderPass.reset();
        m_vertexBuffer.reset();
        m_uniformBuffer.reset();
    }
    if ( m_imguiDescriptor ) {
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)m_imguiDescriptor);
        m_imguiDescriptor = nullptr;
    }
}

std::string readFile(std::string path)
{
    // 读取文件内容
    std::ifstream fs;
    fs.open(path, std::ios::binary | std::ios::ate);
    if ( !fs.is_open() ) {
        XERROR("Fatal: Could not open File[{}]!", path);
        return {};
    }
    auto        sourceSize = fs.tellg();
    std::string source;
    source.resize(sourceSize);
    fs.seekg(0);
    fs.read(source.data(), sourceSize);
    fs.close();
    return source;
}

/**
 * @brief 加载并创建 Shader 模块
 */
void VKOffScreenRenderer::createShader()
{
    // 读取源码初始化shader
    // 假设 assets 肯定在运行目录上n级
    // 而 build 目录通常在 root/build/Modules/Main/ 下 (深度为 3 或 4)
    auto rootDir = std::filesystem::current_path();
    // 向上查找直到找到 assets 文件夹
    while ( !std::filesystem::exists(rootDir / "assets") &&
            rootDir.has_parent_path() ) {
        rootDir = rootDir.parent_path();
    }

    if ( !std::filesystem::exists(rootDir / "assets") ) {
        XERROR("Fatal: Could not find assets directory!");
        return;
    }

    // 跨平台（自动处理 / 或 \）
    auto assetPath = rootDir / "assets";
    auto testVertexShaderSourcePath =
        assetPath / "shaders" / "testVertexShader.spv";
    auto testFragmentShaderSourcePath =
        assetPath / "shaders" / "testFragmentShader.spv";

    // 读取文件内容
    std::string testVertexShaderSource =
        readFile(testVertexShaderSourcePath.generic_string());
    std::string testFragmentShaderSource =
        readFile(testFragmentShaderSourcePath.generic_string());

    // 创建着色器
    auto test_vkShader = std::make_unique<VKShader>(
        m_device, testVertexShaderSource, testFragmentShaderSource);
    m_vkShaders.emplace("testShader", std::move(test_vkShader));
}

}  // namespace MMM::Graphic
