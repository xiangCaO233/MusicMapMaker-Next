#include "canvas/Basic2DCanvas.h"
#include "config/skin/SkinConfig.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <glm/glm.hpp>

namespace MMM::Canvas
{
Basic2DCanvas::Basic2DCanvas(const std::string& name, uint32_t w, uint32_t h)
    : IRenderableView(name), m_canvasName(name)
{
    m_targetWidth  = w;
    m_targetHeight = h;
}

// 接口实现
void Basic2DCanvas::update()
{
    // 1. 在 Begin 之前设置窗口大小
    // ImGuiCond_FirstUseEver: 只在第一次运行且没有 .ini 配置文件记录时生效
    // ImGuiCond_Always: 强制每一帧都固定这个大小（用户无法拖拽改变大小）
    // ImGuiCond_Once: 每轮启动只设置一次
    ImGui::SetNextWindowSize(ImVec2((float)m_width, (float)m_height),
                             ImGuiCond_FirstUseEver);

    ImGui::Begin("Music Score");
    // 每一帧都要从 0 开始重新构建顶点
    m_brush.clear();
    // 处理逻辑... 填充 m_brush
    m_brush.drawRect(glm::vec2{ 20.f, 20.f },
                     glm::vec2{ 100, 100 },
                     glm::vec4{ 0, 1, 1, 1 });

    // 1. 获取 ImGui 窗口分配给内容的实际大小
    ImVec2 size = ImGui::GetContentRegionAvail();
    if ( size.x > 0 && size.y > 0 ) {
        // 仅仅是发送请求，不会立刻改变渲染状态
        setTargetSize(static_cast<uint32_t>(size.x),
                      static_cast<uint32_t>(size.y));
    }

    vk::DescriptorSet texID = getDescriptorSet();
    // 增加判空，防止在重构瞬间崩溃
    if ( texID != VK_NULL_HANDLE ) {
        ImGui::Image((ImTextureID)(VkDescriptorSet)texID, size);
    } else {
        ImGui::Text("Loading Surface...");
    }
    ImGui::End();
}

///@brief 是否需要重新记录命令 (比如数据变了)
bool Basic2DCanvas::isDirty() const
{
    return false;
}

/**
 * @brief 获取 Shader 源码接口 (在固定时刻需要创建)
 */
std::vector<std::string> Basic2DCanvas::getShaderSources(
    const std::string& shader_name)
{
    // 如果缓存里有，直接返回内存里的字符串拷贝
    if ( m_shaderSourceCache.find(shader_name) != m_shaderSourceCache.end() ) {
        return m_shaderSourceCache[shader_name];
    }

    // 读取spv源码初始化shader
    Config::SkinData::CanvasConfig canvas_config =
        Config::SkinManager::instance().getCanvasConfig(m_canvasName);
    if ( canvas_config.canvas_name == "" ) {
        XERROR("无法获取画布{}的配置", m_canvasName);
        return {};
    }

    // 获取着色器源码
    if ( auto shaderModuleIt =
             canvas_config.canvas_shader_modules.find(shader_name);
         shaderModuleIt != canvas_config.canvas_shader_modules.end() ) {
        // 着色器spv文件路径
        auto shader_spv_path = shaderModuleIt->second;
        if ( !std::filesystem::exists(shader_spv_path) ) {
            XWARN("Shader module {} not defiend.", shader_name);
            return {};
        }

        // 读取着色器文件内容
        std::string vertexShaderSource = Graphic::VKShader::readFile(
            (shader_spv_path / "VertexShader.spv").generic_string());
        std::string fragmentShaderSource = Graphic::VKShader::readFile(
            (shader_spv_path / "FragmentShader.spv").generic_string());

        // 源码列表
        std::vector<std::string> result;

        if ( auto geometryShaderPath = (shader_spv_path / "GeometryShader.spv");
             std::filesystem::exists(geometryShaderPath) ) {
            result = { vertexShaderSource,
                       Graphic::VKShader::readFile(
                           geometryShaderPath.generic_string()),
                       fragmentShaderSource };
        } else {
            result = { vertexShaderSource, fragmentShaderSource };
        }

        // 存入缓存
        m_shaderSourceCache[shader_name] = result;
        return result;
    } else {
        XERROR("无法获取画布{}的{}着色器配置", m_canvasName, shader_name);
        return {};
    }
}

/**
 * @brief 获取 Shader 名称(需要按名称储存和销毁)
 */
std::string Basic2DCanvas::getShaderName(const std::string& shader_module_name)
{
    return m_canvasName + ":" + shader_module_name;
}

}  // namespace MMM::Canvas
