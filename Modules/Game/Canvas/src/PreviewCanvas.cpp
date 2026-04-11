#include "canvas/PreviewCanvas.h"
#include "common/LogicCommands.h"
#include "config/skin/SkinConfig.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
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
PreviewCanvas::PreviewCanvas(
    const std::string& name, uint32_t w, uint32_t h,
    std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer)
    : IUIView(name)
    , IRenderableView(name)
    , m_canvasName(name)
    , m_syncBuffer(std::move(syncBuffer))
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
    if ( m_syncBuffer ) {
        m_currentSnapshot = m_syncBuffer->pullLatestSnapshot();
    }

    // --- 交互：发送鼠标位置指令给逻辑线程 ---
    ImVec2 mousePos      = ImGui::GetMousePos();
    ImVec2 windowPos     = ImGui::GetCursorScreenPos();
    ImVec2 localMousePos = { mousePos.x - windowPos.x,
                             mousePos.y - windowPos.y };
    bool   isHovered     = ImGui::IsWindowHovered();

    Event::EventBus::instance().publish(
        Event::LogicCommandEvent(Logic::CmdSetMousePosition{
            m_cameraId, localMousePos.x, localMousePos.y, isHovered }));

    // --- 跳转时间逻辑 ---
    if ( m_currentSnapshot && isHovered ) {
        // 核心修复：仅在鼠标松开时触发跳转。
        // 不论是单击还是拖拽，均在松开的一刻根据当时鼠标位置的时间戳进行 Seek。
        if ( ImGui::IsMouseReleased(0) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSeek{ m_currentSnapshot->hoveredTime }));
        }

        // --- 交互：滚轮调整预览区倍率 ---
        float wheel = ImGui::GetIO().MouseWheel;
        if ( std::abs(wheel) > 0.01f ) {
            auto  editorCfg = Logic::EditorEngine::instance().getEditorConfig();
            float step      = 0.5f;
            if ( ImGui::GetIO().KeyShift ) step *= 2.0f;

            // 增大 areaRatio
            // 代表显示更多内容（缩小），减小代表显示更少内容（放大）
            // 习惯上向上滚动为放大（减小倍率）
            editorCfg.visual.previewConfig.areaRatio -= wheel * step;
            editorCfg.visual.previewConfig.areaRatio = std::clamp(
                editorCfg.visual.previewConfig.areaRatio, 1.0f, 50.0f);

            Logic::EditorEngine::instance().setEditorConfig(editorCfg);
        }
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
            m_textureAtlas->addTexture(static_cast<uint32_t>(id),
                                       p.generic_string());
    };

    addTex(Logic::TextureID::Note, "note.note");
    addTex(Logic::TextureID::Node, "note.node");
    addTex(Logic::TextureID::HoldEnd, "note.holdend");
    addTex(Logic::TextureID::HoldBodyVertical, "note.holdbodyvertical");
    addTex(Logic::TextureID::HoldBodyHorizontal, "note.holdbodyhorizontal");
    addTex(Logic::TextureID::FlickArrowLeft, "note.arrowleft");
    addTex(Logic::TextureID::FlickArrowRight, "note.arrowright");
    addTex(Logic::TextureID::Track, "panel.track.background");
    addTex(Logic::TextureID::JudgeArea, "panel.track.judgearea");
    addTex(Logic::TextureID::Logo, "logo");

    // 自动加载所有序列帧资源，并使用 SkinManager 分配好的 ID
    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        uint32_t currentId = seq.startId;
        for ( const auto& frame : seq.frames ) {
            m_textureAtlas->addTexture(currentId++, frame.generic_string());
        }
    }

    m_textureAtlas->build(4096);

    m_atlasUVs.clear();
    for ( uint32_t i = static_cast<uint32_t>(Logic::TextureID::None);
          i <= static_cast<uint32_t>(Logic::TextureID::Logo);
          ++i ) {
        if ( i == static_cast<uint32_t>(Logic::TextureID::Background) )
            continue;
        m_atlasUVs[i] = m_textureAtlas->getUV(i);
    }

    // 更新特序列帧 UV
    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        for ( uint32_t i = 0; i < seq.frames.size(); ++i ) {
            uint32_t id    = seq.startId + i;
            m_atlasUVs[id] = m_textureAtlas->getUV(id);
        }
    }

    Logic::EditorEngine::instance().setAtlasUVMap(m_cameraId, m_atlasUVs);

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
    vk::Rect2D        lastScissor;

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

        if ( cmd.scissor != lastScissor ) {
            cmdBuf.setScissor(0, 1, &cmd.scissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

}  // namespace MMM::Canvas
