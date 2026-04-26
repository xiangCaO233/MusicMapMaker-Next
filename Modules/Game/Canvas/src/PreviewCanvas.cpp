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
#include "graphic/imguivk/VKContext.h"
#include <filesystem>
#include <glm/glm.hpp>
#include <utility>
#include <chrono>
#include <algorithm>

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

    if ( m_currentSnapshot ) {
        // --- 亚帧时间补偿 (直接修改动态顶点 Y 坐标) ---
        float newYOffset = 0.0f;

        if ( m_currentSnapshot->isPlaying &&
             m_currentSnapshot->snapshotSysTime > 0.0 ) {
            double now =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            double dt = now - m_currentSnapshot->snapshotSysTime;
            if ( dt > 0.0 && dt < 0.1 ) {
                double      scrollSpeed  = 500.0;
                double      snapshotTime = m_currentSnapshot->currentTime;
                const auto& segs         = m_currentSnapshot->scrollSegments;
                if ( !segs.empty() ) {
                    auto it = std::upper_bound(
                        segs.begin(),
                        segs.end(),
                        snapshotTime,
                        [](double                              val,
                           const Logic::System::ScrollSegment& seg) {
                            return val < seg.time;
                        });
                    if ( it != segs.begin() ) {
                        --it;
                    }
                    scrollSpeed = it->speed;
                }
                // 关键点：预览区的偏移需要乘上预览区的缩放倍率 renderScaleY
                newYOffset = static_cast<float>(
                    dt * m_currentSnapshot->playbackSpeed * scrollSpeed *
                    m_currentSnapshot->renderScaleY);
            }
        }

        // 应用顶点级 Y 偏移 (仅修改动态顶点: 标尺刻度、事件线等)
        uint32_t startVtx = m_currentSnapshot->staticVertexCount;
        auto&    vertices  = m_currentSnapshot->vertices;
        uint32_t endVtx   = m_currentSnapshot->dynamicVertexCount > 0
                                ? (startVtx + m_currentSnapshot->dynamicVertexCount)
                                : static_cast<uint32_t>(vertices.size());

        // 如果是同一个快照被复用，先撤销上一帧的偏移
        if ( m_lastOffsetSnapshot == m_currentSnapshot &&
             std::abs(m_lastAppliedYOffset) > 0.0001f ) {
            for ( size_t i = startVtx; i < endVtx && i < vertices.size(); ++i ) {
                vertices[i].pos.y -= m_lastAppliedYOffset;
            }
        }

        // 应用新偏移
        if ( std::abs(newYOffset) > 0.0001f ) {
            for ( size_t i = startVtx; i < endVtx && i < vertices.size(); ++i ) {
                vertices[i].pos.y += newYOffset;
            }
        }

        m_lastOffsetSnapshot = m_currentSnapshot;
        m_lastAppliedYOffset = newYOffset;
    } else {
        m_lastOffsetSnapshot = nullptr;
        m_lastAppliedYOffset = 0.0f;
    }

    // --- 交互：发送鼠标位置指令给逻辑线程 ---
    ImVec2 mousePos      = ImGui::GetMousePos();
    ImVec2 windowPos     = ImGui::GetCursorScreenPos();
    ImVec2 localMousePos = { mousePos.x - windowPos.x,
                             mousePos.y - windowPos.y };
    bool   isHovered     = ImGui::IsWindowHovered();
    bool   isDragging    = ImGui::IsMouseDragging(0);

    Event::EventBus::instance().publish(
        Event::LogicCommandEvent(Logic::CmdSetMousePosition{ m_cameraId,
                                                             localMousePos.x,
                                                             localMousePos.y,
                                                             isHovered,
                                                             isDragging }));

    // --- 拖拽提示：告知用户松手时跳转的位置 ---
    if ( isDragging && m_currentSnapshot &&
         m_currentSnapshot->isPreviewDragging ) {
        ImGui::SetTooltip(TR("canvas.preview.jump_to"),
                          m_currentSnapshot->previewHoverTime);
    }

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

void PreviewCanvas::onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                                     vk::PipelineLayout      pipelineLayout,
                                     vk::DescriptorSetLayout setLayout,
                                     vk::DescriptorSet       defaultDescriptor,
                                     uint32_t                frameIndex)
{
    if ( !m_currentSnapshot ) return;

    auto& renderer = Graphic::VKContext::get().value().get().getRenderer();
    auto  pool     = renderer.getDescriptorPool();

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        atlasDescriptor = m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
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
            vk::Rect2D physicalScissor = getPhysicalScissor(cmd.scissor);
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

}  // namespace MMM::Canvas
