#include "graphic/imguivk/VKRenderPipeline.h"
#include "graphic/imguivk/mem/VKUniforms.h"
#include "graphic/imguivk/mesh/VKBasicVertex.h"
#include "log/colorful-log.h"
#include <glm/glm.hpp>

namespace MMM::Graphic
{

/**
 * @brief 根据画布用途、混合模式和资源布局创建一条固定图形管线。
 *
 * 构造期间的所有 Vulkan create-info 都是局部描述，创建完成后只保留管线、管线
 * 布局以及可能自有的 descriptor set layout。共享布局由调用方维持生命周期。
 *
 * @param logicalDevice 逻辑设备引用
 * @param shader 着色器管理器引用 (提供 Shader Stages)
 * @param renderPass 渲染流程引用 (提供附件格式兼容性)
 * @param swapchain 交换链引用
 * @param w 视口宽度
 * @param h 视口高度
 * @param is2DCanvas 是否把 viewport 与 scissor 设为命令录制时提供的动态状态。
 * @param additiveBlend 是否使用预乘颜色的纯加法混合。
 * @param blendEnable 是否启用颜色附件混合。
 * @param sharedLayout 可复用的 descriptor set layout；为空时在本对象内创建。
 * @param useVertexInput 是否从绑定的顶点缓冲读取标准画布顶点。
 * @param alphaWeightedAdditive 是否对非预乘来源执行带源 Alpha 的加法混合。
 */
VKRenderPipeline::VKRenderPipeline(
    vk::Device& logicalDevice, VKShader& shader, VKRenderPass& renderPass,
    VKSwapchain& swapchain, bool is2DCanvas, int w, int h, bool additiveBlend,
    bool blendEnable, vk::DescriptorSetLayout sharedLayout, bool useVertexInput,
    bool alphaWeightedAdditive)
    : m_logicalDevice(logicalDevice)
{
    if ( sharedLayout != VK_NULL_HANDLE ) {
        // 共享布局由资源所有者统一销毁，本对象只把它纳入 pipeline layout。
        m_descriptorSetLayout    = sharedLayout;
        m_ownDescriptorSetLayout = false;
        XDEBUG("Using Shared VK Descriptor Set Layout.");
    } else {
        // 独立管线使用与 Brush 一致的 combined image sampler
        // 绑定，并记录所有权， 使析构只清理由本对象创建的布局。
        vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
        descriptorSetLayoutCreateInfo.setBindings(
            { Graphic::BRUSH_TEXTURE_BIND_DESC });
        m_descriptorSetLayout =
            logicalDevice
                .createDescriptorSetLayout(descriptorSetLayoutCreateInfo)
                .value;
        m_ownDescriptorSetLayout = true;
        XDEBUG("Created VK Descriptor Set Layout.");
    }

    // 每次绘制通过 push constant 传入一个变换矩阵；顶点与片元阶段共享同一范围，
    // 其布局必须与对应 SPIR-V 声明保持一致。
    vk::PushConstantRange pushConstantRange;
    pushConstantRange
        .setStageFlags(
            vk::ShaderStageFlagBits::eVertex |
            vk::ShaderStageFlagBits::eFragment)  // 在顶点和片元着色器使用
        .setOffset(0)
        .setSize(sizeof(glm::mat4));  // 大小为一个 4x4 矩阵 (64 bytes)

    // pipeline layout 把纹理描述符与变换矩阵组成着色器可见的完整资源接口。
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

    // 常规 Brush 管线读取固定顶点格式；全屏效果着色器可依赖 gl_VertexIndex
    // 自行生成顶点，此时保持空输入描述，避免无意义的缓冲绑定契约。
    vk::PipelineVertexInputStateCreateInfo pipelineVertexInputStateCreateInfo;
    // 仅在需要顶点输入时绑定属性描述（效果着色器自行生成顶点）
    if ( useVertexInput ) {
        pipelineVertexInputStateCreateInfo
            .setVertexBindingDescriptions(Graphic::Vertex::VKVERTEX_BIND_DESC)
            .setVertexAttributeDescriptions(
                Graphic::Vertex::VKVERTEX_ATTR_DESC);
    }
    graphicsPipelineCreateInfo.setPVertexInputState(
        &pipelineVertexInputStateCreateInfo);

    // 所有调用方提交彼此独立的三角形，关闭 primitive restart，避免引入索引
    // 缓冲哨兵值约定。
    vk::PipelineInputAssemblyStateCreateInfo
        pipelineInputAssemblyStateCreateInfo;
    pipelineInputAssemblyStateCreateInfo
        // 图元重置 - 不启用
        .setPrimitiveRestartEnable(false)
        // 图元装配方式：使用三角形列表。
        .setTopology(vk::PrimitiveTopology::eTriangleList);
    graphicsPipelineCreateInfo.setPInputAssemblyState(
        &pipelineInputAssemblyStateCreateInfo);

    // stages 内部引用 VKShader 持有的 module；模块至少要存活到管线创建返回。
    auto stages = shader.getShaderStageCreateInfos();
    graphicsPipelineCreateInfo.setStages(stages);

    // 画布纹理会以不同区域和尺寸重复绘制，因此 viewport/scissor 延迟到命令
    // 录制时设置；固定窗口管线则把传入尺寸烘焙进不可变状态。
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

    // 只有声明为动态的状态才允许 vkCmdSetViewport/vkCmdSetScissor 覆盖；固定
    // 管线传入空列表，并且不挂接空的动态状态结构。
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

    // 二维画布可能因坐标变换改变绕序，关闭剔除；固定窗口内容保留背面剔除。
    // 当前附件不使用线框或宽线 feature，因此固定为填充和 1 像素线宽。
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
        // 设置多边形绘制模式：填充。
        .setPolygonMode(vk::PolygonMode::eFill)
        // 设置线段宽度
        .setLineWidth(1);
    graphicsPipelineCreateInfo.setPRasterizationState(
        &pipelineRasterizationStateCreateInfo);

    // RenderPass 当前使用单采样附件，管线必须选择 e1 与其兼容；抗锯齿由纹理
    // 内容和着色器处理，不在此处隐式启用设备 feature。
    vk::PipelineMultisampleStateCreateInfo pipelineMultisampleStateCreateInfo;
    pipelineMultisampleStateCreateInfo
        // 暂时不启用超采样
        .setSampleShadingEnable(false)
        // 光栅化阶段的采样等级(默认为0,至少为1光栅化才能输出像素)
        .setRasterizationSamples(vk::SampleCountFlagBits::e1);
    graphicsPipelineCreateInfo.setPMultisampleState(
        &pipelineMultisampleStateCreateInfo);

    // 当前渲染顺序通过提交顺序与 Alpha 合成表达，不创建深度/模板附件，也不向
    // GraphicsPipelineCreateInfo 提供对应状态。

    // 三种混合契约分别服务非预乘透明、预乘加法和非预乘发光纹理；选择顺序让
    // alphaWeightedAdditive 成为更具体的模式。
    vk::PipelineColorBlendStateCreateInfo pipelineColorBlendStateCreateInfo;
    // 4.9.1:颜色附件配置
    vk::PipelineColorBlendAttachmentState pipelineColorBlendAttachmentState;
    pipelineColorBlendAttachmentState.setBlendEnable(blendEnable);

    if ( alphaWeightedAdditive ) {
        // 直通 RGBA 贴图只在此乘一次 Alpha；目标颜色不衰减，暗边不会压黑背景。
        // Alpha 保持目标值，避免发光层破坏后续离屏画布的透明合成。
        pipelineColorBlendAttachmentState
            .setSrcColorBlendFactor(vk::BlendFactor::eSrcAlpha)
            .setDstColorBlendFactor(vk::BlendFactor::eOne)
            .setColorBlendOp(vk::BlendOp::eAdd)
            .setSrcAlphaBlendFactor(vk::BlendFactor::eZero)
            .setDstAlphaBlendFactor(vk::BlendFactor::eOne)
            .setAlphaBlendOp(vk::BlendOp::eAdd);
    } else if ( additiveBlend ) {
        // 预乘来源的 RGB 已包含 Alpha 权重，源和目标均以 One 相加；Alpha 同样
        // 累积，供只依赖亮度的效果附件使用。
        pipelineColorBlendAttachmentState
            .setSrcColorBlendFactor(vk::BlendFactor::eOne)
            .setDstColorBlendFactor(vk::BlendFactor::eOne)
            .setColorBlendOp(vk::BlendOp::eAdd)
            .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
            .setDstAlphaBlendFactor(vk::BlendFactor::eOne)
            .setAlphaBlendOp(vk::BlendOp::eAdd);
    } else {
        // 常规透明使用直通 Alpha 合成，保持项目 UI 与画布纹理的一致语义。
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

    // render pass 决定附件兼容性，pipeline layout 决定描述符与 push constant
    // 接口；两者都是创建固定管线不可缺少的外部契约。
    graphicsPipelineCreateInfo.setLayout(m_graphicsPipelineLayout);
    graphicsPipelineCreateInfo.setRenderPass(renderPass.getRenderPass());

    // 无异常 Vulkan-Hpp 接口显式返回结果；失败时保留空句柄，让 isValid 和析构
    // 路径都能安全识别未完成状态。
    auto pipelineCreateResult = logicalDevice.createGraphicsPipeline(
        nullptr, graphicsPipelineCreateInfo);
    if ( pipelineCreateResult.result != vk::Result::eSuccess ) {
        XERROR("Failed to create VK Graphics RenderPipeline: {}",
               vk::to_string(pipelineCreateResult.result));
        m_graphicsPipeline = nullptr;
        return;
    }
    m_graphicsPipeline = pipelineCreateResult.value;
    XDEBUG("Created VK Graphics RenderPipeline.");
}

VKRenderPipeline::~VKRenderPipeline()
{
    // 按引用依赖逆序释放：pipeline 引用 layout，pipeline layout 又引用
    // descriptor set layout。共享 descriptor 布局不属于本对象，绝不能在此销毁。
    if ( m_graphicsPipeline ) {
        m_logicalDevice.destroyPipeline(m_graphicsPipeline);
        XDEBUG("Destroyed VK Graphics RenderPipeline.");
    }

    // 销毁图形渲染管线布局
    if ( m_graphicsPipelineLayout ) {
        m_logicalDevice.destroyPipelineLayout(m_graphicsPipelineLayout);
        XDEBUG("Destroyed VK Graphics RenderPipeline Layout.");
    }

    // 销毁Descriptor Set布局
    if ( m_ownDescriptorSetLayout && m_descriptorSetLayout ) {
        m_logicalDevice.destroyDescriptorSetLayout(m_descriptorSetLayout);
        XDEBUG("Destroyed VK Descriptor Set Layout.");
    }
}

}  // namespace MMM::Graphic
