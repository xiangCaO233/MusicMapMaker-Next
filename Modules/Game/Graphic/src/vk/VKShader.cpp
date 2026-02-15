#include "graphic/vk/VKShader.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{

/**
 * @brief 构造函数：基础管线 (Vertex + Fragment)
 *
 * @param vkLogicalDevice 逻辑设备引用
 * @param vertexSource 顶点着色器 SPIR-V 二进制数据
 * @param fragmentSource 片段着色器 SPIR-V 二进制数据
 */
VKShader::VKShader(vk::Device& vkLogicalDevice, std::string_view vertexSource,
                   std::string_view fragmentSource)
    : m_vkLogicalDevice(vkLogicalDevice)
{
    // 着色器模块创建信息
    vk::ShaderModuleCreateInfo shaderCreateInfo;
    // 设置着色器源代码
    shaderCreateInfo.setCodeSize(vertexSource.size());
    shaderCreateInfo.setPCode(
        reinterpret_cast<const uint32_t*>(vertexSource.data()));
    // 创建顶点着色器模块
    m_vertexShaderModule = vkLogicalDevice.createShaderModule(shaderCreateInfo);
    XINFO("Created vertex shader module");
    // 管线着色器模块stage创建信息
    vk::PipelineShaderStageCreateInfo pipelineVertexShaderStageCreateInfo;
    pipelineVertexShaderStageCreateInfo
        // 着色器类型 - 顶点着色器
        .setStage(vk::ShaderStageFlagBits::eVertex)
        // 设置顶点着色器模块
        .setModule(m_vertexShaderModule)
        // 指定着色器入口函数
        .setPName("main");
    m_shaderStageCreateInfos.push_back(pipelineVertexShaderStageCreateInfo);

    // 同理
    shaderCreateInfo.setCodeSize(fragmentSource.size());
    shaderCreateInfo.setPCode(
        reinterpret_cast<const uint32_t*>(fragmentSource.data()));
    // 创建片段着色器模块
    m_fragmentShaderModule =
        vkLogicalDevice.createShaderModule(shaderCreateInfo);
    XINFO("Created fragment shader module");
    // 管线着色器模块stage创建信息
    vk::PipelineShaderStageCreateInfo pipelineFragmentShaderStageCreateInfo;
    pipelineFragmentShaderStageCreateInfo
        .setStage(vk::ShaderStageFlagBits::eFragment)
        .setModule(m_fragmentShaderModule)
        .setPName("main");
    m_shaderStageCreateInfos.push_back(pipelineFragmentShaderStageCreateInfo);
}

/**
 * @brief 构造函数：含几何着色器 (Vertex + Geometry + Fragment)
 *
 * @param vkLogicalDevice 逻辑设备引用
 * @param vertexSource 顶点着色器 SPIR-V 二进制数据
 * @param geometrySource 几何着色器 SPIR-V 二进制数据
 * @param fragmentSource 片段着色器 SPIR-V 二进制数据
 */
VKShader::VKShader(vk::Device& vkLogicalDevice, std::string_view vertexSource,
                   std::string_view geometrySource,
                   std::string_view fragmentSource)
    // 委托构造
    : VKShader(vkLogicalDevice, vertexSource, fragmentSource)
{
    vk::ShaderModuleCreateInfo shaderCreateInfo;
    shaderCreateInfo.setCodeSize(geometrySource.size());
    shaderCreateInfo.setPCode(
        reinterpret_cast<const uint32_t*>(geometrySource.data()));
    // 创建几何着色器模块
    m_geometryShaderModule =
        vkLogicalDevice.createShaderModule(shaderCreateInfo);
    XINFO("Created geometry shader module");
    // 管线着色器模块stage创建信息
    vk::PipelineShaderStageCreateInfo pipelineGeometryShaderStageCreateInfo;
    pipelineGeometryShaderStageCreateInfo
        .setStage(vk::ShaderStageFlagBits::eGeometry)
        .setModule(m_geometryShaderModule)
        .setPName("main");
    m_shaderStageCreateInfos.push_back(pipelineGeometryShaderStageCreateInfo);
}

VKShader::~VKShader()
{
    // 销毁着色器模块
    m_vkLogicalDevice.destroyShaderModule(m_vertexShaderModule);
    XINFO("Destroyed vertex shader module");

    m_vkLogicalDevice.destroyShaderModule(m_fragmentShaderModule);
    XINFO("Destroyed fragment shader module");

    if ( m_geometryShaderModule ) {
        m_vkLogicalDevice.destroyShaderModule(m_geometryShaderModule);
        XINFO("Destroyed geometry shader module");
    }
}

/**
 * @brief 获取渲染管线所需的着色器阶段创建信息列表
 * @return std::vector<vk::PipelineShaderStageCreateInfo>
 *         包含所有已加载 Shader 的 Stage 配置信息
 */
std::vector<vk::PipelineShaderStageCreateInfo>
VKShader::getShaderStageCreateInfos()
{
    return m_shaderStageCreateInfos;
}

} // namespace MMM::Graphic


