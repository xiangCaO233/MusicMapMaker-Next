#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "graphic/imguivk/VKTexture.h"
#include "graphic/imguivk/mesh/VKBasicVertex.h"
#include "imgui_impl_vulkan.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <utility>

namespace MMM::Graphic
{

void VKOffScreenRenderer::transitionImageInternal(vk::CommandPool commandPool,
                                                  vk::Queue       queue,
                                                  vk::ImageLayout oldLayout,
                                                  vk::ImageLayout newLayout,
                                                  vk::Image       image)
{
    vk::CommandBufferAllocateInfo allocInfo;
    allocInfo.setCommandPool(commandPool)
        .setLevel(vk::CommandBufferLevel::ePrimary)
        .setCommandBufferCount(1);

    vk::CommandBuffer cmd = m_device.allocateCommandBuffers(allocInfo).value[0];

    vk::CommandBufferBeginInfo beginInfo;
    beginInfo.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    (void)cmd.begin(beginInfo);

    vk::ImageMemoryBarrier barrier;
    barrier.setOldLayout(oldLayout)
        .setNewLayout(newLayout)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(image ? image : m_image)
        .setSubresourceRange({ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });

    vk::PipelineStageFlags srcStage;
    vk::PipelineStageFlags dstStage;

    // 核心修正：针对 Intel 核显驱动，必须显式指定 AccessMask 和 StageMask
    // 的同步关系
    if ( oldLayout == vk::ImageLayout::eUndefined &&
         newLayout == vk::ImageLayout::eShaderReadOnlyOptimal ) {
        barrier.setSrcAccessMask(vk::AccessFlagBits::eNone)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
        srcStage = vk::PipelineStageFlagBits::eTopOfPipe;
        dstStage = vk::PipelineStageFlagBits::eFragmentShader;
    } else if ( oldLayout == vk::ImageLayout::eUndefined &&
                newLayout == vk::ImageLayout::eColorAttachmentOptimal ) {
        barrier.setSrcAccessMask(vk::AccessFlagBits::eNone)
            .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite);
        srcStage = vk::PipelineStageFlagBits::eTopOfPipe;
        dstStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    } else {
        // 通用兜底
        barrier
            .setSrcAccessMask(vk::AccessFlagBits::eMemoryRead |
                              vk::AccessFlagBits::eMemoryWrite)
            .setDstAccessMask(vk::AccessFlagBits::eMemoryRead |
                              vk::AccessFlagBits::eMemoryWrite);
        srcStage = vk::PipelineStageFlagBits::eAllCommands;
        dstStage = vk::PipelineStageFlagBits::eAllCommands;
    }

    cmd.pipelineBarrier(srcStage, dstStage, {}, nullptr, nullptr, barrier);

    (void)cmd.end();

    vk::SubmitInfo submitInfo;
    submitInfo.setCommandBuffers(cmd);
    (void)queue.submit(submitInfo);
    (void)queue.waitIdle();  // 必须等待完成，因为后面要立刻用

    m_device.freeCommandBuffers(commandPool, cmd);
}

