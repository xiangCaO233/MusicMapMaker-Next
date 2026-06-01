#include "canvas/TimelineCanvas.h"
#include "canvas/TimeFormatUtils.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "ui/Icons.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string_view>

namespace MMM::Canvas
{
namespace
{
/// @brief Timeline 画布齿轮按钮的类型信息
struct TimelineGearInfo {
    uint32_t     mask;
    entt::entity Logic::TimelineInteractiveElement::* entity;
    double Logic::TimelineInteractiveElement::* value;
    const char*                                 label;
    const char*                                 editType;
    ImVec4                                      color;
    float                                       xRatio;
};
}  // namespace

TimelineCanvas::TimelineCanvas(
    const std::string& name, uint32_t w, uint32_t h,
    std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer)
    : UI::IUIView(name)
    , UI::IRenderableView(name)
    , m_canvasName(name)
    , m_syncBuffer(std::move(syncBuffer))
{
    m_targetWidth  = w;
    m_targetHeight = h;
}

void TimelineCanvas::update(UI::UIManager* sourceManager)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float                dpiScale = viewport->DpiScale;

    std::string windowName =
        fmt::format("{}###{}", TR("canvas.timeline"), m_name);

    UI::LayoutContext lctx(
        m_layoutCtx, windowName, true, ImGuiWindowFlags_NoScrollbar);

    ImVec2 size = ImGui::GetContentRegionAvail();

    if ( m_currentSnapshot ) {
        // 1. 绘制垂直音频时间滚动条及时间点表格按钮
        if ( m_currentSnapshot->hasBeatmap &&
             m_currentSnapshot->totalTime > 0.0 ) {
            float time  = static_cast<float>(m_currentSnapshot->currentTime);
            float total = static_cast<float>(m_currentSnapshot->totalTime);

            float sliderWidth  = 24.0f;
            float sliderHeight = size.y;

            ImGui::BeginGroup();

            ImVec2 sliderSize(sliderWidth, sliderHeight);
            if ( ImGui::VSliderFloat("##AudioTimeSlider",
                                     sliderSize,
                                     &time,
                                     0.0f,
                                     total,
                                     "") ) {
                float visualOffset = Config::AppConfig::instance()
                                         .getVisualConfig()
                                         .getEffectiveVisualOffset();
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdSeek{
                        static_cast<double>(time) - visualOffset }));
            }

