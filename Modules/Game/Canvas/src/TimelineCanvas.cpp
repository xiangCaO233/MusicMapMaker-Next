#include "canvas/TimelineCanvas.h"
#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "ui/Icons.h"
#include "graphic/imguivk/VKContext.h"
#include <filesystem>
#include <cmath>
#include <chrono>
#include <algorithm>

namespace MMM::Canvas
{

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

    UI::LayoutContext lctx(m_layoutCtx, windowName, true, 0);

    ImVec2 size = ImGui::GetContentRegionAvail();

    if ( m_syncBuffer ) {
        m_currentSnapshot = m_syncBuffer->pullLatestSnapshot();
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
                    newYOffset = static_cast<float>(
                        dt * m_currentSnapshot->playbackSpeed * scrollSpeed);
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

            // 1. 绘制垂直音频时间滚动条
            if ( m_currentSnapshot->hasBeatmap &&
                 m_currentSnapshot->totalTime > 0.0 ) {
                float time = static_cast<float>(m_currentSnapshot->currentTime);
                float total = static_cast<float>(m_currentSnapshot->totalTime);

                float  sliderWidth = 20.0f;
                ImVec2 sliderSize(sliderWidth, size.y);
                if ( ImGui::VSliderFloat("##AudioTimeSlider",
                                         sliderSize,
                                         &time,
                                         0.0f,
                                         total,
                                         "") ) {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdSeek{ static_cast<double>(time) }));
                }

                if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%.3f / %.3f s", time, total);
                }

                ImGui::SameLine();
            }

            // 2. 扣除 slider 空间后剩下的空间绘制画布
            size = ImGui::GetContentRegionAvail();
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
                if ( isHovered && std::abs(wheel) > 0.01f ) {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(Logic::CmdScroll{
                            m_name, -wheel, ImGui::GetIO().KeyShift }));
                }

                // 3. 处理右键点击创建事件
                if ( isHovered &&
                     ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
                    handleRightClick(size);
                }

                // 4. 绘制交互层元件 (齿轮按钮)
                ImVec2 canvasPos = ImGui::GetItemRectMin();
                ImVec2 mousePos  = ImGui::GetMousePos();
                float  iconSize  = 20.0f;
                float  padding   = 5.0f;

                auto& visual = Config::AppConfig::instance().getVisualConfig();
                float proximity = visual.snapThreshold;

                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(0, 0, 0, 0));

                bool isFocused =
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

                for ( const auto& el : m_currentSnapshot->timelineElements ) {
                    float localMouseY = mousePos.y - canvasPos.y;
                    float mappedY     = el.y;
                    bool  isNear      = std::abs(localMouseY - mappedY) < proximity;

                    if ( isNear && isFocused ) {
                        // BPM 齿轮 (左侧)
                        if ( el.effects & Logic::System::SCROLL_EFFECT_BPM ) {
                            ImVec2 pos(canvasPos.x + padding,
                                       canvasPos.y + mappedY - iconSize * 0.5f);
                            ImGui::SetCursorScreenPos(pos);

                            ImGui::PushStyleColor(
                                ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
                            std::string id = fmt::format("BPM_{}", el.time);
                            if ( ImGui::Button(
                                     (std::string(UI::ICON_MMM_COG) + "##" + id)
                                         .c_str(),
                                     ImVec2(iconSize, iconSize)) ) {
                                XINFO("BPM gear clicked at time: {}", el.time);
                                m_editingEntity = el.bpmEntity;
                                m_editTime      = el.time;
                                m_editValue     = el.bpmValue;
                                m_editType      = "BPM";
                                m_isPopupOpen   = true;
                                ImGui::OpenPopup("TimelineEventEditor");
                            }
                            ImGui::PopStyleColor();

                            if ( ImGui::IsItemHovered() ) {
                                ImGui::SetTooltip("BPM Event: %.3f s", el.time);
                            }
                        }

                        // Scroll 齿轮 (右侧)
                        if ( el.effects &
                             Logic::System::SCROLL_EFFECT_SCROLL ) {
                            ImVec2 pos(
                                canvasPos.x + size.x - iconSize - padding,
                                canvasPos.y + mappedY - iconSize * 0.5f);
                            ImGui::SetCursorScreenPos(pos);

                            ImGui::PushStyleColor(
                                ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
                            std::string id = fmt::format("SCROLL_{}", el.time);
                            if ( ImGui::Button(
                                     (std::string(UI::ICON_MMM_COG) + "##" + id)
                                         .c_str(),
                                     ImVec2(iconSize, iconSize)) ) {
                                XINFO("Scroll gear clicked at time: {}",
                                      el.time);
                                m_editingEntity = el.scrollEntity;
                                m_editTime      = el.time;
                                m_editValue   = (el.scrollValue < -1e-6)
                                                    ? (-100.0 / el.scrollValue)
                                                    : el.scrollValue;
                                m_editType    = "Scroll";
                                m_isPopupOpen = true;
                                ImGui::OpenPopup("TimelineEventEditor");
                            }
                            ImGui::PopStyleColor();

                            if ( ImGui::IsItemHovered() ) {
                                ImGui::SetTooltip("Scroll Event: %.3f s",
                                                  el.time);
                            }
                        }
                    }
                }
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar();

                // 5. 渲染弹窗
                renderEventEditorPopup();
                renderEventCreationPopup();
            }
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

    auto canvas_config =
        Config::SkinManager::instance().getCanvasConfig("Basic2DCanvas");
    auto it = canvas_config.canvas_shader_modules.find("main");
    if ( it != canvas_config.canvas_shader_modules.end() ) {
        auto        path = it->second;
        std::string vert =
            Graphic::VKShader::readFile((path / "VertexShader.spv").string());
        std::string frag =
            Graphic::VKShader::readFile((path / "FragmentShader.spv").string());
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
}

void TimelineCanvas::onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                                      vk::PipelineLayout      pipelineLayout,
                                      vk::DescriptorSetLayout setLayout,
                                      vk::DescriptorSet       defaultDescriptor,
                                      uint32_t                frameIndex)
{
    if ( !m_currentSnapshot ) return;

    vk::DescriptorSet lastBound = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        // Timeline 目前主要用纯色，使用 defaultDescriptor
        vk::DescriptorSet tex = defaultDescriptor;

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

} // namespace MMM::Canvas
