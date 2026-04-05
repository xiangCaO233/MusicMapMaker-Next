#include "canvas/PreviewCanvas.h"
#include "common/LogicCommands.h"
#include "config/skin/SkinConfig.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "ui/ITextureLoader.h"
#include "ui/IUIView.h"
#include "ui/UIManager.h"
#include <filesystem>
#include <glm/glm.hpp>
#include <utility>

namespace MMM::Canvas
{
PreviewCanvas::PreviewCanvas(const std::string& name, uint32_t w, uint32_t h)
    : IUIView(name), IRenderableView(name), m_canvasName(name)
{
    m_targetWidth  = w;
    m_targetHeight = h;
}

void PreviewCanvas::update(UI::UIManager* sourceManager)
{
    // 预览窗口专用 ID：###PreviewWindow
    std::string windowName =
        fmt::format("{}###PreviewWindow", TR("canvas.preview"));

    UI::LayoutContext lctx(m_layoutCtx, windowName);
    RenderContext rctx(this, windowName.c_str(), m_targetWidth, m_targetHeight);

    // 拉取预览视口的快照
    auto syncBuffer = Logic::EditorEngine::instance().getSyncBuffer(m_cameraId);
    if ( syncBuffer ) {
        m_currentSnapshot = syncBuffer->pullLatestSnapshot();
    }

    // --- 简单的点击跳转时间逻辑 ---
    if ( m_currentSnapshot && ImGui::IsWindowHovered() &&
         ImGui::IsMouseClicked(0) ) {
        ImVec2 mousePos  = ImGui::GetMousePos();
        ImVec2 windowPos = ImGui::GetWindowPos();
        ImVec2 localPos  = { mousePos.x - windowPos.x,
                             mousePos.y - windowPos.y };

        Logic::EditorEngine::instance().pushCommand(
            Logic::CmdUpdateDrag{ m_cameraId, localPos.x, localPos.y });
        Logic::EditorEngine::instance().pushCommand(
            Logic::CmdEndDrag{ m_cameraId });
    }
}

bool PreviewCanvas::isDirty() const
{
    return true;
}

void PreviewCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                               uint32_t h) const
{
    Event::CanvasResizeEvent e;
    e.canvasName = m_cameraId;
    e.lastSize   = { oldW, oldH };
    e.newSize    = { w, h };
    Event::EventBus::instance().publish(e);
}

std::vector<std::string> PreviewCanvas::getShaderSources(
    const std::string& shader_name)
{
    if ( m_shaderSourceCache.count(shader_name) ) {
        return m_shaderSourceCache[shader_name];
    }

    // 预览窗口复用 BasicCanvas 的着色器配置
    Config::SkinData::CanvasConfig canvas_config =
        Config::SkinManager::instance().getCanvasConfig(m_canvasName);

    if ( canvas_config.canvas_name == "" ) {
        XERROR("PreviewCanvas: 无法获取 {} 的着色器配置", m_canvasName);
        return {};
    }

    if ( auto it = canvas_config.canvas_shader_modules.find(shader_name);
         it != canvas_config.canvas_shader_modules.end() ) {

        auto path = it->second;
        if ( !std::filesystem::exists(path) ) return {};

        std::string vs = Graphic::VKShader::readFile(
            (path / "VertexShader.spv").generic_string());
        std::string fs = Graphic::VKShader::readFile(
            (path / "FragmentShader.spv").generic_string());

        std::vector<std::string> result;
        auto                     gsPath = path / "GeometryShader.spv";
        if ( std::filesystem::exists(gsPath) ) {
            result = { vs,
                       Graphic::VKShader::readFile(gsPath.generic_string()),
                       fs };
        } else {
            result = { vs, fs };
        }

        m_shaderSourceCache[shader_name] = result;
        return result;
    }

    return {};
}

std::string PreviewCanvas::getShaderName(const std::string& shader_module_name)
{
    return "PreviewCanvas:" + shader_module_name;
}

bool PreviewCanvas::needReload()
{
    return m_needReload;
}

void PreviewCanvas::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                   vk::Device&         logicalDevice,
                                   vk::CommandPool& cmdPool, vk::Queue& queue)
{
    m_physicalDevice = physicalDevice;
    m_logicalDevice  = logicalDevice;
    m_cmdPool        = cmdPool;
    m_queue          = queue;

    m_textureAtlas = std::make_unique<Graphic::VKTextureAtlas>(
        physicalDevice, logicalDevice, cmdPool, queue);

    unsigned char white[] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255 };
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::None), white, 4, 4);

    auto& skin   = Config::SkinManager::instance();
    auto  addTex = [&](Logic::TextureID id, const std::string& key) {
        auto p = skin.getAssetPath(key);
        if ( !p.empty() )
            m_textureAtlas->addTexture(static_cast<uint32_t>(id), p.string());
    };

    addTex(Logic::TextureID::Note, "note.note");
    addTex(Logic::TextureID::Node, "note.node");
    addTex(Logic::TextureID::HoldEnd, "note.holdend");
    addTex(Logic::TextureID::HoldBodyVertical, "note.holdbodyvertical");
    addTex(Logic::TextureID::HoldBodyHorizontal, "note.holdbodyhorizontal");
    addTex(Logic::TextureID::FlickArrowLeft, "note.arrowleft");
    addTex(Logic::TextureID::FlickArrowRight, "note.arrowright");
    addTex(Logic::TextureID::Track, "panel.track");

    m_textureAtlas->build(2048);

    m_atlasUVs.clear();
    for ( uint32_t i = static_cast<uint32_t>(Logic::TextureID::None);
          i <= static_cast<uint32_t>(Logic::TextureID::Track);
          ++i ) {
        if ( i == static_cast<uint32_t>(Logic::TextureID::Background) )
            continue;
        m_atlasUVs[i] = m_textureAtlas->getUV(i);
    }

    m_needReload = false;
}

const std::vector<Graphic::Vertex::VKBasicVertex>&
PreviewCanvas::getVertices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->vertices;
    }
    static std::vector<Graphic::Vertex::VKBasicVertex> empty;
    return empty;
}

const std::vector<uint32_t>& PreviewCanvas::getIndices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->indices;
    }
    static std::vector<uint32_t> empty;
    return empty;
}

void PreviewCanvas::onRecordDrawCmds(vk::CommandBuffer& cmdBuf,
                                     vk::PipelineLayout pipelineLayout,
                                     vk::DescriptorSet  defaultDescriptor)
{
    if ( !m_currentSnapshot ) return;

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        atlasDescriptor = (vk::DescriptorSet)(VkDescriptorSet)
                              m_textureAtlas->getImTextureID();
    }

    vk::DescriptorSet lastBound = VK_NULL_HANDLE;

    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        vk::DescriptorSet tex = m_atlasUVs.count(cmd.customTextureId)
                                    ? atlasDescriptor
                                    : defaultDescriptor;

        if ( tex != lastBound ) {
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &tex,
                                      0,
                                      nullptr);
            lastBound = tex;
        }
        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

}  // namespace MMM::Canvas
