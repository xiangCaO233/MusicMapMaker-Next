#include "canvas/TimelineCanvas.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/system/ScrollCache.h"
#include <filesystem>

namespace MMM::Canvas
{

TimelineCanvas::TimelineCanvas(const std::string& name, uint32_t w, uint32_t h)
    : UI::IUIView(name), UI::IRenderableView(name), m_canvasName(name)
{
    m_targetWidth  = w;
    m_targetHeight = h;
}

void TimelineCanvas::update(UI::UIManager* sourceManager)
{
    std::string windowName =
        fmt::format("{}###{}", TR("canvas.timeline"), m_name);

    ImGui::Begin(windowName.c_str());

    ImVec2 size = ImGui::GetContentRegionAvail();

    auto syncBuffer = Logic::EditorEngine::instance().getSyncBuffer(m_name);
    if ( syncBuffer ) {
        m_currentSnapshot = syncBuffer->pullLatestSnapshot();
        if ( m_currentSnapshot ) {
            // 在画布左侧绘制一个垂直的全局音频时间滚动条
            if ( m_currentSnapshot->hasBeatmap &&
                 m_currentSnapshot->totalTime > 0.0 ) {
                float time = static_cast<float>(m_currentSnapshot->currentTime);
                float total = static_cast<float>(m_currentSnapshot->totalTime);

                ImVec2 sliderSize(20.0f, size.y);
                if ( ImGui::VSliderFloat("##AudioTimeSlider",
                                         sliderSize,
                                         &time,
                                         0.0f,
                                         total,
                                         "") ) {
                    Logic::EditorEngine::instance().pushCommand(
                        Logic::CmdSeek{ static_cast<double>(time) });
                }

                if ( ImGui::IsItemActive() || ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%.3f / %.3f s", time, total);
                }

                ImGui::SameLine();
            }

            // 扣除掉上面 slider 占据的空间后剩下的空间绘制画布
            size = ImGui::GetContentRegionAvail();
            if ( size.x > 0 && size.y > 0 ) {
                setTargetSize(static_cast<uint32_t>(size.x),
                              static_cast<uint32_t>(size.y));
            }

            vk::DescriptorSet texID = getDescriptorSet();
            if ( texID != VK_NULL_HANDLE ) {
                ImGui::Image((ImTextureID)(VkDescriptorSet)texID, size);

                // 在 ImGui Image 之上绘制交互层
                ImVec2 canvasPos = ImGui::GetItemRectMin();
                ImVec2 mousePos  = ImGui::GetMousePos();
                auto*  drawList  = ImGui::GetWindowDrawList();
                float  iconSize  = 20.0f;
                float  padding   = 5.0f;
                float  proximity = 5.0f;  // 5像素检测范围

                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(0, 0, 0, 0));

                for ( const auto& el : m_currentSnapshot->timelineElements ) {
                    float localMouseY = mousePos.y - canvasPos.y;
                    bool  isNear = std::abs(localMouseY - el.y) < proximity;

                    if ( isNear ) {
                        // BPM 齿轮 (左侧)
                        if ( el.effects & Logic::System::SCROLL_EFFECT_BPM ) {
                            ImVec2 pos(canvasPos.x + padding,
                                       canvasPos.y + el.y - iconSize * 0.5f);
                            ImGui::SetCursorScreenPos(pos);

                            ImGui::PushStyleColor(
                                ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
                            std::string id = fmt::format("BPM_{}", el.time);
                            if ( ImGui::Button(
                                     (std::string("\xef\x80\x93##") + id)
                                         .c_str(),
                                     ImVec2(iconSize, iconSize)) ) {
                                XINFO("BPM gear clicked at time: {}", el.time);
                                // TODO: 发送事件或打开编辑弹窗
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
                                canvasPos.y + el.y - iconSize * 0.5f);
                            ImGui::SetCursorScreenPos(pos);

                            ImGui::PushStyleColor(
                                ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
                            std::string id = fmt::format("SCROLL_{}", el.time);
                            if ( ImGui::Button(
                                     (std::string("\xef\x80\x93##") + id)
                                         .c_str(),
                                     ImVec2(iconSize, iconSize)) ) {
                                XINFO("Scroll gear clicked at time: {}",
                                      el.time);
                                // TODO: 发送事件或打开编辑弹窗
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
            }
        }
    }

    ImGui::End();
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

void TimelineCanvas::onRecordDrawCmds(vk::CommandBuffer& cmdBuf,
                                      vk::PipelineLayout pipelineLayout,
                                      vk::DescriptorSet  defaultDescriptor)
{
    if ( !m_currentSnapshot || m_currentSnapshot->indices.empty() ) return;

    cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                              pipelineLayout,
                              0,
                              1,
                              &defaultDescriptor,
                              0,
                              nullptr);

    cmdBuf.drawIndexed(
        static_cast<uint32_t>(m_currentSnapshot->indices.size()), 1, 0, 0, 0);
}

}  // namespace MMM::Canvas
