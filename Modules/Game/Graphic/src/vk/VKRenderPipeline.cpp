#include "graphic/vk/VKRenderPipeline.h"
#include "log/colorful-log.h"
#include "graphic/vk/mesh/VKVertex.h"
#include <cassert>

namespace MMM
{
namespace Graphic
{

/**
 * @brief 构造函数，创建图形管线
 *
 * @param logicalDevice 逻辑设备引用
 * @param shader 着色器管理器引用 (提供 Shader Stages)
 * @param renderPass 渲染流程引用 (提供附件格式兼容性)
 * @param swapchain 交换链引用
 * @param w 视口宽度
 * @param h 视口高度
 */
VKRenderPipeline::VKRenderPipeline(vk::Device& logicalDevice, VKShader& shader,
                                   VKRenderPass& renderPass,
                                   VKSwapchain& swapchain, int w, int h)
    : m_logicalDevice(logicalDevice)
{
    // 3:创建渲染管线布局
    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
    m_graphicsPipelineLayout =
        logicalDevice.createPipelineLayout(pipelineLayoutCreateInfo);
    XINFO("Created VK Graphics RenderPipeline Layout.");

    // 4:图形管线创建信息
    vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo;

    // 4.1:顶点输入状态创建信息
    vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo;
    // 设置顶点输入属性描述信息
    pipelineVertexInputStateCreateInfo
        .setVertexBindingDescriptions(Graphic::VKVERTEX_BIND_DESC)
        .setVertexAttributeDescriptions(Graphic::VKVERTEX_ATTR_DESC);
    graphicsPipelineCreateInfo.setPVertexInputState(
        &pipelineVertexInputStateCreateInfo);

    // 4.2:顶点装配状态创建信息
    vk::PipelineInputAssemblyStateCreateInfo
        pipelineInputAssemblyStateCreateInfo;
    pipelineInputAssemblyStateCreateInfo
        // 图元重置 - 不启用
        .setPrimitiveRestartEnable(false)
        // 图元装配方式 - 使用点集方便直接几何着色
        // .setTopology(vk::PrimitiveTopology::ePointList);
        // 测试着色器使用三角形列表
        .setTopology(vk::PrimitiveTopology::eTriangleList);
    graphicsPipelineCreateInfo.setPInputAssemblyState(
        &pipelineInputAssemblyStateCreateInfo);

    // 4.3:着色器配置
    auto stages = shader.getShaderStageCreateInfos();
    graphicsPipelineCreateInfo.setStages(stages);

    // 4.4.视口配置
    vk::PipelineViewportStateCreateInfo pipelineViewportStateCreateInfo;
    // 单纯绘制窗口区域 - 无裁切
    vk::Viewport viewPort{ 0, 0, float(w), float(h), 0, 1 };
    vk::Rect2D   scissor{ { 0, 0 }, { uint32_t(w), uint32_t(h) } };
    pipelineViewportStateCreateInfo
        // 设置视口
        .setViewports(viewPort)
        // 设置裁切区域
        .setScissors(scissor);
    graphicsPipelineCreateInfo.setPViewportState(
        &pipelineViewportStateCreateInfo);

    // 4.5:光栅化配置
    vk::PipelineRasterizationStateCreateInfo
        pipelineRasterizationStateCreateInfo;
    pipelineRasterizationStateCreateInfo
        // 设置是否抛弃光栅化结果 - 否
        .setRasterizerDiscardEnable(false)
        // 设置面剔除 - 剔除背面
        .setCullMode(vk::CullModeFlagBits::eBack)
        // 设置如何代表正面 - 逆时针方向代表正面
        .setFrontFace(vk::FrontFace::eClockwise)
        // 设置多边形绘制模式 - 填充
        .setPolygonMode(vk::PolygonMode::eFill)
        // 线框
        // .setPolygonMode(vk::PolygonMode::eLine)
        // 点集
        // .setPolygonMode(vk::PolygonMode::ePoint)
        // 设置线段宽度
        .setLineWidth(1)
        // 设置是否启用深度偏移量 - 2D用不到,不知道用在哪
        // .setDepthBiasEnable(false)
        // 设置深度偏移量夹逼值 - 用到的话需要设置
        // .setDepthBiasClamp()
        ;
    graphicsPipelineCreateInfo.setPRasterizationState(
        &pipelineRasterizationStateCreateInfo);

    // 4.6:多重采样配置
    vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;
    pipelineMultisampleStateCreateInfo
        // 暂时不启用超采样
        .setSampleShadingEnable(false)
        // 光栅化阶段的采样等级(默认为0,至少为1光栅化才能输出像素)
        .setRasterizationSamples(vk::SampleCountFlagBits::e1);
    graphicsPipelineCreateInfo.setPMultisampleState(
        &pipelineMultisampleStateCreateInfo);

    // 4.7:深度测试和模板测试
    // 3d绘制需要，暂时跳过

    // 4.8:色彩融混
    vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo;
    // 4.8.1:颜色附件配置
    vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState;
    pipelineColorBlendAttachmentState
        // 开启混合
        .setBlendEnable(true)
        // 设置 RGB 混合因子
        // 也就是：新颜色(Src) * Alpha + 旧颜色(Dst) * (1 - Alpha)
        .setSrcColorBlendFactor(vk::BlendFactor::eSrcAlpha)
        .setDstColorBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
        .setColorBlendOp(vk::BlendOp::eAdd)  // 相加
        // 设置 Alpha 通道混合因子
        // 通常直接保留新像素的 Alpha，或者两者相加。
        // 下面配置表示：FinalAlpha = (SrcAlpha * 1) + (DstAlpha * 0) =>
        // 直接使用新像素的 Alpha
        .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
        .setDstAlphaBlendFactor(vk::BlendFactor::eZero)
        .setAlphaBlendOp(vk::BlendOp::eAdd)
        // 需要写出的分量 - rgba都要写出
        .setColorWriteMask(
            vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    pipelineColorBlendStateCreateInfo
        // 常规渲染不使用逻辑操作（如异或）
        .setLogicOpEnable(false)
        // 设置颜色附件
        .setAttachments(pipelineColorBlendAttachmentState);
    graphicsPipelineCreateInfo.setPColorBlendState(
        &pipelineColorBlendStateCreateInfo);

    // 4.9:设置renderpass和layout
    graphicsPipelineCreateInfo.setLayout(m_graphicsPipelineLayout);
    graphicsPipelineCreateInfo.setRenderPass(renderPass.m_graphicRenderPass);

    // 4.10:最终创建管线
    auto pipelineCreateResult = logicalDevice.createGraphicsPipeline(
        nullptr, graphicsPipelineCreateInfo);
    assert(pipelineCreateResult.result == vk::Result::eSuccess);
    m_graphicsPipeline = pipelineCreateResult.value;
    XINFO("Created VK Graphics RenderPipeline.");
}

VKRenderPipeline::~VKRenderPipeline()
{
    // 销毁图形渲染管线
    m_logicalDevice.destroyPipeline(m_graphicsPipeline);
    XINFO("Destroyed VK Graphics RenderPipeline.");

    // 销毁图形渲染管线布局
    m_logicalDevice.destroyPipelineLayout(m_graphicsPipelineLayout);
    XINFO("Destroyed VK Graphics RenderPipeline Layout.");
}

}  // namespace Graphic
}  // namespace MMM
