#include "canvas/Basic2DCanvas.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "event/canvas/interactive/ResizeEvent.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/ITextureLoader.h"
#include "ui/IUIView.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include <algorithm>
#include <filesystem>
#include <glm/glm.hpp>
#include <utility>
#include <vector>

namespace MMM::Canvas
{
Basic2DCanvas::Basic2DCanvas(
    const std::string& name, uint32_t w, uint32_t h,
    std::shared_ptr<Logic::BeatmapSyncBuffer> syncBuffer,
    const std::string&                        cameraId)
    : IUIView(name)
    , IRenderableView(name)
    , m_canvasName(name)
    , m_cameraId(cameraId.empty() ? name : cameraId)
    , m_syncBuffer(std::move(syncBuffer))
{
    m_targetWidth  = w;
    m_targetHeight = h;

    m_dropSubId = Event::EventBus::instance().subscribe<Event::GLFWDropEvent>(
        [this](const Event::GLFWDropEvent& e) {
            XINFO("Basic2DCanvas received GLFWDropEvent with {} paths",
                  e.paths.size());
            m_pendingDrops.push_back({ e.paths, e.pos });
        });
}

Basic2DCanvas::~Basic2DCanvas()
{
    Event::EventBus::instance().unsubscribe<Event::GLFWDropEvent>(m_dropSubId);
}

// 接口实现
void Basic2DCanvas::update(UI::UIManager* sourceManager)
{
    std::string windowName =
        fmt::format("{}###{}", TR("canvas.editor"), m_canvasName);
    UI::LayoutContext lctx(m_layoutCtx, windowName);
    RenderContext     rctx(
        this, m_canvasName.c_str(), m_targetWidth, m_targetHeight);

    // 处理拖拽
    if ( !m_pendingDrops.empty() ) {
        bool isHovered = ImGui::IsWindowHovered(
            ImGuiHoveredFlags_RootAndChildWindows |
            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 winPos   = ImGui::GetWindowPos();
        ImVec2 winSize  = ImGui::GetWindowSize();

        XINFO(
            "Canvas [{}] checking {} drops. Hovered: {}, Mouse:({}, {}), "
            "WinPos:({}, {}), WinSize:({}, {})",
            m_canvasName,
            m_pendingDrops.size(),
            isHovered,
            mousePos.x,
            mousePos.y,
            winPos.x,
            winPos.y,
            winSize.x,
            winSize.y);

        if ( isHovered ) {
            for ( const auto& drop : m_pendingDrops ) {
                if ( !drop.paths.empty() ) {
                    std::filesystem::path p(drop.paths[0]);
                    std::filesystem::path projectPath =
                        std::filesystem::is_directory(p) ? p : p.parent_path();
                    auto ext = p.extension().string();

                    XINFO("File dropped on Canvas: {}, opening project: {}",
                          p.string(),
                          projectPath.string());

                    // 1. 打开项目
                    Event::OpenProjectEvent ev;
                    ev.m_projectPath = projectPath;
                    Event::EventBus::instance().publish(ev);

                    // 2. 跳转到谱面管理器
                    Event::UISubViewToggleEvent evt;
                    evt.sourceUiName           = m_canvasName;
                    evt.uiManager              = sourceManager;
                    evt.targetFloatManagerName = "SideBarManager";
                    evt.subViewId =
                        UI::TabToSubViewId(UI::SideBarTab::BeatMapExplorer);
                    evt.showSubView = true;
                    Event::EventBus::instance().publish(evt);

                    // 3. 如果是谱面文件，直接加载
                    if ( ext == ".osu" || ext == ".imd" || ext == ".mc" ) {
                        XINFO("Auto-loading beatmap from drop: {}",
                              p.filename().string());
                        try {
                            auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
                                MMM::BeatMap::loadFromFile(p));
                            Logic::EditorEngine::instance().pushCommand(
                                Logic::CmdLoadBeatmap{ loadedBeatmap });
                        } catch ( const std::exception& e ) {
                            XERROR("Failed to load dropped beatmap: {}",
                                   e.what());
                        }
                    }
                }
            }
        }
        m_pendingDrops.clear();
    }

    // 尝试拉取最新的逻辑线程渲染快照 (专属摄像机缓冲)
    if ( m_syncBuffer ) {
        m_currentSnapshot = m_syncBuffer->pullLatestSnapshot();
    }

    // --- 快捷键处理：仅主画布响应空格切换播放/暂停 ---
    if ( m_currentSnapshot ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_Space, false) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetPlayState{ !m_currentSnapshot->isPlaying }));
        }

        // --- 快捷键：工具切换 (V/E: Move, M/Q: Marquee, P/W: Draw, C/R: Cut) ---
        if ( ImGui::IsKeyPressed(ImGuiKey_V, false) ||
             ImGui::IsKeyPressed(ImGuiKey_E, false) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdChangeTool{ Logic::EditTool::Move }));
        } else if ( ImGui::IsKeyPressed(ImGuiKey_M, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Q, false) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdChangeTool{ Logic::EditTool::Marquee }));
        } else if ( ImGui::IsKeyPressed(ImGuiKey_P, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_W, false) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdChangeTool{ Logic::EditTool::Draw }));
        } else if ( ImGui::IsKeyPressed(ImGuiKey_C, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_R, false) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdChangeTool{ Logic::EditTool::Cut }));
        }
    }

    if ( m_currentSnapshot &&
         m_currentSnapshot->backgroundPath != m_loadedBgPath ) {
        m_loadedBgPath = m_currentSnapshot->backgroundPath;
        if ( m_physicalDevice && m_logicalDevice && m_cmdPool && m_queue &&
             !m_loadedBgPath.empty() &&
             std::filesystem::exists(m_loadedBgPath) ) {
            try {
                m_bgTexture =
                    std::make_unique<Graphic::VKTexture>(m_loadedBgPath,
                                                         m_physicalDevice,
                                                         m_logicalDevice,
                                                         m_cmdPool,
                                                         m_queue);
                XINFO("Loaded background texture: {}", m_loadedBgPath);
            } catch ( const std::exception& e ) {
                XERROR("Failed to load background texture: {}", e.what());
                m_bgTexture.reset();
            }
        } else {
            m_bgTexture.reset();
        }
    }

    // --- MVP 交互实现：拾取测试 ---
    if ( m_currentSnapshot ) {
        // 获取鼠标在 ImGui 窗口中的相对位置
        ImVec2 mousePos = ImGui::GetMousePos();

        // 由于 Basic2DCanvas 是绘制在全屏 Dock 窗口中的 Image，
        // 我们需要计算相对于窗口内容的本地坐标。
        ImVec2 windowPos     = ImGui::GetCursorScreenPos();
        ImVec2 localMousePos = { mousePos.x - windowPos.x,
                                 mousePos.y - windowPos.y };

        // --- 交互：发送鼠标位置指令给逻辑线程 (用于工具提示等) ---
        bool isHovered  = ImGui::IsWindowHovered();
        bool isDragging = ImGui::IsMouseDragging(0);
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdSetMousePosition{ m_cameraId,
                                        localMousePos.x,
                                        localMousePos.y,
                                        isHovered,
                                        isDragging }));

        // --- 交互：显示精确时间戳工具提示 ---
        if ( isHovered && m_currentSnapshot->isHoveringCanvas &&
             !m_currentSnapshot->isPlaying ) {
            auto& visual = Config::AppConfig::instance().getVisualConfig();
            auto& layout = visual.trackLayout;

            // 转换 localMousePos 到归一化比例 (0.0~1.0)
            float normX = localMousePos.x / m_targetWidth;
            float normY = localMousePos.y / m_targetHeight;

            // 只有当鼠标在视觉配置定义的轨道布局范围内时才显示 Tooltip
            bool isInTrackLayout =
                (normX >= layout.left && normX <= layout.right &&
                 normY >= layout.top && normY <= layout.bottom);

            if ( isInTrackLayout ) {
                bool isEditTool =
                    (m_currentSnapshot->currentTool != Logic::EditTool::Move &&
                     m_currentSnapshot->currentTool !=
                         Logic::EditTool::Marquee);

                if ( m_currentSnapshot->isSnapped || isEditTool ||
                     m_currentSnapshot->hoveredNoteNumerator > 0 ) {
                    ImGui::SetNextWindowPos(
                        ImVec2(mousePos.x + 15, mousePos.y + 15));
                    ImGui::SetNextWindowBgAlpha(0.7f);
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                        ImVec2(12, 12));
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                        ImVec2(8, 6));

                    ImGui::BeginTooltip();

                    if ( m_currentSnapshot->hoveredNoteNumerator > 0 ) {
                        ImGui::TextColored(
                            ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                            "%s: %d/%d",
                            TR("ui.canvas.note_fraction").data(),
                            m_currentSnapshot->hoveredNoteNumerator,
                            m_currentSnapshot->hoveredNoteDenominator);
                        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                                           "%s: %.3f s",
                                           TR("ui.canvas.note_time").data(),
                                           m_currentSnapshot->hoveredNoteTime);
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                    }

                    if ( m_currentSnapshot->isSnapped ) {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                           "%s: %.3f s",
                                           TR("ui.canvas.snap").data(),
                                           m_currentSnapshot->snappedTime);

                        if ( m_currentSnapshot->snappedNumerator == 1 &&
                             m_currentSnapshot->snappedDenominator == 1 ) {
                            ImGui::TextColored(
                                ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
                                "%s (1/1)",
                                TR("ui.canvas.beat_fraction").data());
                        } else {
                            ImGui::TextColored(
                                ImVec4(0.8f, 0.9f, 1.0f, 1.0f),
                                "%s (%d/%d)",
                                TR("ui.canvas.beat_fraction").data(),
                                m_currentSnapshot->snappedNumerator,
                                m_currentSnapshot->snappedDenominator);
                        }
                    } else {
                        ImGui::Text("%s: %.3f s",
                                    TR("ui.canvas.time").data(),
                                    m_currentSnapshot->hoveredTime);
                    }

                    if ( m_currentSnapshot->hoveredBeatIndex > 0 ) {
                        ImGui::Text("%s: %d",
                                    TR("ui.canvas.beat_index").data(),
                                    m_currentSnapshot->hoveredBeatIndex);
                    }

                    ImGui::Text("%s: %d",
                                TR("ui.canvas.track").data(),
                                m_currentSnapshot->hoveredTrack + 1);

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f),
                                       "%s: %d",
                                       TR("ui.canvas.beat_divisor").data(),
                                       m_currentSnapshot->currentBeatDivisor);

                    ImGui::EndTooltip();
                    ImGui::PopStyleVar(2);
                }
            }
        }

        entt::entity hoveredEntity   = entt::null;
        uint8_t      hoveredPart     = 0;
        int          hoveredSubIndex = -1;

        // 倒序遍历 Hitbox (最上层的优先)
        for ( auto it = m_currentSnapshot->hitboxes.rbegin();
              it != m_currentSnapshot->hitboxes.rend();
              ++it ) {
            if ( localMousePos.x >= it->x && localMousePos.x <= it->x + it->w &&
                 localMousePos.y >= it->y &&
                 localMousePos.y <= it->y + it->h ) {
                hoveredEntity   = it->entity;
                hoveredPart     = static_cast<uint8_t>(it->part);
                hoveredSubIndex = it->subIndex;
                break;
            }
        }

        // 推送悬停指令到逻辑线程
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdSetHoveredEntity{
                hoveredEntity, hoveredPart, hoveredSubIndex }));

        // --- 交互：点击选择与拖拽 ---
        if ( ImGui::IsMouseClicked(0) ) {
            if ( hoveredEntity != entt::null ) {
                // 点击了实体，发送选择指令
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdSelectEntity{
                        hoveredEntity, !ImGui::GetIO().KeyCtrl }));

                // 如果没在拖拽，则尝试开始拖拽
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdStartDrag{ hoveredEntity, m_cameraId }));
            } else {
                // 点击空白处，取消所有选择
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdSelectEntity{ entt::null, true }));
            }
        }

        if ( ImGui::IsMouseDragging(0) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdUpdateDrag{
                    m_cameraId, localMousePos.x, localMousePos.y }));
        }

        if ( ImGui::IsMouseReleased(0) ) {
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdEndDrag{ m_cameraId }));
        }

        // --- 交互：鼠标滚轮控制时间跳转与属性修改 ---
        float wheel = ImGui::GetIO().MouseWheel;
        if ( isHovered && std::abs(wheel) > 0.01f ) {
            bool isCtrlPressed  = ImGui::GetIO().KeyCtrl;
            bool isAltPressed   = ImGui::GetIO().KeyAlt;
            bool isShiftPressed = ImGui::GetIO().KeyShift;

            if ( isCtrlPressed ) {
                auto editorCfg =
                    Logic::EditorEngine::instance().getEditorConfig();
                int   direction     = editorCfg.settings.reverseScroll ? -1 : 1;
                float adjustedWheel = wheel * direction;
                float step          = 0.1f;
                if ( isShiftPressed )
                    step *= editorCfg.settings.scrollSpeedMultiplier;
                editorCfg.visual.timelineZoom += adjustedWheel * step;
                editorCfg.visual.timelineZoom =
                    std::clamp(editorCfg.visual.timelineZoom, 0.1f, 10.0f);
                Logic::EditorEngine::instance().setEditorConfig(editorCfg);
            } else if ( isAltPressed ) {
                auto editorCfg =
                    Logic::EditorEngine::instance().getEditorConfig();
                int   direction     = editorCfg.settings.reverseScroll ? -1 : 1;
                float adjustedWheel = wheel * direction;

                static std::unordered_map<std::string, float> wheelAccumulator;
                float& acc = wheelAccumulator[m_cameraId];
                acc += adjustedWheel;

                int steps = 0;
                if ( acc >= 1.0f ) {
                    steps = static_cast<int>(acc);
                    acc -= steps;
                } else if ( acc <= -1.0f ) {
                    steps = static_cast<int>(acc);
                    acc -= steps;
                }

                if ( steps != 0 ) {
                    if ( isShiftPressed ) {
                        const std::vector<int> presets = { 1, 2, 3,  4,
                                                           6, 8, 12, 16 };
                        int current = editorCfg.settings.beatDivisor;

                        if ( steps > 0 ) {
                            for ( int i = 0; i < steps; ++i ) {
                                auto it = std::upper_bound(
                                    presets.begin(), presets.end(), current);
                                if ( it != presets.end() ) {
                                    current = *it;
                                } else {
                                    current = presets.back();
                                }
                            }
                        } else {
                            for ( int i = 0; i < -steps; ++i ) {
                                auto it = std::lower_bound(
                                    presets.begin(), presets.end(), current);
                                if ( it != presets.begin() ) {
                                    current = *(--it);
                                } else {
                                    current = presets.front();
                                }
                            }
                        }
                        editorCfg.settings.beatDivisor = current;
                    } else {
                        // 正常模式下，如果按住 Ctrl 等（其实这里 logic 只处理
                        // Alt）， 如果有加速需求逻辑（虽然此处 user 没提 Alt
                        // 模式也加速，但为了统一倍率感） 暂时保持原样，仅在
                        // Scroll 指令中应用倍率
                        editorCfg.settings.beatDivisor += steps;
                    }
                    editorCfg.settings.beatDivisor =
                        std::clamp(editorCfg.settings.beatDivisor, 1, 64);
                    Logic::EditorEngine::instance().setEditorConfig(editorCfg);
                }
            } else {
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdScroll{ m_cameraId, -wheel, isShiftPressed }));
            }
        }
    }
}

