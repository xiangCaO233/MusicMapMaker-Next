#include "graphic/vk/VKRenderPass.h"
#include "log/colorful-log.h"
#include "graphic/vk/VKSwapchain.h"

namespace MMM
{
namespace Graphic
{

/**
 * @brief 构造函数
 * @param logicalDevice Vulkan 逻辑设备引用
 * @param swapchain 交换链引用 (用于获取图像格式)
 */
VKRenderPass::VKRenderPass(vk::Device& logicalDevice, VKSwapchain& swapchain)
    : m_logicalDevice(logicalDevice)
{
    // 1:创建渲染流程
    vk::RenderPassCreateInfo renderPassCreateInfo;
    // 1.1:附件描述
    vk::AttachmentDescription attachmentDescription;
    attachmentDescription
        // 图像格式用swapchain中的
        .setFormat(swapchain.info().imageFormat)
        // 附件进入时的布局 - 暂不关心
        .setInitialLayout(vk::ImageLayout::eUndefined)
        // 附件输出时的布局 - 呈现附件
        .setFinalLayout(vk::ImageLayout::ePresentSrcKHR)
        // 附件加载时需要的操作 - 加载时清空
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        // 附件保存时需要的操作 - 正常存储
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        // 3d绘制时需要的深度模板操作
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
        .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        // 设置对纹理进行n重采样 - 单重就行
        .setSamples(vk::SampleCountFlagBits::e1);
    renderPassCreateInfo.setAttachments(attachmentDescription);

    // 1.2:子流程
    vk::SubpassDescription subpassDescription;
    // 1.2.1:附件引用
    vk::AttachmentReference attachmentReference;
    attachmentReference
        // 布局沿用输出布局
        .setLayout(vk::ImageLayout::eColorAttachmentOptimal)
        // 设置引用的renderpass中附件的索引
        .setAttachment(0);
    subpassDescription
        // 绑定作用到图形管线上
        .setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
        // 设置颜色附件
        .setColorAttachments(attachmentReference);
    renderPassCreateInfo.setSubpasses(subpassDescription);

    // 1.3:流程依赖
    // vulkan 存在一个隐含的 initsubpass,
    // 还是需要手动指定这个initsubpass和创建的subpass的依赖关系
    vk::SubpassDependency subpassDependency;
    subpassDependency
        // 先执行 VK_SUBPASS_EXTERNAL subpass
        .setSrcSubpass(VK_SUBPASS_EXTERNAL)
        // 后执行 指定上面设置的setSubpasses 的[索引]
        .setDstSubpass(0)
        // 对subpass的访问权限设置 - 给setDstSubpass的subpass写权限
        .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
        // 设置指定subpass执行完后的使用场景 - 输出颜色附件
        .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
        .setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput);

    renderPassCreateInfo.setDependencies(subpassDependency);

    // 2:创建renderpass
    m_graphicRenderPass = logicalDevice.createRenderPass(renderPassCreateInfo);
    XINFO("Created VK Graphics RenderPipeline RenderPass.");
}

VKRenderPass::~VKRenderPass()
{
    // 销毁图形渲染流程
    m_logicalDevice.destroyRenderPass(m_graphicRenderPass);
    XINFO("Destroyed VK Graphics RenderPipeline RenderPass.");
}

}  // namespace Graphic

}  // namespace MMM
