#pragma once

#include "vulkan/vulkan.hpp"
#include <string_view>
#include <vector>

namespace MMM::Graphic
{

/**
 * @brief Vulkan 着色器管理类
 *
 * 负责编译、加载和管理 Shader Module（顶点、片段、几何等）。
 * 并提供给管线创建所需的 Shader Stage 信息。
 */
class VKShader final
{
public:
    /**
     * @brief 构造函数：基础管线 (Vertex + Fragment)
     *
     * @param vkLogicalDevice 逻辑设备引用
     * @param vertexSource 顶点着色器 SPIR-V 二进制数据
     * @param fragmentSource 片段着色器 SPIR-V 二进制数据
     */
    VKShader(vk::Device& vkLogicalDevice, std::string_view vertexSource,
             std::string_view fragmentSource);

    /**
     * @brief 构造函数：含几何着色器 (Vertex + Geometry + Fragment)
     *
     * @param vkLogicalDevice 逻辑设备引用
     * @param vertexSource 顶点着色器 SPIR-V 二进制数据
     * @param geometrySource 几何着色器 SPIR-V 二进制数据
     * @param fragmentSource 片段着色器 SPIR-V 二进制数据
     */
    VKShader(vk::Device& vkLogicalDevice, std::string_view vertexSource,
             std::string_view geometrySource, std::string_view fragmentSource);

    // 禁用拷贝和移动
    VKShader(VKShader&&)                 = delete;
    VKShader(const VKShader&)            = delete;
    VKShader& operator=(VKShader&&)      = delete;
    VKShader& operator=(const VKShader&) = delete;

    ~VKShader();

    /**
     * @brief 获取渲染管线所需的着色器阶段创建信息列表
     * @return std::vector<vk::PipelineShaderStageCreateInfo>
     *         包含所有已加载 Shader 的 Stage 配置信息
     */
    std::vector<vk::PipelineShaderStageCreateInfo> getShaderStageCreateInfos();

private:
    /// @brief 逻辑设备引用
    vk::Device& m_vkLogicalDevice;

    /// @brief 顶点着色器模块句柄
    vk::ShaderModule m_vertexShaderModule;

    /// @brief 几何着色器模块句柄 (可选)
    vk::ShaderModule m_geometryShaderModule;

    /// @brief 片段着色器模块句柄
    vk::ShaderModule m_fragmentShaderModule;

    /// @brief 缓存的管线着色器阶段创建信息列表
    std::vector<vk::PipelineShaderStageCreateInfo> m_shaderStageCreateInfos;
};

} // namespace MMM::Graphic


