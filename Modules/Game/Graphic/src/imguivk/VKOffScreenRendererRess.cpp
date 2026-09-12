#include "config/skin/SkinConfig.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKTexture.h"
#include "graphic/imguivk/mesh/VKBasicVertex.h"
#include "imgui_impl_vulkan.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

namespace MMM::Graphic
{

/// @brief 用一次性命令把颜色图像转换到离屏渲染所需的初始布局。
///
/// 该辅助函数只服务于资源创建阶段。它为指定图像分配临时 primary command
/// buffer，记录单个 image barrier，提交后等待队列空闲，再释放临时命令缓冲。
/// 未显式传入 image 时使用主离屏图像 m_image。
///
/// @param commandPool 可分配一次性 primary command buffer 的命令池。
/// @param queue 与命令池队列族兼容、负责执行布局转换的图形队列。
/// @param oldLayout 创建者当前保证的源布局。
/// @param newLayout 首次 render pass 或 descriptor 读取前要求的目标布局。
/// @param image 要转换的颜色图像；空句柄表示主离屏图像。
/// @warning 低频资源创建路径：内部同步等待 queue idle，禁止从每帧录制或 UI
/// 更新热路径调用。
void VKOffScreenRenderer::transitionImageInternal(vk::CommandPool commandPool,
                                                  vk::Queue       queue,
                                                  vk::ImageLayout oldLayout,
                                                  vk::ImageLayout newLayout,
                                                  vk::Image       image)
{
    // 临时命令缓冲只承载一次布局转换，eOneTimeSubmit 允许驱动按单次提交优化。
    vk::CommandBufferAllocateInfo allocInfo;
    allocInfo.setCommandPool(commandPool)
        .setLevel(vk::CommandBufferLevel::ePrimary)
        .setCommandBufferCount(1);

    // m_device 已由 reCreateFrameBuffer 绑定；资源重建负责保证命令池属于同一
    // logical device。
    vk::CommandBuffer cmd = m_device.allocateCommandBuffers(allocInfo).value[0];

    vk::CommandBufferBeginInfo beginInfo;
    beginInfo.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    (void)cmd.begin(beginInfo);

    // 离屏缓冲均为单 mip、单 array layer 的颜色图像，因此 barrier
    // 覆盖完整资源。
    vk::ImageMemoryBarrier barrier;
    barrier.setOldLayout(oldLayout)
        .setNewLayout(newLayout)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        // 可选句柄使主图像与 glow/ping/pong 图像复用同一转换实现。
        .setImage(image ? image : m_image)
        .setSubresourceRange({ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 });

    // access mask 与 stage 必须成对描述生产者和消费者，不能只改变 layout。
    vk::PipelineStageFlags srcStage;
    vk::PipelineStageFlags dstStage;

    // 核心修正：针对 Intel 核显驱动，必须显式指定 AccessMask 和 StageMask
    // 的同步关系
    if ( oldLayout == vk::ImageLayout::eUndefined &&
         newLayout == vk::ImageLayout::eShaderReadOnlyOptimal ) {
        // 新图像没有需要保留的旧内容，首次消费者是 fragment shader 采样读取。
        barrier.setSrcAccessMask(vk::AccessFlagBits::eNone)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
        srcStage = vk::PipelineStageFlagBits::eTopOfPipe;
        dstStage = vk::PipelineStageFlagBits::eFragmentShader;
    } else if ( oldLayout == vk::ImageLayout::eUndefined &&
                newLayout == vk::ImageLayout::eColorAttachmentOptimal ) {
        // 首次作为颜色附件时只需等待 color output 阶段取得写权限。
        barrier.setSrcAccessMask(vk::AccessFlagBits::eNone)
            .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite);
        srcStage = vk::PipelineStageFlagBits::eTopOfPipe;
        dstStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    } else {
        // 未专门建模的布局组合采用保守的全命令读写依赖，保证正确性优先。
        // 新增高频转换组合时应扩展精确分支，而不是长期依赖该宽泛兜底。
        barrier
            .setSrcAccessMask(vk::AccessFlagBits::eMemoryRead |
                              vk::AccessFlagBits::eMemoryWrite)
            .setDstAccessMask(vk::AccessFlagBits::eMemoryRead |
                              vk::AccessFlagBits::eMemoryWrite);
        srcStage = vk::PipelineStageFlagBits::eAllCommands;
        dstStage = vk::PipelineStageFlagBits::eAllCommands;
    }

    // barrier 只描述图像依赖，不携带 memory 或 buffer barrier。
    cmd.pipelineBarrier(srcStage, dstStage, {}, nullptr, nullptr, barrier);

    (void)cmd.end();

    // 提交不设置 semaphore/fence，因为下方 queue.waitIdle 直接建立同步边界。
    vk::SubmitInfo submitInfo;
    submitInfo.setCommandBuffers(cmd);
    (void)queue.submit(submitInfo);
    // 创建流程随后立即创建 descriptor 或 framebuffer 并使用图像，必须等待转换
    // 完成；此阻塞只允许出现在尺寸/皮肤触发的低频重建阶段。
    (void)queue.waitIdle();

    // 队列空闲后命令缓冲已不再在途，可以安全归还调用方提供的命令池。
    m_device.freeCommandBuffers(commandPool, cmd);
}