            if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
                const auto timeText =
                    formatCanvasTimePair(static_cast<double>(time),
                                         static_cast<double>(total),
                                         m_currentSnapshot);
                ImGui::SetTooltip("%s", timeText.c_str());
            }

            ImGui::EndGroup();
            ImGui::SameLine();
        }

        // 2. 扣除 slider 空间后剩下的空间绘制画布
        size   = ImGui::GetContentRegionAvail();
        size.x = std::floor(size.x);
        size.y = std::floor(size.y);

        if ( size.x > 0 && size.y > 0 ) {
            setTargetSize(static_cast<uint32_t>(size.x),
                          static_cast<uint32_t>(size.y),
                          dpiScale);
        }

        vk::DescriptorSet texID = getDescriptorSet();
        if ( texID != VK_NULL_HANDLE ) {
            ImGui::Image((ImTextureID)(VkDescriptorSet)texID, size);

            bool  isHovered = ImGui::IsItemHovered();
            float wheel     = ImGui::GetIO().MouseWheel;
            if ( isHovered && std::abs(wheel) > 0.01f &&
                 !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt ) {
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdScroll{
                        m_name, -wheel, ImGui::GetIO().KeyShift }));
            }

            // 3. 处理右键点击创建事件
            if ( isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
                handleRightClick(size);
            }

            // 4. 绘制交互层元件 (齿轮按钮)
            ImVec2 canvasPos = ImGui::GetItemRectMin();
            ImVec2 mousePos  = ImGui::GetMousePos();
            float  iconSize  = 20.0f;
            float  padding   = 5.0f;

            auto& visual    = Config::AppConfig::instance().getVisualConfig();
            float proximity = visual.snapThreshold;

            UI::Utils::pushFixedButtonStyleVars();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));

            bool isFocused =
                ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

            for ( const auto& el : m_currentSnapshot->timelineElements ) {
                float localMouseY = mousePos.y - canvasPos.y;
                float mappedY     = el.y;
                bool  isNear      = std::abs(localMouseY - mappedY) < proximity;

                if ( isNear && isFocused ) {
                    const TimelineGearInfo gears[] = {
                        { Logic::System::SCROLL_EFFECT_BPM,
                          &Logic::TimelineInteractiveElement::bpmEntity,
                          &Logic::TimelineInteractiveElement::bpmValue,
                          "BPM",
                          "BPM",
                          ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                          0.0f },
                        { Logic::System::SCROLL_EFFECT_SCROLL,
                          &Logic::TimelineInteractiveElement::scrollEntity,
                          &Logic::TimelineInteractiveElement::scrollValue,
                          "Scroll",
                          "Scroll",
                          ImVec4(0.2f, 1.0f, 0.2f, 1.0f),
                          1.0f },
                        { Logic::System::SCROLL_EFFECT_JUMP,
                          &Logic::TimelineInteractiveElement::jumpEntity,
                          &Logic::TimelineInteractiveElement::jumpValue,
                          "Jump",
                          "Jump",
                          ImVec4(0.2f, 0.45f, 1.0f, 1.0f),
                          0.33f },
                        { Logic::System::SCROLL_EFFECT_HS,
                          &Logic::TimelineInteractiveElement::hsEntity,
                          &Logic::TimelineInteractiveElement::hsValue,
                          "HS",
                          "HS",
                          ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                          0.66f },
                    };

                    for ( const auto& gear : gears ) {
                        if ( (el.effects & gear.mask) == 0 ) continue;

                        float x = canvasPos.x + padding;
                        if ( gear.xRatio >= 0.99f ) {
                            x = canvasPos.x + size.x - iconSize - padding;
                        } else if ( gear.xRatio > 0.0f ) {
                            x = canvasPos.x + padding +
                                (size.x - iconSize - 2.0f * padding) *
                                    gear.xRatio;
                        }

                        ImVec2 pos(x, canvasPos.y + mappedY - iconSize * 0.5f);
                        ImGui::SetCursorScreenPos(pos);

                        ImGui::PushStyleColor(ImGuiCol_Text, gear.color);
                        auto        entity = el.*(gear.entity);
                        std::string id =
                            fmt::format("{}_{}_{}",
                                        gear.label,
                                        el.time,
                                        static_cast<uint32_t>(entity));
                        if ( ImGui::Button(
                                 (std::string(UI::ICON_MMM_COG) + "##" + id)
                                     .c_str(),
                                 ImVec2(iconSize, iconSize)) ) {
                            XINFO("{} gear clicked at time: {}",
                                  gear.label,
                                  el.time);
                            m_editingEntity = entity;
                            m_editTime      = el.time;
                            m_editValue     = el.*(gear.value);
                            if ( std::string_view(gear.editType) == "Scroll" ) {
                                bool isMalodyScroll = false;
                                if ( auto session =
                                         Logic::EditorEngine::instance()
                                             .getActiveSession() ) {
                                    auto& registry =
                                        session->getContext().timelineRegistry;
                                    if ( registry.valid(entity) &&
                                         registry
                                             .all_of<Logic::TimelineComponent>(
                                                 entity) ) {
                                        const auto& tl =
                                            registry
                                                .get<Logic::TimelineComponent>(
                                                    entity);
                                        isMalodyScroll =
                                            tl.m_metadata.timing_properties
                                                .contains(
                                                    ::MMM::TimingMetadataType::
                                                        MALODY);
                                    }
                                }
                                if ( !isMalodyScroll && m_editValue < -1e-6 ) {
                                    m_editValue = -100.0 / m_editValue;
                                }
                            }
                            m_editType    = gear.editType;
                            m_isPopupOpen = true;
                            ImGui::OpenPopup("TimelineEventEditor");
                        }
                        ImGui::PopStyleColor();

                        if ( ImGui::IsItemHovered() ) {
                            const auto timeText =
                                formatCanvasTime(el.time, m_currentSnapshot);
                            ImGui::SetTooltip(
                                "%s Event: %s", gear.label, timeText.c_str());
                        }
                    }
                }
            }
            ImGui::PopStyleColor(2);
            UI::Utils::popFixedButtonStyleVars();

            // 绘制右上角时间点面板汉堡按钮
            ImVec2 menuBtnPos = ImVec2(canvasPos.x + size.x - 30.0f - 10.0f,
                                       canvasPos.y + 10.0f);
            ImGui::SetCursorScreenPos(menuBtnPos);

            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.15f, 0.18f, 0.22f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(0.25f, 0.28f, 0.32f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(0.35f, 0.38f, 0.42f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 15.0f);
            UI::Utils::pushFixedButtonStyleVars();
            if ( ImGui::Button(UI::ICON_MMM_BARS, ImVec2(30.0f, 30.0f)) ) {
                m_isTableWindowOpen = !m_isTableWindowOpen;
            }
            UI::Utils::popFixedButtonStyleVars();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            if ( ImGui::IsItemHovered() ) {
                ImGui::SetTooltip(
                    "%s",
                    TR("ui.timeline.timing_points_table_btn_tooltip").data());
            }

            // 5. 渲染弹窗

            renderEventEditorPopup();
            renderEventCreationPopup();
            renderTimingPointsTableWindow();
        }
    }
}

const std::vector<Graphic::Vertex::VKBasicVertex>&
TimelineCanvas::getVertices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->vertices;
    }
    static std::vector<Graphic::Vertex::VKBasicVertex> empty;
    return empty;
}