void VKOffScreenRenderer::createOffscreenBuffer(
    vk::PhysicalDevice& phyDevice, vk::Device& logicalDevice,
    VKSwapchain& swapchain, vk::CommandPool commandPool, vk::Queue queue,
    uint32_t width, uint32_t height, vk::Image& image, vk::DeviceMemory& memory,
    vk::ImageView& imageView, vk::Framebuffer& framebuffer,
    vk::Sampler& sampler, VKRenderPass* pass)
{
    vk::ImageCreateInfo imageInfo;
    imageInfo.setImageType(vk::ImageType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Unorm)  // 修正：强制使用 R8G8B8A8Unorm
        .setExtent({ width, height, 1 })
        .setMipLevels(1)
        .setArrayLayers(1)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal)
        .setUsage(vk::ImageUsageFlagBits::eColorAttachment |
                  vk::ImageUsageFlagBits::eSampled |
                  vk::ImageUsageFlagBits::eTransferSrc |
                  vk::ImageUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);

    image = logicalDevice.createImage(imageInfo).value;

    vk::MemoryRequirements memRequirements =
        logicalDevice.getImageMemoryRequirements(image);

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
        return 0;
    };
    vk::MemoryAllocateInfo allocInfo;
    allocInfo.setAllocationSize(memRequirements.size)
        .setMemoryTypeIndex(
            findMemoryType(memRequirements.memoryTypeBits,
                           vk::MemoryPropertyFlagBits::eDeviceLocal));

    memory = logicalDevice.allocateMemory(allocInfo).value;
    (void)logicalDevice.bindImageMemory(image, memory, 0);

    vk::ImageViewCreateInfo viewInfo;
    viewInfo.setImage(image)
        .setViewType(vk::ImageViewType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Unorm)
        .setSubresourceRange(vk::ImageSubresourceRange(
            vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

    imageView = logicalDevice.createImageView(viewInfo).value;

    if ( !sampler ) {
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

        sampler = logicalDevice.createSampler(samplerInfo).value;
    }

    vk::FramebufferCreateInfo framebufferInfo;
    framebufferInfo.setRenderPass(pass->getRenderPass())
        .setAttachments(imageView)
        .setWidth(width)
        .setHeight(height)
        .setLayers(1);

    framebuffer = logicalDevice.createFramebuffer(framebufferInfo).value;

    transitionImageInternal(commandPool,
                            queue,
                            vk::ImageLayout::eUndefined,
                            vk::ImageLayout::eShaderReadOnlyOptimal,
                            image);
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
    // 核心修复：无论 m_device 是否有效，都尝试等待当前已有的设备
    if ( m_device ) {
        (void)m_device.waitIdle();
    }

    // 先释放
    releaseResources();

    // 赋值引用
    m_device         = logicalDevice;
    m_physicalDevice = phyDevice;

    // 赋值实际尺寸
    uint32_t creationW = m_targetWidth;
    uint32_t creationH = m_targetHeight;

    // 创建离屏渲染流程
    m_offScreenRenderPass =
        std::make_unique<VKRenderPass>(logicalDevice,
                                       swapchain,
                                       vk::ImageLayout::eShaderReadOnlyOptimal,
                                       true,
                                       vk::Format::eR8G8B8A8Unorm);

    m_compositeRenderPass =
        std::make_unique<VKRenderPass>(logicalDevice,
                                       swapchain,
                                       vk::ImageLayout::eShaderReadOnlyOptimal,
                                       false,
                                       vk::Format::eR8G8B8A8Unorm);

    // 创建模糊用的 RenderPass，不执行 Clear (因为是全屏绘制)
    m_blurRenderPass =
        std::make_unique<VKRenderPass>(logicalDevice,
                                       swapchain,
                                       vk::ImageLayout::eShaderReadOnlyOptimal,
                                       false,
                                       vk::Format::eR8G8B8A8Unorm);

    // 创建所有着色器模块
    createShaderModules();

    // 获取共享布局
    auto& renderer     = VKContext::get().value().get().getRenderer();
    auto  sharedLayout = renderer.getBrushTextureLayout();

    // 用主着色器创建画笔主渲染管线(2DCanvas)
    m_mainBrushRenderPipeline =
        std::make_unique<VKRenderPipeline>(logicalDevice,
                                           *m_vkShaders[getShaderName("main")],
                                           *m_offScreenRenderPass,
                                           swapchain,
                                           true,
                                           0,
                                           0,
                                           false,
                                           true,
                                           sharedLayout);

    m_glowBrushRenderPipeline = std::make_unique<VKRenderPipeline>(
        logicalDevice,
        *m_vkShaders[getShaderName("main")],
        *m_offScreenRenderPass,
        swapchain,
        true,
        0,
        0,
        false,  // 使用标准混合渲染发光遮罩层，保持外观一致
        true,
        sharedLayout);

    if ( m_vkShaders.count(getShaderName("effect")) ) {
        m_blurRenderPipeline = std::make_unique<VKRenderPipeline>(
            logicalDevice,
            *m_vkShaders[getShaderName("effect")],
            *m_blurRenderPass,  // 使用不 Clear 的 RenderPass
            swapchain,
            true,
            0,
            0,
            false,
            false);  // Blur pass: additiveBlend=false, blendEnable=false

        m_compositeRenderPipeline = std::make_unique<VKRenderPipeline>(
            logicalDevice,
            *m_vkShaders[getShaderName("effect")],
            *m_compositeRenderPass,
            swapchain,
            true,
            0,
            0,
            true,
            true);  // Composite pass: additive blend = true
    }

    // 再重建
    // ==========================================
    // 1. 创建 vk::Image (离屏画布)
    // ==========================================
    vk::ImageCreateInfo imageInfo;
    imageInfo.setImageType(vk::ImageType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Unorm)  // 修正：强制使用 R8G8B8A8Unorm
        .setExtent(vk::Extent3D{ creationW, creationH, 1 })
        .setMipLevels(1)
        .setArrayLayers(1)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal)
        // 关键：作为颜色附件输出，并且可以被采样器读取，增加传输支持以兼容部分驱动特性
        .setUsage(vk::ImageUsageFlagBits::eColorAttachment |
                  vk::ImageUsageFlagBits::eSampled |
                  vk::ImageUsageFlagBits::eTransferSrc |
                  vk::ImageUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);

    m_image = m_device.createImage(imageInfo).value;

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

    m_imageMemory = m_device.allocateMemory(allocInfo).value;
    // 绑定显存到 Image
    (void)m_device.bindImageMemory(m_image, m_imageMemory, 0);

    // ==========================================
    // 3. 创建 ImageView 和 Sampler
    // ==========================================
    vk::ImageViewCreateInfo viewInfo;
    viewInfo.setImage(m_image)
        .setViewType(vk::ImageViewType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Unorm)  // 修正：修正格式一致性
        .setSubresourceRange(vk::ImageSubresourceRange(
            vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

    m_imageView = m_device.createImageView(viewInfo).value;

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

    m_sampler = m_device.createSampler(samplerInfo).value;

    // ==========================================
    // 4. 创建 Framebuffer 供 RenderPass 渲染使用
    // ==========================================
    vk::FramebufferCreateInfo framebufferInfo;
    framebufferInfo.setRenderPass(m_offScreenRenderPass->getRenderPass())
        .setAttachments(m_imageView)
        .setWidth(creationW)
        .setHeight(creationH)
        .setLayers(1);

    m_framebuffer = m_device.createFramebuffer(framebufferInfo).value;

    // ==========================================
    // 4.5 初始布局转换 (重要！)
    // ==========================================
    // 刚创建的 Image 是 Undefined。虽然 RenderPass 会负责转换，
    // 但 ImGui 描述符集在录制时就会检查图像当前状态。
    {
        transitionImageInternal(commandPool,
                                queue,
                                vk::ImageLayout::eUndefined,
                                vk::ImageLayout::eShaderReadOnlyOptimal,
                                m_image);
    }

    // --- 创建离屏效果缓冲 ---
    createOffscreenBuffer(phyDevice,
                          logicalDevice,
                          swapchain,
                          commandPool,
                          queue,
                          creationW,
                          creationH,
                          m_glowImage,
                          m_glowImageMemory,
                          m_glowImageView,
                          m_glowFramebuffer,
                          m_glowSampler,
                          m_offScreenRenderPass.get());
    createOffscreenBuffer(phyDevice,
                          logicalDevice,
                          swapchain,
                          commandPool,
                          queue,
                          creationW,
                          creationH,
                          m_pingImage,
                          m_pingImageMemory,
                          m_pingImageView,
                          m_pingFramebuffer,
                          m_glowSampler,
                          m_offScreenRenderPass.get());
    createOffscreenBuffer(phyDevice,
                          logicalDevice,
                          swapchain,
                          commandPool,
                          queue,
                          creationW,
                          creationH,
                          m_pongImage,
                          m_pongImageMemory,
                          m_pongImageView,
                          m_pongFramebuffer,
                          m_glowSampler,
                          m_offScreenRenderPass.get());

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

    m_imguiGlowDescriptor = (vk::DescriptorSet)ImGui_ImplVulkan_AddTexture(
        (VkSampler)m_glowSampler,
        (VkImageView)m_glowImageView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // ==========================================
    // 6. 创建顶点缓冲区和uniform缓冲区 (多帧)
    // ==========================================
    // 计算缓冲区大小
    size_t bufferSize = sizeof(Vertex::VKBasicVertex) * maxVertexCount;

    m_vertexBuffers.clear();
    m_indexBuffers.clear();
    m_uniformBuffers.clear();

    for ( int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
        m_vertexBuffers.push_back(std::make_unique<VKMemBuffer>(
            phyDevice,
            m_device,
            bufferSize,
            vk::BufferUsageFlagBits::eVertexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));

        m_indexBuffers.push_back(std::make_unique<VKMemBuffer>(
            phyDevice,
            m_device,
            bufferSize,
            vk::BufferUsageFlagBits::eIndexBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));

        m_uniformBuffers.push_back(std::make_unique<VKMemBuffer>(
            phyDevice,
            m_device,
            sizeof(float),
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
    }

    // ==========================================
    // 7. 创建uniform需要的描述符池和描述符集
    // ==========================================
    createDescriptPool();
    createDescriptSets();

    m_width  = creationW;
    m_height = creationH;

    m_lastAllocatedCount = maxVertexCount;
    m_need_reCreate.store(false);

    XDEBUG(
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
    // 7. 创建独立的描述符池 (分配多帧所需的 Sets)
    // ==========================================
    // 每个 Frame 需要 4 个 Set (offscreen, glow, ping, pong)
    uint32_t totalSets = 4 * MAX_FRAMES_IN_FLIGHT;

    vk::DescriptorPoolSize uniformPoolSize(vk::DescriptorType::eUniformBuffer,
                                           totalSets);
    vk::DescriptorPoolSize samplerPoolSize(
        vk::DescriptorType::eCombinedImageSampler, totalSets);
    std::array<vk::DescriptorPoolSize, 2> poolSizes{ uniformPoolSize,
                                                     samplerPoolSize };
    vk::DescriptorPoolCreateInfo          poolInfo(
        {}, totalSets, (uint32_t)poolSizes.size(), poolSizes.data());
    m_descriptorPool = m_device.createDescriptorPool(poolInfo).value;
}

/**
 * @brief 创建描述符集列表
 */
void VKOffScreenRenderer::createDescriptSets()
{
    // ==========================================
    // 8. 分配并绑定描述符集
    // ==========================================
    // 使用画笔共享布局
    auto& renderer     = VKContext::get().value().get().getRenderer();
    auto  sharedLayout = renderer.getBrushTextureLayout();

    m_offScreenDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    m_glowDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    m_pingDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    m_pongDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);

    for ( int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
        vk::DescriptorSetAllocateInfo allocInfo(
            m_descriptorPool, 1, &sharedLayout);
        m_offScreenDescriptorSets[i] =
            m_device.allocateDescriptorSets(allocInfo).value[0];
        m_glowDescriptorSets[i] =
            m_device.allocateDescriptorSets(allocInfo).value[0];
        m_pingDescriptorSets[i] =
            m_device.allocateDescriptorSets(allocInfo).value[0];
        m_pongDescriptorSets[i] =
            m_device.allocateDescriptorSets(allocInfo).value[0];
    }
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

    vk::DescriptorImageInfo glowInfo;
    glowInfo.setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(m_glowImageView)
        .setSampler(m_glowSampler);

    vk::DescriptorImageInfo pingInfo;
    pingInfo.setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(m_pingImageView)
        .setSampler(m_glowSampler);

    vk::DescriptorImageInfo pongInfo;
    pongInfo.setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(m_pongImageView)
        .setSampler(m_glowSampler);

    std::vector<vk::WriteDescriptorSet> writes;
    for ( int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
        writes.push_back(
            vk::WriteDescriptorSet(m_offScreenDescriptorSets[i],
                                   0,
                                   0,
                                   1,
                                   vk::DescriptorType::eCombinedImageSampler,
                                   &imageInfo,
                                   nullptr,
                                   nullptr));
        writes.push_back(
            vk::WriteDescriptorSet(m_glowDescriptorSets[i],
                                   0,
                                   0,
                                   1,
                                   vk::DescriptorType::eCombinedImageSampler,
                                   &glowInfo,
                                   nullptr,
                                   nullptr));
        writes.push_back(
            vk::WriteDescriptorSet(m_pingDescriptorSets[i],
                                   0,
                                   0,
                                   1,
                                   vk::DescriptorType::eCombinedImageSampler,
                                   &pingInfo,
                                   nullptr,
                                   nullptr));
        writes.push_back(
            vk::WriteDescriptorSet(m_pongDescriptorSets[i],
                                   0,
                                   0,
                                   1,
                                   vk::DescriptorType::eCombinedImageSampler,
                                   &pongInfo,
                                   nullptr,
                                   nullptr));
    }
    m_device.updateDescriptorSets(writes, nullptr);

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
    if ( m_imguiGlowDescriptor ) {
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)m_imguiGlowDescriptor);
        m_imguiGlowDescriptor = nullptr;
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

        if ( m_glowFramebuffer ) m_device.destroyFramebuffer(m_glowFramebuffer);
        if ( m_pingFramebuffer ) m_device.destroyFramebuffer(m_pingFramebuffer);
        if ( m_pongFramebuffer ) m_device.destroyFramebuffer(m_pongFramebuffer);
        if ( m_glowImageView ) m_device.destroyImageView(m_glowImageView);
        if ( m_pingImageView ) m_device.destroyImageView(m_pingImageView);
        if ( m_pongImageView ) m_device.destroyImageView(m_pongImageView);
        if ( m_glowImage ) m_device.destroyImage(m_glowImage);
        if ( m_pingImage ) m_device.destroyImage(m_pingImage);
        if ( m_pongImage ) m_device.destroyImage(m_pongImage);
        if ( m_glowImageMemory ) m_device.freeMemory(m_glowImageMemory);
        if ( m_pingImageMemory ) m_device.freeMemory(m_pingImageMemory);
        if ( m_pongImageMemory ) m_device.freeMemory(m_pongImageMemory);
        if ( m_glowSampler ) m_device.destroySampler(m_glowSampler);

        m_glowFramebuffer = m_pingFramebuffer = m_pongFramebuffer =
            VK_NULL_HANDLE;
        m_glowImageView = m_pingImageView = m_pongImageView = VK_NULL_HANDLE;
        m_glowImage = m_pingImage = m_pongImage = VK_NULL_HANDLE;
        m_glowImageMemory = m_pingImageMemory = m_pongImageMemory =
            VK_NULL_HANDLE;
        m_glowSampler = VK_NULL_HANDLE;

        m_mainBrushRenderPipeline.reset();
        m_glowBrushRenderPipeline.reset();
        m_blurRenderPipeline.reset();
        m_compositeRenderPipeline.reset();
        m_offScreenRenderPass.reset();
        m_blurRenderPass.reset();
        m_compositeRenderPass.reset();

        m_vertexBuffers.clear();
        m_indexBuffers.clear();
        m_uniformBuffers.clear();
    }
}

}  // namespace MMM::Graphic