// --- 改变尺寸后的回调 ---
void Basic2DCanvas::resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                               uint32_t h) const
{
    Event::CanvasResizeEvent e;
    e.canvasName = m_cameraId;  // 使用 cameraId 告知逻辑线程哪个视口变了
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

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        // 核心修复：强制通过 getImTextureID 确保描述符集已在 ImGui 中注册
        atlasDescriptor = (vk::DescriptorSet)(VkDescriptorSet)
                              m_textureAtlas->getImTextureID();
    }

    vk::DescriptorSet lastBoundTexture = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

    for ( const auto& cmd : m_currentSnapshot->cmds ) {
        vk::DescriptorSet actualTexture = cmd.texture;

        // 核心修复：只要该 ID 在图集 UV 映射表中（包含 ID 0），就使用图集
        if ( m_atlasUVs.count(cmd.customTextureId) ) {
            actualTexture = atlasDescriptor;
        } else if ( cmd.customTextureId ==
                    static_cast<uint32_t>(Logic::TextureID::Background) ) {
            if ( m_bgTexture ) {
                actualTexture = (vk::DescriptorSet)(VkDescriptorSet)
                                    m_bgTexture->getImTextureID();
            }
        }

        if ( actualTexture == VK_NULL_HANDLE ) {
            actualTexture = defaultDescriptor;
        }

        // 避免冗余绑定
        if ( actualTexture != lastBoundTexture ) {
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &actualTexture,
                                      0,
                                      nullptr);
            lastBoundTexture = actualTexture;
        }

        if ( cmd.scissor != lastScissor ) {
            cmdBuf.setScissor(0, 1, &cmd.scissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

void Basic2DCanvas::onRecordGlowCmds(vk::CommandBuffer& cmdBuf,
                                     vk::PipelineLayout pipelineLayout,
                                     vk::DescriptorSet  defaultDescriptor)
{
    if ( !m_currentSnapshot ) return;

    vk::DescriptorSet atlasDescriptor = VK_NULL_HANDLE;
    if ( m_textureAtlas ) {
        atlasDescriptor = (vk::DescriptorSet)(VkDescriptorSet)
                              m_textureAtlas->getImTextureID();
    }

    vk::DescriptorSet lastBoundTexture = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

    for ( const auto& cmd : m_currentSnapshot->glowCmds ) {
        vk::DescriptorSet actualTexture = cmd.texture;

        if ( m_atlasUVs.count(cmd.customTextureId) ) {
            actualTexture = atlasDescriptor;
        } else if ( cmd.customTextureId ==
                    static_cast<uint32_t>(Logic::TextureID::Background) ) {
            if ( m_bgTexture ) {
                actualTexture = (vk::DescriptorSet)(VkDescriptorSet)
                                    m_bgTexture->getImTextureID();
            }
        }

        if ( actualTexture == VK_NULL_HANDLE ) {
            actualTexture = defaultDescriptor;
        }

        if ( actualTexture != lastBoundTexture ) {
            cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      pipelineLayout,
                                      0,
                                      1,
                                      &actualTexture,
                                      0,
                                      nullptr);
            lastBoundTexture = actualTexture;
        }

        if ( cmd.scissor != lastScissor ) {
            cmdBuf.setScissor(0, 1, &cmd.scissor);
            lastScissor = cmd.scissor;
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
    if ( m_shaderSourceCache.count(shader_name) )
        return m_shaderSourceCache[shader_name];

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
    m_physicalDevice = physicalDevice;
    m_logicalDevice  = logicalDevice;
    m_cmdPool        = cmdPool;
    m_queue          = queue;

    auto& skin = Config::SkinManager::instance();

    m_textureAtlas = std::make_unique<Graphic::VKTextureAtlas>(
        physicalDevice, logicalDevice, cmdPool, queue);

    // 注册所有纹理到图集
    // 1. 注册 4x4 纯白纹理作为 None (用于绘制纯色几何体而不切换 DrawCall)
    // 使用 4x4 配合 UV 收缩可以彻底防止线性过滤导致的采样溢出
    unsigned char white[] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                              255, 255, 255, 255 };
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::None), white, 4, 4);



    // 2. 注册资源纹理
    m_textureAtlas->addTexture(static_cast<uint32_t>(Logic::TextureID::Note),
                               skin.getAssetPath("note.note").generic_string());
    m_textureAtlas->addTexture(static_cast<uint32_t>(Logic::TextureID::Node),
                               skin.getAssetPath("note.node").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::HoldEnd),
        skin.getAssetPath("note.holdend").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::HoldBodyVertical),
        skin.getAssetPath("note.holdbodyvertical").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::HoldBodyHorizontal),
        skin.getAssetPath("note.holdbodyhorizontal").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::FlickArrowLeft),
        skin.getAssetPath("note.arrowleft").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::FlickArrowRight),
        skin.getAssetPath("note.arrowright").generic_string());

    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::Track),
        skin.getAssetPath("panel.track.background").generic_string());
    m_textureAtlas->addTexture(
        static_cast<uint32_t>(Logic::TextureID::JudgeArea),
        skin.getAssetPath("panel.track.judgearea").generic_string());
    m_textureAtlas->addTexture(static_cast<uint32_t>(Logic::TextureID::Logo),
                               skin.getAssetPath("logo").generic_string());

    // 自动加载所有序列帧资源，并使用 SkinManager 分配好的 ID
    for ( const auto& [key, seq] : skin.getData().effectSequences ) {
        uint32_t currentId = seq.startId;
        for ( const auto& frame : seq.frames ) {
            m_textureAtlas->addTexture(currentId++, frame.generic_string());
        }
    }

    // 构建图集
    m_textureAtlas->build(4096);

    // 更新 UV 映射缓存并同步给逻辑线程
    m_atlasUVs.clear();
    for ( uint32_t i = static_cast<uint32_t>(Logic::TextureID::None);
          i <= static_cast<uint32_t>(Logic::TextureID::Logo);
          ++i ) {
        // 背景不进入合图，跳过其 UV 记录，防止逻辑层错误识别
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
    XINFO("Basic2DCanvas textures reloaded into atlas for camera: " +
          m_cameraId);
}

}  // namespace MMM::Canvas