const std::vector<uint32_t>& TimelineCanvas::getIndices() const
{
    if ( m_currentSnapshot ) {
        return m_currentSnapshot->indices;
    }
    static std::vector<uint32_t> empty;
    return empty;
}

bool TimelineCanvas::isDirty() const
{
    return true;
}

/// @brief 判断当前帧是否需要准备时间线快照。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要准备时返回 true。
bool TimelineCanvas::needsParallelUiPrepare(
    const UI::UiFrameSnapshot& snapshot) const
{
    (void)snapshot;
    return m_syncBuffer && m_isOpen;
}

/// @brief 在线程池中拉取并准备时间线快照。
/// @param snapshot 当前帧 UI 快照。
void TimelineCanvas::prepareUiFrameData(const UI::UiFrameSnapshot& snapshot)
{
    (void)snapshot;
    m_preparedSnapshot = prepareCanvasSnapshot(
        m_syncBuffer.get(), m_lastOffsetSnapshot, m_lastAppliedYOffset, false);
    m_hasPreparedSnapshot = true;
}

/// @brief 将准备好的时间线快照切换到主线程可见状态。
void TimelineCanvas::swapPreparedUiFrameData()
{
    if ( !m_hasPreparedSnapshot ) {
        return;
    }

    m_currentSnapshot     = m_preparedSnapshot.snapshot;
    m_lastOffsetSnapshot  = m_preparedSnapshot.offsetSnapshot;
    m_lastAppliedYOffset  = m_preparedSnapshot.appliedYOffset;
    m_hasPreparedSnapshot = false;

    if ( !m_currentSnapshot ) {
        m_lastOffsetSnapshot = nullptr;
        m_lastAppliedYOffset = 0.0f;
    }
}

void TimelineCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                                uint32_t h) const
{
    Event::CanvasResizeEvent e;
    e.canvasName = m_name;
    e.lastSize   = { oldW, oldH };
    e.newSize    = { w, h };
    Event::EventBus::instance().publish(e);
}

std::vector<std::string> TimelineCanvas::getShaderSources(
    const std::string& shader_name)
{
    if ( m_shaderSourceCache.count(shader_name) )
        return m_shaderSourceCache[shader_name];

    // Timeline 仅使用主着色器，不支持效果着色器（无模糊/发光后处理）
    if ( shader_name != "main" ) return {};

    auto canvas_config =
        Config::SkinManager::instance().getCanvasConfig("Basic2DCanvas");
    auto it = canvas_config.canvas_shader_modules.find("main");
    if ( it != canvas_config.canvas_shader_modules.end() ) {
        auto        path = it->second;
        std::string vert = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "VertexShader.spv"));
        std::string frag = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "FragmentShader.spv"));
        m_shaderSourceCache[shader_name] = { vert, frag };
        return m_shaderSourceCache[shader_name];
    }
    return {};
}

std::string TimelineCanvas::getShaderName(const std::string& shader_module_name)
{
    return m_name + ":" + shader_module_name;
}

bool TimelineCanvas::needReload()
{
    return std::exchange(m_needReload, false);
}

void TimelineCanvas::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                    vk::Device&         logicalDevice,
                                    vk::CommandPool& cmdPool, vk::Queue& queue)
{
    m_textureAtlas = std::make_unique<Graphic::VKTextureAtlas>(
        physicalDevice, logicalDevice, cmdPool, queue);

    unsigned char white[] = { 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255 };
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::None), white, 2, 2);

    auto& skin           = Config::SkinManager::instance();
    auto  notePath       = skin.getAssetPath("note.note");
    bool  hasNoteTexture = false;
    if ( !notePath.empty() ) {
        m_textureAtlas->addTexture(
            static_cast<uint32_t>(Logic::TextureID::Note), notePath);
        hasNoteTexture = true;
    }

    m_textureAtlas->build(1024);

    m_atlasUVs.clear();
    m_atlasUVs[static_cast<uint32_t>(Logic::TextureID::None)] =
        m_textureAtlas->getUV(static_cast<uint32_t>(Logic::TextureID::None));
    if ( hasNoteTexture ) {
        m_atlasUVs[static_cast<uint32_t>(Logic::TextureID::Note)] =
            m_textureAtlas->getUV(
                static_cast<uint32_t>(Logic::TextureID::Note));
    }

    Logic::EditorEngine::instance().setAtlasUVMap(m_canvasName, m_atlasUVs);
}

void TimelineCanvas::onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
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
        atlasDescriptor =
            m_textureAtlas->getNativeDescriptorSet(pool, setLayout);
    }

    vk::DescriptorSet lastBound = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        vk::DescriptorSet tex = m_atlasUVs.count(cmd.customTextureId)
                                    ? atlasDescriptor
                                    : defaultDescriptor;
        if ( tex == VK_NULL_HANDLE ) {
            tex = defaultDescriptor;
        }

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
