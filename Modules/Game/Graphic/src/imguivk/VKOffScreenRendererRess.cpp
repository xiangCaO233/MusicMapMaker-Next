#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "graphic/imguivk/VKTexture.h"
#include "graphic/imguivk/mem/VKUniforms.h"
#include "graphic/imguivk/mesh/VKBasicVertex.h"
#include "imgui_impl_vulkan.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <utility>

namespace MMM::Graphic
{

// 顶点信息
std::array<Vertex::VKBasicVertex, 3> g_vertices{
    Vertex::VKBasicVertex{
        .pos   = { .x = 0.f, .y = -.5f },
        .color = { .r = 1.f, .g = 0.f, .b = 0.f, .a = 1.f } },
    Vertex::VKBasicVertex{
        .pos   = { .x = .5f, .y = .5f },
        .color = { .r = 0.f, .g = 1.f, .b = 0.f, .a = 1.f } },
    Vertex::VKBasicVertex{
        .pos   = { .x = -.5f, .y = .5f },
        .color = { .r = 0.f, .g = 0.f, .b = 1.f, .a = 1.f } },

};

void VKOffScreenRenderer::transitionImageInternal(vk::CommandPool pool,
                                                  vk::Queue       queue,
                                                  vk::ImageLayout oldLayout,
                                                  vk::ImageLayout newLayout)
{
    vk::CommandBufferAllocateInfo allocInfo(
        pool, vk::CommandBufferLevel::ePrimary, 1);
    vk::CommandBuffer cmd = m_device.allocateCommandBuffers(allocInfo)[0];
    cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });

    vk::ImageMemoryBarrier barrier(
        {},
        {},
        oldLayout,
        newLayout,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        m_image,
        { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });

    // 这里根据布局设置具体的 AccessMask 和 StageMask (参考 VKTexture 里的实现)
    vk::PipelineStageFlags srcStage = vk::PipelineStageFlagBits::eTopOfPipe;
    vk::PipelineStageFlags dstStage = vk::PipelineStageFlagBits::eAllCommands;

    cmd.pipelineBarrier(srcStage, dstStage, {}, nullptr, nullptr, barrier);
    cmd.end();

    vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, &cmd);
    queue.submit(submitInfo, nullptr);
    queue.waitIdle();  // 必须等待完成，因为后面要立刻用
    m_device.freeCommandBuffers(pool, cmd);
}

