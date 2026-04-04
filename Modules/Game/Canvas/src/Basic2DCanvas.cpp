#include "canvas/Basic2DCanvas.h"
#include "config/skin/SkinConfig.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/LogicCommands.h"
#include "ui/ITextureLoader.h"
#include "ui/IUIView.h"
#include "ui/UIManager.h"
#include <filesystem>
#include <glm/glm.hpp>
#include <utility>

namespace MMM::Canvas
{
Basic2DCanvas::Basic2DCanvas(const std::string& name, uint32_t w, uint32_t h)
    : IUIView(name), IRenderableView(name), m_canvasName(name)
{
    m_targetWidth  = w;
    m_targetHeight = h;
}

// 接口实现
void Basic2DCanvas::update(UI::UIManager* sourceManager)
{
    std::string windowName =
        TR("canvas.editor") + std::string("###") + m_canvasName;
    UI::LayoutContext lctx(m_layoutCtx, windowName);
    RenderContext     rctx(
        this, m_canvasName.c_str(), m_targetWidth, m_targetHeight);

    // 尝试拉取最新的逻辑线程渲染快照 (专属摄像机缓冲)
    auto syncBuffer =
        Logic::EditorEngine::instance().getSyncBuffer(m_canvasName);
    if ( syncBuffer ) {
        m_currentSnapshot = syncBuffer->pullLatestSnapshot();
    }

    // --- MVP 交互实现：拾取测试 ---
    if ( m_currentSnapshot ) {
        // 获取鼠标在 ImGui 窗口中的相对位置
        ImVec2 mousePos = ImGui::GetMousePos();

        // 由于 Basic2DCanvas 是绘制在全屏 Dock 窗口中的 Image，
        // 我们需要计算相对于窗口内容的本地坐标。
        ImVec2 windowPos     = ImGui::GetWindowPos();
        ImVec2 localMousePos = { mousePos.x - windowPos.x,
                                 mousePos.y - windowPos.y };

        entt::entity hoveredEntity = entt::null;

        // 倒序遍历 Hitbox (最上层的优先)
        for ( auto it = m_currentSnapshot->hitboxes.rbegin();
              it != m_currentSnapshot->hitboxes.rend();
              ++it ) {
            if ( localMousePos.x >= it->x && localMousePos.x <= it->x + it->w &&
                 localMousePos.y >= it->y &&
                 localMousePos.y <= it->y + it->h ) {
                hoveredEntity = it->entity;
                break;
            }
        }

        // 推送悬停指令到逻辑线程
        Logic::EditorEngine::instance().pushCommand(
            Logic::CmdSetHoveredEntity{ hoveredEntity });
    }
}

// --- 改变尺寸后的回调 ---
void Basic2DCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                               uint32_t h) const
{
    Event::CanvasResizeEvent e;
    e.canvasName = m_canvasName;
    e.lastSize   = { oldW, oldH };
    e.newSize    = { w, h };
    Event::EventBus::instance().publish(e);
}

const std::vector<Graphic::Vertex::VKBasicVertex>&
Basic2DCanvas::getVertices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->vertices;
    }
    static std::vector<Graphic::Vertex::VKBasicVertex> empty;
    return empty;
}

const std::vector<uint32_t>& Basic2DCanvas::getIndices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->indices;
    }
    static std::vector<uint32_t> empty;
    return empty;
}

void Basic2DCanvas::onRecordDrawCmds(vk::CommandBuffer& cmdBuf,
                                     vk::PipelineLayout pipelineLayout,
                                     vk::DescriptorSet  defaultDescriptor)
{
    if ( !m_currentSnapshot ) return;

    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        vk::DescriptorSet actualTexture = cmd.texture;
        
        if (cmd.customTextureId != 0) {
            auto textureId = static_cast<Logic::TextureID>(cmd.customTextureId);
            switch (textureId) {
                case Logic::TextureID::Note:
                    if (m_noteTexture) actualTexture = (VkDescriptorSet)(uint64_t)m_noteTexture->getImTextureID();
                    break;
                case Logic::TextureID::HoldHead:
                    if (m_holdOrFlickHeadTexture) actualTexture = (VkDescriptorSet)(uint64_t)m_holdOrFlickHeadTexture->getImTextureID();
                    break;
                case Logic::TextureID::HoldEnd:
                    if (m_holdEndTexture) actualTexture = (VkDescriptorSet)(uint64_t)m_holdEndTexture->getImTextureID();
                    break;
                case Logic::TextureID::HoldBodyVertical:
                    if (m_holdBodyVerticalTexture) actualTexture = (VkDescriptorSet)(uint64_t)m_holdBodyVerticalTexture->getImTextureID();
                    break;
                case Logic::TextureID::HoldBodyHorizontal:
                    if (m_holdBodyHorizontalTexture) actualTexture = (VkDescriptorSet)(uint64_t)m_holdBodyHorizontalTexture->getImTextureID();
                    break;
                case Logic::TextureID::FlickArrowLeft:
                    if (m_flickArrowLeftTexture) actualTexture = (VkDescriptorSet)(uint64_t)m_flickArrowLeftTexture->getImTextureID();
                    break;
                case Logic::TextureID::FlickArrowRight:
                    if (m_flickArrowRightTexture) actualTexture = (VkDescriptorSet)(uint64_t)m_flickArrowRightTexture->getImTextureID();
                    break;
                default:
                    break;
            }
        }

        if ( actualTexture != VK_NULL_HANDLE ) {
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &actualTexture,
                                      0,
                                      nullptr);
        } else {
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &defaultDescriptor,
                                      0,
                                      nullptr);
        }
        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

///@brief 是否需要重新记录命令 (比如数据变了)
bool Basic2DCanvas::isDirty() const
{
    return true;
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
    auto& skin = Config::SkinManager::instance();

    // 载入Note纹理
    m_noteTexture =
        std::make_unique<Graphic::VKTexture>(skin.getAssetPath("note.note"),
                                             physicalDevice,
                                             logicalDevice,
                                             cmdPool,
                                             queue);
    m_holdOrFlickHeadTexture =
        std::make_unique<Graphic::VKTexture>(skin.getAssetPath("note.head"),
                                             physicalDevice,
                                             logicalDevice,
                                             cmdPool,
                                             queue);
    m_holdEndTexture =
        std::make_unique<Graphic::VKTexture>(skin.getAssetPath("note.holdend"),
                                             physicalDevice,
                                             logicalDevice,
                                             cmdPool,
                                             queue);
    m_holdBodyVerticalTexture = std::make_unique<Graphic::VKTexture>(
        skin.getAssetPath("note.holdbodyvertical"),
        physicalDevice,
        logicalDevice,
        cmdPool,
        queue);
    m_holdBodyHorizontalTexture = std::make_unique<Graphic::VKTexture>(
        skin.getAssetPath("note.holdbodyhorizontal"),
        physicalDevice,
        logicalDevice,
        cmdPool,
        queue);
    m_flickArrowLeftTexture = std::make_unique<Graphic::VKTexture>(
        skin.getAssetPath("note.arrowleft"),
        physicalDevice,
        logicalDevice,
        cmdPool,
        queue);
    m_flickArrowRightTexture = std::make_unique<Graphic::VKTexture>(
        skin.getAssetPath("note.arrowright"),
        physicalDevice,
        logicalDevice,
        cmdPool,
        queue);
}

}  // namespace MMM::Canvas
