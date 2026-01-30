#pragma once

#include "vulkan/vulkan.hpp"
#include <string_view>
#include <vector>

namespace MMM
{
namespace Graphic
{

class VKShader final
{
public:
    VKShader(vk::Device& vkLogicalDevice, std::string_view vertexSource,
             std::string_view fragmentSource);
    VKShader(vk::Device& vkLogicalDevice, std::string_view vertexSource,
             std::string_view geometrySource, std::string_view fragmentSource);
    VKShader(VKShader&&)                 = delete;
    VKShader(const VKShader&)            = delete;
    VKShader& operator=(VKShader&&)      = delete;
    VKShader& operator=(const VKShader&) = delete;
    ~VKShader();

    /*
     * 获取vk渲染管线着色器模块stage创建信息列表
     * */
    std::vector<vk::PipelineShaderStageCreateInfo> getShaderStageCreateInfos();

private:
    /*
     * vk逻辑设备引用
     * */
    vk::Device& m_vkLogicalDevice;

    /*
     * vk顶点着色器模块
     * */
    vk::ShaderModule m_vertexShaderModule;

    /*
     * vk几何着色器模块
     * */
    vk::ShaderModule m_geometryShaderModule;

    /*
     * vk片段着色器模块
     * */
    vk::ShaderModule m_fragmentShaderModule;

    /*
     * vk渲染管线着色器模块stage创建信息列表
     * */
    std::vector<vk::PipelineShaderStageCreateInfo> m_shaderStageCreateInfos;
};

}  // namespace Graphic

}  // namespace MMM
