#include "graphic/imguivk/VKRenderPipeline.h"
#include "graphic/imguivk/mem/VKUniforms.h"
#include "graphic/imguivk/mesh/VKBasicVertex.h"
#include "log/colorful-log.h"
#include <cassert>
#include <glm/glm.hpp>

namespace MMM::Graphic
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
                                   VKSwapchain& swapchain, bool is2DCanvas,
                                   int w, int h, bool additiveBlend,
                                   bool                    blendEnable,
                                   vk::DescriptorSetLayout sharedLayout)
    : m_logicalDevice(logicalDevice)
{
    if ( sharedLayout != VK_NULL_HANDLE ) {
        m_descriptorSetLayout    = sharedLayout;
        m_ownDescriptorSetLayout = false;
        XDEBUG("Using Shared VK Descriptor Set Layout.");
    } else {
        // 2:创建Descriptor Set布局
        vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
        // 包括uniform在内的所有描述符绑定配置
        descriptorSetLayoutCreateInfo.setBindings(
            { Graphic::BRUSH_TEXTURE_BIND_DESC });
        m_descriptorSetLayout =
            logicalDevice
                .createDescriptorSetLayout(descriptorSetLayoutCreateInfo)
                .value;
        m_ownDescriptorSetLayout = true;
        XDEBUG("Created VK Descriptor Set Layout.");
    }

    // --- 定义 Push Constant 范围 ---
    vk::PushConstantRange pushConstantRange;
    pushConstantRange
        .setStageFlags(
            vk::ShaderStageFlagBits::eVertex |
            vk::ShaderStageFlagBits::eFragment)  // 在顶点和片元着色器使用
        .setOffset(0)
        .setSize(sizeof(glm::mat4));  // 大小为一个 4x4 矩阵 (64 bytes)

    // 3:创建渲染管线布局(主要说明整个shader中uniform变量的布局)
    vk::PipelineLayoutCreateInfo pipelineLayoutCreateInfo;
    // 设置SetLayout到管线布局配置中
    pipelineLayoutCreateInfo
        // 描述符布局配置
        .setSetLayouts(m_descriptorSetLayout)
        // pushConstant范围配置
        .setPushConstantRanges(pushConstantRange);
    m_graphicsPipelineLayout =
        logicalDevice.createPipelineLayout(pipelineLayoutCreateInfo).value;
    XDEBUG("Created VK Graphics RenderPipeline Layout.");

    // 4:图形管线创建信息
    vk::GraphicsPipelineCreateInfo graphicsPipelineCreateInfo;

    // 4.1:顶点输入状态创建信息
    vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo;
    // 设置顶点输入属性描述信息
    pipelineVertexInputStateCreateInfo
        .setVertexBindingDescriptions(Graphic::Vertex::VKVERTEX_BIND_DESC)
        .setVertexAttributeDescriptions(Graphic::Vertex::VKVERTEX_ATTR_DESC);
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
    if ( is2DCanvas ) {
        // 2d窗口 - 不固定视口大小
        pipelineViewportStateCreateInfo
            // 设置视口数量 1 在命令录制时传入具体的视口
            .setViewportCount(1)
            // 设置裁切区域数量 1 在命令录制时传入具体的裁切区域
            .setScissorCount(1);
    } else {
        // 非2d窗口 - 按传入的视口大小固定
        // 单纯绘制窗口区域 - 无裁切
        vk::Viewport viewPort{
            0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1
        };
        vk::Rect2D scissor{
            { 0, 0 }, { static_cast<uint32_t>(w), static_cast<uint32_t>(h) }
        };
        pipelineViewportStateCreateInfo
            // 设置视口
            .setViewports(viewPort)
            // 设置裁切区域
            .setScissors(scissor);
    }
    graphicsPipelineCreateInfo.setPViewportState(
        &pipelineViewportStateCreateInfo);

    // 4.5 配置动态状态
    std::vector<vk::DynamicState> dynamicStates;
    if ( is2DCanvas ) {
        dynamicStates.push_back(vk::DynamicState::eViewport);
        dynamicStates.push_back(vk::DynamicState::eScissor);
    }

    vk::PipelineDynamicStateCreateInfo dynamicStateCreateInfo;
    dynamicStateCreateInfo.setDynamicStates(dynamicStates);

    // 将动态状态关联到管线
    if ( !dynamicStates.empty() ) {
        graphicsPipelineCreateInfo.setPDynamicState(&dynamicStateCreateInfo);
    }

    // 4.6:光栅化配置
    vk::PipelineRasterizationStateCreateInfo
        pipelineRasterizationStateCreateInfo;
    pipelineRasterizationStateCreateInfo
        // 设置是否抛弃光栅化结果 - 否
        .setRasterizerDiscardEnable(false)
        // 设置面剔除 - 剔除背面
        .setCullMode(is2DCanvas ?
                                // 画布模式不设置面剔除
                         vk::CullModeFlagBits::eNone
                                // 非画布模式不设置剔除背面
                                : vk::CullModeFlagBits::eBack)
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

    // 4.7:多重采样配置
    vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;
    pipelineMultisampleStateCreateInfo
        // 暂时不启用超采样
        .setSampleShadingEnable(false)
        // 光栅化阶段的采样等级(默认为0,至少为1光栅化才能输出像素)
        .setRasterizationSamples(vk::SampleCountFlagBits::e1);
    graphicsPipelineCreateInfo.setPMultisampleState(
        &pipelineMultisampleStateCreateInfo);

    // 4.8:深度测试和模板测试
    // 3d绘制需要，暂时跳过

    // 4.9:色彩融混
    vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo;
    // 4.9.1:颜色附件配置
    vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState;
    pipelineColorBlendAttachmentState.setBlendEnable(blendEnable);

    if ( additiveBlend ) {
        pipelineColorBlendAttachmentState
            .setSrcColorBlendFactor(vk::BlendFactor::eOne)
            .setDstColorBlendFactor(vk::BlendFactor::eOne)
            .setColorBlendOp(vk::BlendOp::eAdd)
            .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
            .setDstAlphaBlendFactor(vk::BlendFactor::eOne)
            .setAlphaBlendOp(vk::BlendOp::eAdd);
    } else {
        pipelineColorBlendAttachmentState
            .setSrcColorBlendFactor(vk::BlendFactor::eSrcAlpha)
            .setDstColorBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
            .setColorBlendOp(vk::BlendOp::eAdd)
            .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
            .setDstAlphaBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
            .setAlphaBlendOp(vk::BlendOp::eAdd);
    }

    pipelineColorBlendAttachmentState.setColorWriteMask(
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    pipelineColorBlendStateCreateInfo
        // 常规渲染不使用逻辑操作（如异或）
        .setLogicOpEnable(false)
        // 设置颜色附件
        .setAttachments(pipelineColorBlendAttachmentState);
    graphicsPipelineCreateInfo.setPColorBlendState(
        &pipelineColorBlendStateCreateInfo);

    // 4.10:设置renderpass和layout
    graphicsPipelineCreateInfo.setLayout(m_graphicsPipelineLayout);
    graphicsPipelineCreateInfo.setRenderPass(renderPass.getRenderPass());

    // 4.11:最终创建管线
    auto pipelineCreateResult = logicalDevice.createGraphicsPipeline(
        nullptr, graphicsPipelineCreateInfo);
    assert(pipelineCreateResult.result == vk::Result::eSuccess);
    m_graphicsPipeline = pipelineCreateResult.value;
    XDEBUG("Created VK Graphics RenderPipeline.");
}

VKRenderPipeline::~VKRenderPipeline()
{
    // 销毁图形渲染管线
    m_logicalDevice.destroyPipeline(m_graphicsPipeline);
    XDEBUG("Destroyed VK Graphics RenderPipeline.");

    // 销毁图形渲染管线布局
    m_logicalDevice.destroyPipelineLayout(m_graphicsPipelineLayout);
    XDEBUG("Destroyed VK Graphics RenderPipeline Layout.");

    // 销毁Descriptor Set布局
    if ( m_ownDescriptorSetLayout ) {
        m_logicalDevice.destroyDescriptorSetLayout(m_descriptorSetLayout);
        XDEBUG("Destroyed VK Descriptor Set Layout.");
    }
}

}  // namespace MMM::Graphic