/// @brief 创建一组可作为颜色附件和采样源的离屏效果资源。
///
/// 函数按 image、device-local memory、image view、共享 sampler、framebuffer 的
/// 依赖顺序建立资源，并把图像预转换为 shader-read 布局。传入的输出句柄由当前
/// VKOffScreenRenderer 独占，最终统一由 releaseResources 销毁。
///
/// @param phyDevice 用于查询 device-local memory type 的物理设备。
/// @param logicalDevice 创建并绑定 Vulkan 对象的逻辑设备。
/// @param swapchain 保留与离屏 render pass 构造接口一致的交换链上下文。
/// @param commandPool 用于录制初始布局转换的一次性命令池。
/// @param queue 执行并同步初始布局转换的图形队列。
/// @param width 效果目标的物理像素宽度，调用方保证大于零。
/// @param height 效果目标的物理像素高度，调用方保证大于零。
/// @param image 接收新建图像句柄。
/// @param memory 接收绑定到图像的 device-local memory。
/// @param imageView 接收覆盖完整颜色子资源的图像视图。
/// @param framebuffer 接收与 pass 兼容的单附件 framebuffer。
/// @param sampler 接收或复用线性 clamp sampler。
/// @param pass framebuffer 所属 render pass，调用方保证非空且格式兼容。
/// @warning 低频资源重建路径：包含显存分配、Vulkan 对象创建与同步等待，不得从
/// 每帧渲染热路径调用。
void VKOffScreenRenderer::createOffscreenBuffer(
    vk::PhysicalDevice& phyDevice, vk::Device& logicalDevice,
    VKSwapchain& swapchain, vk::CommandPool commandPool, vk::Queue queue,
    uint32_t width, uint32_t height, vk::Image& image, vk::DeviceMemory& memory,
    vk::ImageView& imageView, vk::Framebuffer& framebuffer,
    vk::Sampler& sampler, VKRenderPass* pass)
{
    // 三个发光缓冲统一使用无 mip 的 RGBA8 格式，便于在 pass 间交替采样和写入。
    vk::ImageCreateInfo imageInfo;
    imageInfo.setImageType(vk::ImageType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Unorm)  // 修正：强制使用 R8G8B8A8Unorm
        .setExtent({ width, height, 1 })
        .setMipLevels(1)
        .setArrayLayers(1)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal)
        // ColorAttachment 用于遮罩/模糊输出，Sampled 用于下一阶段读取；transfer
        // 标志为驱动兼容与诊断拷贝保留，不改变常规后处理路径。
        .setUsage(vk::ImageUsageFlagBits::eColorAttachment |
                  vk::ImageUsageFlagBits::eSampled |
                  vk::ImageUsageFlagBits::eTransferSrc |
                  vk::ImageUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);

    // 创建成功后的 image 仍未绑定内存，禁止在 bindImageMemory 前创建实际访问。
    image = logicalDevice.createImage(imageInfo).value;

    vk::MemoryRequirements memRequirements =
        logicalDevice.getImageMemoryRequirements(image);

    // 仅选择同时被图像 requirements 接受且具备全部请求属性的 memory type。
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
        // 现有无异常构建接口以索引零兜底；调用平台必须提供可用的 device-local
        // 类型，否则后续分配/绑定结果会暴露初始化失败。
        return 0;
    };
    vk::MemoryAllocateInfo allocInfo;
    allocInfo.setAllocationSize(memRequirements.size)
        .setMemoryTypeIndex(
            findMemoryType(memRequirements.memoryTypeBits,
                           vk::MemoryPropertyFlagBits::eDeviceLocal));

    // 图像与显存保持一对一所有权，释放时必须先销毁 view/framebuffer/image。
    memory = logicalDevice.allocateMemory(allocInfo).value;
    (void)logicalDevice.bindImageMemory(image, memory, 0);

    // 后处理 shader 和颜色附件都通过同一个 2D view 访问唯一 mip/layer。
    vk::ImageViewCreateInfo viewInfo;
    viewInfo.setImage(image)
        .setViewType(vk::ImageViewType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Unorm)
        .setSubresourceRange(vk::ImageSubresourceRange(
            vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

    imageView = logicalDevice.createImageView(viewInfo).value;

    // glow、ping、pong 传入同一 sampler
    // 句柄；只在首次调用时创建，避免重复对象。
    if ( !sampler ) {
        vk::SamplerCreateInfo samplerInfo;
        // 低分辨率效果纹理放大到主画布时需要线性采样，边缘钳制防止模糊核从
        // 图像另一侧回绕取样。
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

    // framebuffer 与传入 pass 的单颜色附件声明保持相同格式和样本数。
    vk::FramebufferCreateInfo framebufferInfo;
    framebufferInfo.setRenderPass(pass->getRenderPass())
        .setAttachments(imageView)
        .setWidth(width)
        .setHeight(height)
        .setLayers(1);

    framebuffer = logicalDevice.createFramebuffer(framebufferInfo).value;

    // descriptor 始终声明
    // ShaderReadOnlyOptimal；创建结束前先建立匹配的实际布局。
    transitionImageInternal(commandPool,
                            queue,
                            vk::ImageLayout::eUndefined,
                            vk::ImageLayout::eShaderReadOnlyOptimal,
                            image);
}

/// @brief 按最新目标尺寸重建主离屏画布及完整发光后处理资源图。
///
/// 重建覆盖 render pass、shader module、pipeline、主颜色目标、glow/ping/pong
/// 目标、ImGui texture descriptor、per-frame 几何缓冲和内部 descriptor sets。
/// 所有 Vulkan 资源在设备空闲后整体替换，成功末尾才提交尺寸并清除重建脏位。
///
/// @param phyDevice 资源分配及 memory type 查询使用的物理设备。
/// @param logicalDevice 创建本轮全部 Vulkan 对象的逻辑设备。
/// @param swapchain 为 render pass 和 pipeline 提供兼容格式及 extent 上下文。
/// @param commandPool 用于纹理上传和初始图像布局转换的命令池。
/// @param queue 与 commandPool 队列族兼容的图形队列。
/// @param shaderModulePath 保留的 shader 模块路径入口；当前 shader 由派生类回调
/// 提供，此参数不参与资源选择。
/// @param maxVertexCount 初始 per-frame 顶点/索引元素容量高水位。
/// @warning 低频资源重建路径：会 waitIdle、分配显存、创建 pipeline 并更新
/// descriptor。只能在尺寸消抖或皮肤资源变更确认后调用。
void VKOffScreenRenderer::reCreateFrameBuffer(
    vk::PhysicalDevice& phyDevice, vk::Device& logicalDevice,
    VKSwapchain& swapchain, vk::CommandPool commandPool, vk::Queue queue,
    const std::filesystem::path& shaderModulePath, size_t maxVertexCount)
{
    // 调用点与资源层双重检查重建脏位和消抖窗口，避免过期请求重复销毁资源。
    if ( !needReCreateFrameBuffer() ) return;

    // Vulkan image/framebuffer extent 不能为零；窗口最小化时保留脏位，待恢复
    // 到有效尺寸后再重建。
    if ( m_targetWidth == 0 || m_targetHeight == 0 ) return;

    // 旧资源可能仍被已提交的离屏命令或 ImGui descriptor 引用，销毁前必须等待
    // 当前绑定设备空闲；首次创建时 m_device 为空，直接跳过。
    if ( m_device ) {
        (void)m_device.waitIdle();
    }

    // releaseResources 先从 ImGui 注销纹理，再按依赖顺序释放旧 Vulkan 对象。
    releaseResources();

    // 后续辅助函数统一使用成员 device；物理设备按值保存非 owning 句柄。
    m_device         = logicalDevice;
    m_physicalDevice = phyDevice;

    // 本轮复制一次目标尺寸，保证重建期间使用一致 extent；新的 resize 请求仍会
    // 重新置脏并触发下一轮重建。
    uint32_t creationW = m_targetWidth;
    uint32_t creationH = m_targetHeight;
    // 发光目标允许按皮肤配置降采样以控制模糊成本，并把异常配置钳制到受支持范围。
    const float glowResolutionScale = std::clamp(
        Config::SkinManager::instance().getValue("glow.resolution_scale", 0.5f),
        0.125f,
        1.0f);
    // 极小主画布也必须产生至少 1x1 的合法 Vulkan 图像。
    const uint32_t glowCreationW = std::max<uint32_t>(
        1, static_cast<uint32_t>(std::ceil(creationW * glowResolutionScale)));
    const uint32_t glowCreationH = std::max<uint32_t>(
        1, static_cast<uint32_t>(std::ceil(creationH * glowResolutionScale)));

    // 主 pass 清除透明目标；结束时转换为 shader-read，供 ImGui 或后处理采样。
    m_offScreenRenderPass =
        std::make_unique<VKRenderPass>(logicalDevice,
                                       swapchain,
                                       vk::ImageLayout::eShaderReadOnlyOptimal,
                                       true,
                                       vk::Format::eR8G8B8A8Unorm);

    // Composite pass 使用 load 语义保留主颜色，在其上叠加 glow 或覆盖批次。
    m_compositeRenderPass =
        std::make_unique<VKRenderPass>(logicalDevice,
                                       swapchain,
                                       vk::ImageLayout::eShaderReadOnlyOptimal,
                                       false,
                                       vk::Format::eR8G8B8A8Unorm);

    // 模糊 pass 同样不清除旧附件，因为每轮全屏三角形覆盖完整目标区域。
    m_blurRenderPass =
        std::make_unique<VKRenderPass>(logicalDevice,
                                       swapchain,
                                       vk::ImageLayout::eShaderReadOnlyOptimal,
                                       false,
                                       vk::Format::eR8G8B8A8Unorm);

    // shader 必须先于 pipeline 创建，映射中的 owning pointer
    // 贯穿本轮资源生命期。
    // 主模块创建失败会使后续必要管线无法建立，因此派生实现必须始终提供至少
    // vertex 与 fragment 两阶段；effect 模块才允许缺省。
    createShaderModules();

    // 画笔管线复用主渲染器的纹理 descriptor layout，使派生批次可直接绑定自身
    // 纹理 descriptor，而无需为离屏视图维护第二套布局协议。
    auto& renderer     = VKContext::get().value().get().getRenderer();
    auto  sharedLayout = renderer.getBrushTextureLayout();

    // 主管线负责普通 2DCanvas 批次，启用动态 viewport/scissor 与透明混合。
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

    // 加法管线与主管线共用 shader、render pass、descriptor layout 和顶点格式；
    // DrawCmd 只需切换 pipeline 即可表达加法材质。
    m_additiveBrushRenderPipeline =
        std::make_unique<VKRenderPipeline>(logicalDevice,
                                           *m_vkShaders[getShaderName("main")],
                                           *m_offScreenRenderPass,
                                           swapchain,
                                           true,
                                           0,
                                           0,
                                           false,
                                           true,
                                           sharedLayout,
                                           true,
                                           true);

    // Glow 遮罩仍走画笔顶点输入，仅把匹配批次渲染到低分辨率 glow framebuffer。
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
        // TimelineCanvas 等视图可能让 effect 名称回退到主
        // shader；只有源码集合与 main 不同，才按 gl_VertexIndex 全屏效果 shader
        // 创建无顶点输入管线。
        auto mainSources   = getShaderSources("main");
        auto effectSources = getShaderSources("effect");
        bool effectIsFullscreen =
            !effectSources.empty() && (effectSources != mainSources);

        // Blur 关闭 blending，确保每轮输出只包含当前采样结果而非历史附件颜色。
        m_blurRenderPipeline = std::make_unique<VKRenderPipeline>(
            logicalDevice,
            *m_vkShaders[getShaderName("effect")],
            *m_blurRenderPass,  // 使用不 Clear 的 RenderPass
            swapchain,
            true,
            0,
            0,
            false,
            false,                               // Blur: blendEnable=false
            effectIsFullscreen ? VK_NULL_HANDLE  // 全屏效果自建布局
                               : sharedLayout,
            !effectIsFullscreen);  // 全屏效果不需要顶点输入

        // Composite 开启加法 blending，把最终模糊结果叠加到已经完成的主图像。
        m_compositeRenderPipeline = std::make_unique<VKRenderPipeline>(
            logicalDevice,
            *m_vkShaders[getShaderName("effect")],
            *m_compositeRenderPass,
            swapchain,
            true,
            0,
            0,
            true,
            true,  // Composite: additive blend
            effectIsFullscreen ? VK_NULL_HANDLE : sharedLayout,
            !effectIsFullscreen);  // 全屏效果不需要顶点输入
    }

    // 主颜色图像与所有 pass 固定使用 RGBA8；格式一致是 framebuffer 和 pipeline
    // 兼容性的前提，不能只在其中一处随交换链格式变化。
    vk::ImageCreateInfo imageInfo;
    imageInfo.setImageType(vk::ImageType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Unorm)  // 修正：强制使用 R8G8B8A8Unorm
        .setExtent(vk::Extent3D{ creationW, creationH, 1 })
        .setMipLevels(1)
        .setArrayLayers(1)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal)
        // 主图像既是颜色附件也是 ImGui 采样源；transfer 能力为驱动兼容和后续
        // 诊断拷贝保留，不要求常规帧额外执行传输。
        .setUsage(vk::ImageUsageFlagBits::eColorAttachment |
                  vk::ImageUsageFlagBits::eSampled |
                  vk::ImageUsageFlagBits::eTransferSrc |
                  vk::ImageUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);

    // image 创建与 memory 分配分离，所有依赖对象都在 memory 成功绑定后创建。
    m_image = m_device.createImage(imageInfo).value;

    // 主离屏图像只由 GPU 渲染和采样，使用 device-local memory，CPU 不映射访问。
    vk::MemoryRequirements memRequirements =
        m_device.getImageMemoryRequirements(m_image);

    // memoryTypeBits 是图像实现允许的候选集合，properties 再筛选所需设备属性。
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
        // 记录不可满足的平台能力；当前无异常接口仍返回索引零，让 Vulkan 结果在
        // 初始化阶段统一体现失败，而不是从热路径继续尝试分配。
        XCRITICAL("Failed to find suitable memory type for OffScreen Image!");
        return 0;
    };
    vk::MemoryAllocateInfo allocInfo;
    allocInfo.setAllocationSize(memRequirements.size)
        .setMemoryTypeIndex(
            findMemoryType(memRequirements.memoryTypeBits,
                           vk::MemoryPropertyFlagBits::eDeviceLocal));

    // 使用 requirements.size 覆盖驱动要求的对齐与潜在 padding。
    m_imageMemory = m_device.allocateMemory(allocInfo).value;
    // offset 为零且该 allocation 只归属于主图像，释放顺序由 releaseResources
    // 保证。
    (void)m_device.bindImageMemory(m_image, m_imageMemory, 0);

    // 单层 2D view 同时提供给 framebuffer attachment 和 ImGui descriptor。
    vk::ImageViewCreateInfo viewInfo;
    viewInfo.setImage(m_image)
        .setViewType(vk::ImageViewType::e2D)
        .setFormat(vk::Format::eR8G8B8A8Unorm)  // 修正：修正格式一致性
        .setSubresourceRange(vk::ImageSubresourceRange(
            vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

    m_imageView = m_device.createImageView(viewInfo).value;

    // 主图像 sampler 与发光 sampler 分离：主画布要求像素精确，效果缓冲需要线性
    // 放大，不能为节省对象而共享采样参数。
    vk::SamplerCreateInfo samplerInfo;
    // 主离屏画布与 ImGui 目标按物理像素 1:1 映射；最终提交再次执行线性
    // 插值会把已经抗锯齿的字形边缘重复滤波，导致画布文字明显发糊。
    samplerInfo.setMagFilter(vk::Filter::eNearest)
        .setMinFilter(vk::Filter::eNearest)
        .setAddressModeU(vk::SamplerAddressMode::eClampToEdge)
        .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
        .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
        .setAnisotropyEnable(VK_FALSE)
        .setBorderColor(vk::BorderColor::eIntOpaqueBlack)
        .setUnnormalizedCoordinates(VK_FALSE)
        .setCompareEnable(VK_FALSE)
        .setCompareOp(vk::CompareOp::eAlways)
        .setMipmapMode(vk::SamplerMipmapMode::eNearest);

    m_sampler = m_device.createSampler(samplerInfo).value;

    // framebuffer 只引用 image view，不拥有它；销毁时必须先 framebuffer 后
    // view。
    vk::FramebufferCreateInfo framebufferInfo;
    framebufferInfo.setRenderPass(m_offScreenRenderPass->getRenderPass())
        .setAttachments(m_imageView)
        .setWidth(creationW)
        .setHeight(creationH)
        .setLayers(1);

    m_framebuffer = m_device.createFramebuffer(framebufferInfo).value;

    // 刚创建的 Image 是 Undefined。虽然 RenderPass 会负责转换，
    // 但 ImGui descriptor 从注册起就声明 ShaderReadOnlyOptimal，因此在暴露
    // descriptor 前显式建立一致的初始状态。
    {
        transitionImageInternal(commandPool,
                                queue,
                                vk::ImageLayout::eUndefined,
                                vk::ImageLayout::eShaderReadOnlyOptimal,
                                m_image);
    }

    // 发光链固定使用三个等尺寸目标：glow 保存原始遮罩，ping/pong 交替保存模糊
    // 输出。三者共用线性 sampler，但各自拥有 image、memory、view、framebuffer。
    createOffscreenBuffer(phyDevice,
                          logicalDevice,
                          swapchain,
                          commandPool,
                          queue,
                          glowCreationW,
                          glowCreationH,
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
                          glowCreationW,
                          glowCreationH,
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
                          glowCreationW,
                          glowCreationH,
                          m_pongImage,
                          m_pongImageMemory,
                          m_pongImageView,
                          m_pongFramebuffer,
                          m_glowSampler,
                          m_offScreenRenderPass.get());

    // ImGui backend 为主画布和原始 glow 遮罩各分配一个纹理 descriptor；句柄
    // 必须在内部 descriptor pool 或图像视图销毁前通过 RemoveTexture 注销。
    m_imguiDescriptor = (vk::DescriptorSet)ImGui_ImplVulkan_AddTexture(
        (VkSampler)m_sampler,
        (VkImageView)m_imageView,
        // recordCmds 的 render pass final layout 与该 descriptor 声明保持一致。
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    m_imguiGlowDescriptor = (vk::DescriptorSet)ImGui_ImplVulkan_AddTexture(
        (VkSampler)m_glowSampler,
        (VkImageView)m_glowImageView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // 初始缓冲按 maxVertexCount 建立高水位；recordCmds 发现实际几何超过容量时会
    // 同步扩容。索引缓冲复用同一字节数，VKBasicVertex 大于 uint32_t
    // 时有足够空间。
    size_t bufferSize = sizeof(Vertex::VKBasicVertex) * maxVertexCount;

    // releaseResources 已清空旧 owning pointers，此处再次清空保持重建函数可独立
    // 维持容器后置条件。
    m_vertexBuffers.clear();
    m_indexBuffers.clear();
    m_uniformBuffers.clear();

    for ( int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
        // 每个 in-flight frame 独占 CPU 可见且 coherent 的上传缓冲，避免 CPU
        // 写入 覆盖 GPU 仍在消费的另一帧几何。
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

        // uniform 槽保留现有 descriptor 协议，即使当前上传钩子不写入业务数据。
        m_uniformBuffers.push_back(std::make_unique<VKMemBuffer>(
            phyDevice,
            m_device,
            sizeof(float),
            vk::BufferUsageFlagBits::eUniformBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
    }

    // 四套纹理 descriptor 都按并发帧数分槽；先建池和集合，待白纹理存在后统一
    // 写入实际 image/sampler 绑定。
    createDescriptPool();
    createDescriptSets();

    // 只有核心资源全部创建完成后才发布实际尺寸，recordCmds 据此设置
    // renderArea。
    m_width      = creationW;
    m_height     = creationH;
    m_glowWidth  = glowCreationW;
    m_glowHeight = glowCreationH;

    // 容量值必须与本轮 bufferSize 对应；清脏使用 relaxed 即可，因为 Vulkan 资源
    // 可见性由调用线程和外层渲染同步保证，原子只传递重建请求状态。
    // 先发布尺寸和容量、最后清除脏位，保证下一次 recordCmds 观察到的成员属于
    // 同一轮完整资源集合。
    m_lastAllocatedCount = maxVertexCount;
    m_need_reCreate.store(false, std::memory_order_relaxed);

    XDEBUG(
        "VKOffScreenRenderer recreate successfully[{}x{}]", m_width, m_height);

    if ( !m_whiteTexture ) {
        // 白纹理为没有显式材质的画笔批次提供乘色基底；它与 framebuffer 尺寸
        // 无关，因此跨重建复用并由成员 unique_ptr 管理。
        unsigned char whitePixel[] = { 255, 255, 255, 255 };

        // VKTexture 构造会使用当前 commandPool/queue
        // 完成一次性上传，调用点仍处于 已允许同步和资源创建的低频重建阶段。
        m_whiteTexture = std::make_unique<VKTexture>(
            whitePixel, 1, 1, phyDevice, m_device, commandPool, queue);
    }

    // 白纹理和三张效果图像现已全部存在，可以一次性提交所有 descriptor writes。
    updateDescriptorSets();
}
/// @brief 创建容纳全部并发帧采样集合的私有 descriptor pool。
///
/// 每帧预留 offscreen、glow、ping、pong 四个 set。池同时声明 uniform 与
/// combined image sampler 配额，以保持与共享画笔 descriptor layout
/// 的资源类型兼容。 该 pool 及由其分配的所有 set 由 releaseResources 整体销毁。
///
/// @warning 低频资源创建路径：调用 Vulkan 对象分配，只能在 framebuffer 重建时
/// 执行。
void VKOffScreenRenderer::createDescriptPool()
{
    // 每个 frame slot 固定对应四种采样源，集合总数与 recordCmds
    // 的索引协议一致。
    uint32_t totalSets = 4 * MAX_FRAMES_IN_FLIGHT;

    // 即使当前 writes 仅更新 combined sampler，也为共享布局中的 uniform binding
    // 预留同等数量，避免 pool 类型预算与 layout 声明脱节。
    vk::DescriptorPoolSize uniformPoolSize(vk::DescriptorType::eUniformBuffer,
                                           totalSets);
    vk::DescriptorPoolSize samplerPoolSize(
        vk::DescriptorType::eCombinedImageSampler, totalSets);
    std::array<vk::DescriptorPoolSize, 2> poolSizes{ uniformPoolSize,
                                                     samplerPoolSize };
    // Pool 不启用 FREE_DESCRIPTOR_SET_BIT；重建时统一销毁 pool 比逐 set
    // 释放更简单。
    vk::DescriptorPoolCreateInfo poolInfo(
        {}, totalSets, (uint32_t)poolSizes.size(), poolSizes.data());
    m_descriptorPool = m_device.createDescriptorPool(poolInfo).value;
}

/// @brief 从私有 pool 为每个并发帧分配四类纹理 descriptor set。
///
/// 所有集合使用主渲染器公开的画笔纹理布局，使普通画笔、发光遮罩和全屏效果可
/// 沿用一致的 set 编号与 binding 协议。这里只分配句柄，具体图像绑定由
/// updateDescriptorSets 在资源齐备后统一写入。
///
/// @warning 低频资源创建路径：会分配 descriptor set，不得从每帧命令录制调用。
void VKOffScreenRenderer::createDescriptSets()
{
    // layout 由 VKRenderer 拥有，当前类只借用句柄进行分配，不负责销毁。
    auto& renderer     = VKContext::get().value().get().getRenderer();
    auto  sharedLayout = renderer.getBrushTextureLayout();

    // 四个 vector 必须保持相同长度，recordCmds 用同一个 frameIndex 并行索引。
    m_offScreenDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    m_glowDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    m_pingDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    m_pongDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);

    for ( int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
        // 每次 allocation 只请求一个 set，便于直接把返回句柄放入对应帧槽。
        vk::DescriptorSetAllocateInfo allocInfo(
            m_descriptorPool, 1, &sharedLayout);
        // Offscreen 集合采样白纹理，供没有自定义纹理的普通画笔批次使用。
        m_offScreenDescriptorSets[i] =
            m_device.allocateDescriptorSets(allocInfo).value[0];
        // 其余三组分别作为 blur 首轮输入以及 ping-pong 后续输入。
        m_glowDescriptorSets[i] =
            m_device.allocateDescriptorSets(allocInfo).value[0];
        m_pingDescriptorSets[i] =
            m_device.allocateDescriptorSets(allocInfo).value[0];
        m_pongDescriptorSets[i] =
            m_device.allocateDescriptorSets(allocInfo).value[0];
    }
}

/// @brief 把默认白纹理及 glow/ping/pong 图像写入全部 descriptor set。
///
/// 每类 descriptor set 的所有帧槽绑定同一只读图像，因为图像内容由外层命令
/// 顺序保护；按帧复制集合是为了与统一画笔布局和后续可扩展的 per-frame 资源协议
/// 保持一致。所有图像在调用前已转换为 ShaderReadOnlyOptimal。
///
/// @warning 低频资源重建路径：批量更新 Vulkan descriptor，图像视图和 sampler
/// 必须在 pool 生命周期内持续有效。
void VKOffScreenRenderer::updateDescriptorSets()
{
    // 默认集合绑定 1x1 白纹理，使顶点颜色不被额外纹理内容改变。
    vk::DescriptorImageInfo imageInfo;
    imageInfo.setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(m_whiteTexture->getImageView())  // 绑白色纹理视图
        .setSampler(m_whiteTexture->getSampler());     // 绑白色纹理采样器

    // Glow 是 blur 第一次迭代的原始遮罩输入。
    vk::DescriptorImageInfo glowInfo;
    glowInfo.setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(m_glowImageView)
        .setSampler(m_glowSampler);

    // Ping/Pong descriptor 始终指向固定图像，录制阶段只切换选择哪个 set。
    vk::DescriptorImageInfo pingInfo;
    pingInfo.setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(m_pingImageView)
        .setSampler(m_glowSampler);

    vk::DescriptorImageInfo pongInfo;
    pongInfo.setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(m_pongImageView)
        .setSampler(m_glowSampler);

    // vector 在低频重建路径一次性积累 4*N 个 write；update 调用返回后 Vulkan 已
    // 复制 descriptor 信息，不保留这些栈上 DescriptorImageInfo 指针。
    std::vector<vk::WriteDescriptorSet> writes;
    for ( int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
        // 所有写入都落在共享布局的 binding 0，类型必须与画笔 texture binding
        // 一致。
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
    // 当前没有 descriptor copy 操作，第二个参数保持空集合。
    m_device.updateDescriptorSets(writes, nullptr);
}

/// @brief 保留离屏 uniform 上传扩展点。
///
/// 当前投影和效果参数均通过 push constant 传递，函数保持为空以维持既有录制调用
/// 顺序及派生接口兼容性。若未来恢复 uniform 数据，必须只更新当前空闲帧槽，并与
/// descriptor binding 协议同时调整。
///
/// @warning 每帧命令录制热路径会调用；当前不得在此引入分配、文件访问或 GPU
/// 等待。
void VKOffScreenRenderer::uploadUniformBuffer2GPU() {}

/// @brief 根据派生视图提供的 SPIR-V 源集合创建一个 Vulkan shader 组合。
///
/// 源集合按 vertex、fragment、可选 geometry 的固定顺序解释。缺少必要阶段时返回
/// nullptr；超过三个阶段时忽略多余输入并记录告警。
///
/// @param main_shader_sources 各 shader 阶段的二进制源码字符串。
/// @param module_name 仅用于诊断当前逻辑模块名称。
/// @param logicalDevice 创建 shader module 的逻辑设备。
/// @return 成功时返回独占 VKShader；源为空或不足两个阶段时返回 nullptr。
/// @warning 低频资源创建路径：VKShader 构造会创建 Vulkan shader module，不得在
/// 每帧渲染热路径调用。
std::unique_ptr<VKShader> createShaderModule(
    std::vector<std::string> main_shader_sources, std::string module_name,
    vk::Device& logicalDevice)
{
    if ( main_shader_sources.empty() ) {
        // 空集合表示派生视图没有声明该逻辑模块，调用者可把 effect
        // 视为可选能力。
        return nullptr;
    }
    if ( main_shader_sources.size() < 2 ) {
        // 图形管线至少需要 vertex 和 fragment
        // 两阶段，单份源码无法组成有效管线。
        XWARN(
            "No enough Shader Sources defined in SkinConfig, at lease "
            "define "
            "VertexShader and FragmentShader",
            module_name);
        return nullptr;
    }

    std::unique_ptr<Graphic::VKShader> mainShader{ nullptr };
    if ( main_shader_sources.size() < 3 ) {
        // 两份源码走最常见的 vertex + fragment 构造，不启用 geometry 阶段。
        return std::make_unique<Graphic::VKShader>(
            logicalDevice, main_shader_sources[0], main_shader_sources[1]);

    } else {
        if ( main_shader_sources.size() > 3 ) {
            // 当前 VKShader 协议不表达第四阶段，多余输入不会参与 pipeline。
            XWARN("Extra Shader will not use in Your Canvas");
        }
        // 三份源码按 vertex、fragment、geometry 顺序交给 VKShader 管理。
        return std::make_unique<Graphic::VKShader>(logicalDevice,
                                                   main_shader_sources[0],
                                                   main_shader_sources[1],
                                                   main_shader_sources[2]);
    }
}

/// @brief 重建当前视图声明的主 shader 与可选效果 shader 映射。
///
/// 派生类以逻辑名称提供源码和全局唯一存储名称。主 shader 是普通画笔管线的必要
/// 输入；effect shader 创建失败时仅禁用 blur/composite 能力并记录告警。
/// 映射拥有所有 VKShader，必须晚于使用它们的 pipeline 销毁或由外层 idle 保证
/// 不再有在途引用。
///
/// @warning 低频资源重建路径：会创建 shader module 并可能读取派生缓存，不得在
/// 每帧命令录制期间调用。
void VKOffScreenRenderer::createShaderModules()
{
    // main 名称是派生画布必须实现的基础阶段集合。
    auto main_shader_sources = getShaderSources("main");
    // createShaderModule 负责验证最少阶段数并选择是否包含 geometry shader。
    auto main_shader_module =
        createShaderModule(main_shader_sources, "main", m_device);
    // 使用派生类的唯一名称作为 key，避免不同画布类型共享时发生命名碰撞。
    m_vkShaders.emplace(getShaderName("main"), std::move(main_shader_module));

    // effect 是可选模块；没有独立效果源码的画布仍可完成普通离屏渲染。
    auto effect_shader_sources = getShaderSources("effect");
    auto effect_shader_module =
        createShaderModule(effect_shader_sources, "effect", m_device);
    if ( effect_shader_module ) {
        // 只保存有效 owning pointer，reCreateFrameBuffer 据 key
        // 是否存在创建效果管线。
        m_vkShaders.emplace(getShaderName("effect"),
                            std::move(effect_shader_module));
    } else {
        // 缺少效果 shader 不阻止主画布创建，recordCmds 会按管线有效性跳过
        // glow。
        XWARN("No {} Shader define.", getShaderName("effect"));
    }
}

/// @brief 注销 UI 纹理并释放当前离屏渲染器持有的全部 Vulkan 资源。
///
/// 调用方必须先保证 device idle。释放顺序遵循外部 descriptor、内部 descriptor
/// pool、pipeline/shader、framebuffer、view/image/memory 的引用关系，并把所有
/// 可观察句柄和尺寸重置为空状态，使重复调用保持安全。
///
/// m_whiteTexture 与 framebuffer 尺寸无关，重建时刻意保留并复用；它最终随对象
/// 成员析构释放，而不是在每次 resize 中重复上传。
///
/// @warning 低频资源销毁路径：调用 ImGui Vulkan backend 和 Vulkan destroy API；
/// 不执行 waitIdle，调用者负责在进入函数前建立 GPU 空闲边界。
void VKOffScreenRenderer::releaseResources()
{
    // ImGui backend 可能从自己的 pool 管理纹理 set；必须在 image view 和
    // sampler 失效前注销，且清空句柄防止重复 RemoveTexture。
    if ( m_imguiDescriptor ) {
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)m_imguiDescriptor);
        m_imguiDescriptor = nullptr;
    }
    if ( m_imguiGlowDescriptor ) {
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)m_imguiGlowDescriptor);
        m_imguiGlowDescriptor = nullptr;
    }

    // 首次构造或已释放状态没有有效 device；所有 Vulkan destroy 都依赖该句柄。
    if ( m_device ) {
        // 函数自身不清空 m_device：同一对象重建时仍要继续使用已缓存的设备，最终
        // 对象析构后该非 owning 句柄随成员自然失效。
        // 销毁 pool 会隐式回收四组 descriptor sets，因此先清 pool 再清 vector
        // 句柄。
        if ( m_descriptorPool ) {
            m_device.destroyDescriptorPool(m_descriptorPool);
            m_descriptorPool = nullptr;
        }

        // shader module 只在 pipeline 创建期间参与构造；device idle
        // 后可先清映射。
        m_vkShaders.clear();

        // 主目标按 framebuffer -> sampler/view -> image -> memory
        // 的依赖逆序释放。
        m_device.destroyFramebuffer(m_framebuffer);
        m_device.destroySampler(m_sampler);
        m_device.destroyImageView(m_imageView);
        m_device.destroyImage(m_image);
        m_device.freeMemory(m_imageMemory);
        m_framebuffer = VK_NULL_HANDLE;
        m_sampler     = VK_NULL_HANDLE;
        m_imageView   = VK_NULL_HANDLE;
        m_image       = VK_NULL_HANDLE;
        m_imageMemory = VK_NULL_HANDLE;

        // 三个效果 framebuffer 先于其 image view 销毁，避免留下 attachment
        // 引用。
        if ( m_glowFramebuffer ) m_device.destroyFramebuffer(m_glowFramebuffer);
        if ( m_pingFramebuffer ) m_device.destroyFramebuffer(m_pingFramebuffer);
        if ( m_pongFramebuffer ) m_device.destroyFramebuffer(m_pongFramebuffer);
        // View 不拥有 image；全部 framebuffer 失效后再释放 view。
        if ( m_glowImageView ) m_device.destroyImageView(m_glowImageView);
        if ( m_pingImageView ) m_device.destroyImageView(m_pingImageView);
        if ( m_pongImageView ) m_device.destroyImageView(m_pongImageView);
        // Image 销毁后才能归还各自绑定的 device memory。
        if ( m_glowImage ) m_device.destroyImage(m_glowImage);
        if ( m_pingImage ) m_device.destroyImage(m_pingImage);
        if ( m_pongImage ) m_device.destroyImage(m_pongImage);
        if ( m_glowImageMemory ) m_device.freeMemory(m_glowImageMemory);
        if ( m_pingImageMemory ) m_device.freeMemory(m_pingImageMemory);
        if ( m_pongImageMemory ) m_device.freeMemory(m_pongImageMemory);
        // 三张效果图共享唯一 sampler，只销毁一次。
        if ( m_glowSampler ) m_device.destroySampler(m_glowSampler);

        // 对外可观察的效果句柄统一归零，重复 release 不会二次销毁。
        m_glowFramebuffer = m_pingFramebuffer = m_pongFramebuffer =
            VK_NULL_HANDLE;
        m_glowImageView = m_pingImageView = m_pongImageView = VK_NULL_HANDLE;
        m_glowImage = m_pingImage = m_pongImage = VK_NULL_HANDLE;
        m_glowImageMemory = m_pingImageMemory = m_pongImageMemory =
            VK_NULL_HANDLE;
        m_glowSampler = VK_NULL_HANDLE;
        // 尺寸和临时裁剪倍率不再描述有效资源，随句柄一起回到初始状态。
        m_glowWidth = m_glowHeight = 0;
        m_scissorScaleX = m_scissorScaleY = 0.0f;

        // owning wrappers 在 device 仍有效时析构其 pipeline/render pass 句柄。
        m_additiveBrushRenderPipeline.reset();
        m_mainBrushRenderPipeline.reset();
        m_glowBrushRenderPipeline.reset();
        m_blurRenderPipeline.reset();
        m_compositeRenderPipeline.reset();
        m_offScreenRenderPass.reset();
        m_blurRenderPass.reset();
        m_compositeRenderPass.reset();

        // Per-frame buffers 及 descriptor 句柄容器按组清空，保持索引长度一致。
        m_vertexBuffers.clear();
        m_indexBuffers.clear();
        m_uniformBuffers.clear();
        m_offScreenDescriptorSets.clear();
        m_pingDescriptorSets.clear();
        m_pongDescriptorSets.clear();
        m_glowDescriptorSets.clear();
        // 下一次创建必须重新建立真实容量，不能沿用已释放缓冲的高水位。
        m_lastAllocatedCount = 0;
    }
}

}  // namespace MMM::Graphic