/// @brief 重建帧缓冲
void VKOffScreenRenderer::reCreateFrameBuffer(
    vk::PhysicalDevice& phyDevice, vk::Device& logicalDevice,
    VKSwapchain& swapchain, vk::CommandPool commandPool, vk::Queue queue,
    const std::filesystem::path& shaderModulePath, size_t maxVertexCount)
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

    // 创建所有着色器模块
    createShaderModules();

    // 用主着色器创建画笔主渲染管线(2DCanvas)
    m_mainBrushRenderPipeline =
        std::make_unique<VKRenderPipeline>(logicalDevice,
                                           *m_vkShaders[getShaderName("main")],
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
    // 4.5 初始布局转换 (重要！)
    // ==========================================
    // 刚创建的 Image 是 Undefined。虽然 RenderPass 会负责转换，
    // 但 ImGui 描述符集在录制时就会检查图像当前状态。
    {
        // 你可以复用 VKTexture 里的 transitionImageLayout
        // 逻辑，或者简单写一个： 注意：这里需要传入 physicalDevice,
        // commandPool, queue
        transitionImageInternal(commandPool,
                                queue,
                                vk::ImageLayout::eUndefined,
                                vk::ImageLayout::eShaderReadOnlyOptimal);
    }

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
    size_t bufferSize = sizeof(Vertex::VKBasicVertex) * maxVertexCount;

    // 创建缓冲区
    m_vertexBuffer = std::make_unique<VKMemBuffer>(
        phyDevice,
        m_device,
        bufferSize,
        vk::BufferUsageFlagBits::eVertexBuffer,  // 作为顶点缓冲区
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent  // CPU 可见且自动同步
    );

    m_indexBuffer = std::make_unique<VKMemBuffer>(
        phyDevice,
        m_device,
        bufferSize,
        vk::BufferUsageFlagBits::eIndexBuffer,  // 作为顶点索引缓冲区
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent  // CPU 可见且自动同步
    );

    m_uniformBuffer = std::make_unique<VKMemBuffer>(
        phyDevice,
        m_device,
        sizeof(float),
        vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent);

    // ==========================================
    // 7. 创建uniform需要的描述符池和描述符集
    // ==========================================
    createDescriptPool();
    createDescriptSets();

    // ==========================================
    // 8. 上传顶点数据到 顶点缓冲区
    // ==========================================
    {
        size_t dataSize = sizeof(Vertex::VKBasicVertex) * g_vertices.size();

        // 直接调用封装好的 uploadData 方法！
        // 内部已经处理好了 memcpy 和 flush（如果需要的话）。
        m_vertexBuffer->uploadData(g_vertices.data(), dataSize);
    }

    m_width  = creationW;
    m_height = creationH;

    m_need_reCreate.store(false);

    XINFO(
        "VKOffScreenRenderer recreate successfully[{}x{}]", m_width, m_height);

    // 在 reCreateFrameBuffer 中
    if ( !m_whiteTexture ) {
        // 定义一个 1x1 的白色像素 (RGBA)
        unsigned char whitePixel[] = { 255, 255, 255, 255 };

        // 使用新的内存构造函数
        // 注意：这里的 m_commandPool 需要从 Renderer 传入或者在
        // OffScreenRenderer 内部自己管理一个
        m_whiteTexture =
            std::make_unique<VKTexture>(whitePixel,
                                        1,
                                        1,
                                        phyDevice,
                                        m_device,
                                        commandPool,
                                        queue  // 确保这两个句柄有效
            );
    }

    // ==========================================
    // 9. 更新描述符集
    // ==========================================
    updateDescriptorSets();
}
/**
 * @brief 创建描述符池
 */
void VKOffScreenRenderer::createDescriptPool()
{
    // ==========================================
    // 7. 创建独立的描述符池 (分配2个 Set)
    // ==========================================
    vk::DescriptorPoolSize uniformPoolSize(vk::DescriptorType::eUniformBuffer,
                                           1);
    vk::DescriptorPoolSize samplerPoolSize(
        vk::DescriptorType::eCombinedImageSampler, 1);
    std::array<vk::DescriptorPoolSize, 2> poolSizes{ uniformPoolSize,
                                                     samplerPoolSize };
    vk::DescriptorPoolCreateInfo          poolInfo({}, 2, 2, poolSizes.data());
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
        m_descriptorPool, 1, &m_mainBrushRenderPipeline->m_descriptorSetLayout);
    m_offScreenDescriptorSet = m_device.allocateDescriptorSets(allocInfo)[0];
}

/**
 * @brief 更新描述符集
 */
void VKOffScreenRenderer::updateDescriptorSets()
{
    // 配置图像信息
    vk::DescriptorImageInfo imageInfo;
    imageInfo.setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(m_whiteTexture->getImageView())  // 绑白色纹理视图
        .setSampler(m_whiteTexture->getSampler());     // 绑白色纹理采样器

    vk::WriteDescriptorSet write;
    write.setDstSet(m_offScreenDescriptorSet)
        .setDstBinding(0)
        .setDstArrayElement(0)
        // 类型为纹理采样器
        .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
        .setImageInfo(imageInfo);

    m_device.updateDescriptorSets(1, &write, 0, nullptr);

    // 更新描述符集，指向 m_uniformBuffer
    // vk::DescriptorBufferInfo bufferInfo(
    //     m_uniformBuffer->m_vkBuffer, 0, sizeof(VKTestTimeUniform));
    // vk::WriteDescriptorSet write(m_offScreenDescriptorSet,
    //                              0,
    //                              0,
    //                              1,
    //                              vk::DescriptorType::eUniformBuffer,
    //                              nullptr,
    //                              &bufferInfo);
    // m_device.updateDescriptorSets(write, nullptr);
}

