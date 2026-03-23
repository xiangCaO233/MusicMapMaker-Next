#include "canvas/Basic2DCanvas.h"
#include "config/skin/SkinConfig.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "ui/ITextureLoader.h"
#include "ui/IUIView.h"
#include "ui/UIManager.h"
#include <filesystem>
#include <glm/glm.hpp>
#include <random>
#include <utility>

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
    Graphic::UI::LayoutContext lctx(m_layoutCtx, m_canvasName.c_str());
    RenderContext              rctx(
        this, m_canvasName.c_str(), m_targetWidth, m_targetHeight);

    // m_brush.drawRect({ 40, 40 }, { 70, 100 }, { 0, 1, 1, 1 });

    // 2. 准备随机数引擎
    // 注意：如果想让位置固定，把 engine 和 dist 设为 static
    std::random_device rd;
    std::mt19937       gen(rd());

    // 坐标范围：从 0 到画布宽高
    std::uniform_real_distribution<float> distX(0.0f, (float)m_targetWidth);
    std::uniform_real_distribution<float> distY(0.0f, (float)m_targetHeight);
    // 尺寸范围：10 到 50 像素
    std::uniform_real_distribution<float> distSize(10.0f, 50.0f);
    // 颜色范围：0.0 到 1.0
    std::uniform_real_distribution<float> distCol(0.0f, 1.0f);

    // 3. 循环绘制 100 个
    for ( int i = 0; i < 200; ++i ) {
        glm::vec2 randomPos   = { distX(gen), distY(gen) };
        glm::vec2 randomSize  = { distSize(gen), distSize(gen) };
        glm::vec4 randomColor = {
            distCol(gen), distCol(gen), distCol(gen), 1.0f
        };

        m_brush.drawRect(randomPos, randomSize, randomColor);
    }
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

/// @brief 是否需要重载
bool Basic2DCanvas::needReload()
{
    return std::exchange(m_needReload, false);
}

/// @brief 重载纹理
void Basic2DCanvas::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                   vk::Device&         logicalDevice,
                                   vk::CommandPool& cmdPool, vk::Queue& queue)
{
}

}  // namespace MMM::Canvas