/**
 * @brief 上传uniform缓冲区到GPU
 */
void VKOffScreenRenderer::uploadUniformBuffer2GPU()
{
    // static float currentTime{ 0.f };
    // // 获取当前时间
    // static auto start_time =
    //     std::chrono::high_resolution_clock::now().time_since_epoch();
    // auto since_start =
    //     std::chrono::high_resolution_clock::now().time_since_epoch() -
    //     start_time;
    // auto current_time_ms =
    //     std::chrono::duration_cast<std::chrono::milliseconds>(since_start);
    // currentTime = static_cast<float>(current_time_ms.count()) / 1000.f;

    // // 更新 Uniform 数据 (时间)
    // VKTestTimeUniform ubo{ currentTime };
    // m_uniformBuffer->uploadData(&ubo, sizeof(ubo));
}

std::unique_ptr<VKShader> createShaderModule(
    std::vector<std::string> main_shader_sources, std::string module_name,
    vk::Device& logicalDevice)
{

    if ( main_shader_sources.empty() ) {
        // 没给shader源码
        return nullptr;
    }
    if ( main_shader_sources.size() < 2 ) {
        // 给的shader源码数量不够
        XWARN(
            "No enough Shader Sources defined in SkinConfig, at lease "
            "define "
            "VertexShader and FragmentShader",
            module_name);
        return nullptr;
    }

    std::unique_ptr<Graphic::VKShader> mainShader{ nullptr };
    if ( main_shader_sources.size() < 3 ) {
        // 给的shader源码不包含几何着色器
        // 创建着色器
        return std::make_unique<Graphic::VKShader>(
            logicalDevice, main_shader_sources[0], main_shader_sources[1]);

    } else {
        if ( main_shader_sources.size() > 3 ) {
            // 给的shader源码太多，多的不用
            XWARN("Extra Shader will not use in Your Canvas");
        }
        // 创建着色器
        return std::make_unique<Graphic::VKShader>(logicalDevice,
                                                   main_shader_sources[0],
                                                   main_shader_sources[1],
                                                   main_shader_sources[2]);
    }
}

/**
 * @brief 创建所有着色器
 */
void VKOffScreenRenderer::createShaderModules()
{
    // 获取主着色器spv源代码
    auto main_shader_sources = getShaderSources("main");
    // 创建主着色器
    auto main_shader_module =
        createShaderModule(main_shader_sources, "main", m_device);
    // 放入着色器表
    m_vkShaders.emplace(getShaderName("main"), std::move(main_shader_module));

    // 获取效果着色器spv源代码
    auto effect_shader_sources = getShaderSources("effect");
    // 尝试创建效果着色器
    auto effect_shader_module =
        createShaderModule(effect_shader_sources, "effect", m_device);
    if ( effect_shader_module ) {
        // 放入全局着色器表
        m_vkShaders.emplace(getShaderName("effect"),
                            std::move(effect_shader_module));
    } else {
        XWARN("No {} Shader define.", getShaderName("effect"));
    }
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

        // 清理创建出的着色器程序
        m_vkShaders.clear();

        m_device.destroyFramebuffer(m_framebuffer);
        m_device.destroySampler(m_sampler);
        m_device.destroyImageView(m_imageView);
        m_device.destroyImage(m_image);
        m_device.freeMemory(m_imageMemory);

        // 还要销毁管线和 RenderPass (虽然它们被 unique_ptr 包裹，但 reCreate
        // 时 reset 也会触发销毁，确保它们也执行了销毁逻辑)
        m_mainBrushRenderPipeline.reset();
        m_offScreenRenderPass.reset();

        m_vertexBuffer.reset();
        m_indexBuffer.reset();
        m_uniformBuffer.reset();
    }
}

}  // namespace MMM::Graphic
