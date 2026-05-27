#include "ui/imgui/manager/ToolbarView.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace MMM::UI
{

ToolbarView::ToolbarView(const std::string& name) : IUIView(name) {}

void ToolbarView::update(UIManager* sourceManager)
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();

    // 从逻辑引擎同步当前工具状态
    m_currentTool = Logic::EditorEngine::instance().getCurrentTool();

    // 样式锁定
    auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;

    float windowPadding = std::floor(aesthetics.windowPadding * dpiScale);

    // 强制固定宽度 (增加 12px 左右各 6px 补白)
    float fixedBaseW   = 32.0f;
    float toolbarBaseW = fixedBaseW * dpiScale;
    float fixedW       = std::floor(fixedBaseW * dpiScale);
    float btnSize      = toolbarBaseW;
    float totalFixedW  = fixedW + 2.0f * windowPadding;

    // 2. 锁定窗口尺寸约束
    ImGui::SetNextWindowSizeConstraints(ImVec2(totalFixedW, -1),
                                        ImVec2(totalFixedW, -1));

    // 3. 核心标志
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking;

    float rounding = std::floor(aesthetics.frameRounding * dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

    auto pushBtnStyle = [&](bool active) {
        if ( active ) {
            ImVec4 activeCol = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
            ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
        } else {
            Utils::UIThemeUtils::pushTransparentButtonStyles();
        }
    };

    float windowRound = std::floor(aesthetics.windowRounding * dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(windowPadding, windowPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    if ( ImGui::Begin(" ###Toolbar", nullptr, flags) ) {
        bool pushedIconFont = false;
        if ( auto f = skinCfg.getFont("pure_icons") ) {
            ImGui::PushFont(f);
            pushedIconFont = true;
        }

        const float itemSpacing = std::floor(aesthetics.itemSpacing * dpiScale);
        auto&       engine      = Logic::EditorEngine::instance();
        const auto& editorCfg   = engine.getEditorConfig();

        auto advanceItem = [&]() {
            if ( itemSpacing > 0.0f ) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + itemSpacing);
            }
        };

        auto drawToggleButton = [&](const char* icon,
                                    bool        active,
                                    const char* tooltip,
                                    auto        applyChange) {
            pushBtnStyle(active);
            if ( ImGui::Button(icon, ImVec2(btnSize, btnSize)) ) {
                auto newConfig = editorCfg;
                applyChange(newConfig);
                engine.setEditorConfig(newConfig);
            }
            drawTooltip(tooltip);
            ImGui::PopStyleColor(3);
            advanceItem();
        };

        auto drawRuntimeToggleButton = [&](const char* icon,
                                           bool        active,
                                           const char* tooltip,
                                           auto        applyChange) {
            pushBtnStyle(active);
            if ( ImGui::Button(icon, ImVec2(btnSize, btnSize)) ) {
                applyChange(!active);
            }
            drawTooltip(tooltip);
            ImGui::PopStyleColor(3);
            advanceItem();
        };

        drawToolButton(ICON_MMM_HAND,
                       Logic::EditTool::Move,
                       TR("ui.toolbar.move"),
                       btnSize);
        advanceItem();
        drawToolButton(ICON_MMM_SQUARE_SELECT,
                       Logic::EditTool::Marquee,
                       TR("ui.toolbar.marquee"),
                       btnSize);
        advanceItem();
        drawToolButton(ICON_MMM_PEN,
                       Logic::EditTool::Draw,
                       TR("ui.toolbar.draw"),
                       btnSize);
        advanceItem();

        ImVec2 sepPos = ImGui::GetCursorScreenPos();
        float  sepH   = 2.0f * dpiScale;
        ImGui::GetWindowDrawList()->AddLine(
            { sepPos.x + 4.0f * dpiScale, sepPos.y + sepH * 0.5f },
            { sepPos.x + btnSize - 4.0f * dpiScale, sepPos.y + sepH * 0.5f },
            IM_COL32(100, 100, 100, 150),
            1.0f * dpiScale);
        ImGui::Dummy(ImVec2(btnSize, sepH));
        advanceItem();

        drawToggleButton(ICON_MMM_ARROWS_UP_DOWN,
                         editorCfg.settings.reverseScroll,
                         TR("ui.toolbar.reverse_scroll").data(),
                         [](Config::EditorConfig& config) {
                             config.settings.reverseScroll =
                                 !config.settings.reverseScroll;
                         });

        drawToggleButton(ICON_MMM_MAGNET,
                         editorCfg.settings.scrollSnap,
                         TR("ui.toolbar.scroll_snap").data(),
                         [](Config::EditorConfig& config) {
                             config.settings.scrollSnap =
                                 !config.settings.scrollSnap;
                         });

        drawToggleButton(ICON_MMM_ARROW_DOWN,
                         editorCfg.settings.snapFloor,
                         TR("ui.toolbar.snap_floor").data(),
                         [](Config::EditorConfig& config) {
                             config.settings.snapFloor =
                                 !config.settings.snapFloor;
                         });

        drawToggleButton(ICON_MMM_EYE,
                         !editorCfg.visual.enableLinearScrollMapping,
                         TR("ui.toolbar.scroll_timing_mapping").data(),
                         [](Config::EditorConfig& config) {
                             config.visual.enableLinearScrollMapping =
                                 !config.visual.enableLinearScrollMapping;
                         });

        drawToggleButton(ICON_MMM_BARS,
                         editorCfg.visual.drawBeatLines,
                         TR("ui.toolbar.draw_beat_lines").data(),
                         [](Config::EditorConfig& config) {
                             config.visual.drawBeatLines =
                                 !config.visual.drawBeatLines;
                         });

        drawToggleButton(ICON_MMM_STOP,
                         editorCfg.settings.stopPlaybackOnScroll,
                         TR("ui.toolbar.stop_on_scroll").data(),
                         [](Config::EditorConfig& config) {
                             config.settings.stopPlaybackOnScroll =
                                 !config.settings.stopPlaybackOnScroll;
                         });

        drawToggleButton(ICON_MMM_VISUAL_EFFECTS,
                         editorCfg.visual.enableHitEffects,
                         TR("ui.toolbar.hit_effects").data(),
                         [](Config::EditorConfig& config) {
                             config.visual.enableHitEffects =
                                 !config.visual.enableHitEffects;
                         });

        drawRuntimeToggleButton(
            ICON_MMM_LINK,
            engine.isSyncSameMainAudioCanvasesEnabled(),
            TR("ui.toolbar.sync_same_main_audio").data(),
            [&engine](bool enabled) {
                engine.setSyncSameMainAudioCanvases(enabled);
            });

        float bottomButtonsH = btnSize * 2.0f + itemSpacing;
        float bottomStartY = ImGui::GetCursorPosY() +
                             ImGui::GetContentRegionAvail().y - bottomButtonsH;
        if ( bottomStartY > ImGui::GetCursorPosY() ) {
            ImGui::SetCursorPosY(bottomStartY);
        }

        {
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();

            bool hasBeatmap = session && session->getContext().currentBeatmap;
            int  currentTracks = 4;
            if ( hasBeatmap ) {
                currentTracks =
                    session->getContext()
                        .currentBeatmap->m_baseMapMetadata.track_count;
            }

            if ( !hasBeatmap ) {
                ImGui::BeginDisabled();
            }

            pushBtnStyle(m_showKeyPopup);
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) ImGui::PushFont(contentFont);

            char keyBuf[16];
            if ( hasBeatmap ) {
                snprintf(keyBuf, sizeof(keyBuf), "%dK", currentTracks);
            } else {
                snprintf(keyBuf, sizeof(keyBuf), "--");
            }

            if ( ImGui::Button(keyBuf, ImVec2(btnSize, btnSize)) ) {
                m_showKeyPopup = !m_showKeyPopup;
                if ( m_showKeyPopup ) {
                    m_showDivisorPopup = false;
                }
            }
            m_lastKeyBtnY = ImGui::GetItemRectMin().y;

            if ( hasBeatmap && ImGui::IsItemHovered() ) {
                float wheel = ImGui::GetIO().MouseWheel;
                if ( std::abs(wheel) > 0.1f ) {
                    int delta     = (wheel > 0) ? 1 : -1;
                    int newTracks = std::clamp(currentTracks + delta, 1, 32);
                    if ( newTracks != currentTracks ) {
                        auto meta = session->getContext()
                                        .currentBeatmap->m_baseMapMetadata;
                        meta.track_count = newTracks;
                        engine.pushCommand(
                            Logic::CmdUpdateBeatmapMetadata{ meta });
                    }
                }
                drawTooltip(TR("ui.settings.beatmap.tracks").data());
            }

            if ( contentFont ) ImGui::PopFont();
            ImGui::PopStyleColor(3);

            if ( !hasBeatmap ) {
                ImGui::EndDisabled();
            }
        }
        advanceItem();

        {
            int currentDivisor = editorCfg.settings.beatDivisor;
            pushBtnStyle(m_showDivisorPopup);
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) ImGui::PushFont(contentFont);
            char divisorBuf[16];
            snprintf(divisorBuf, sizeof(divisorBuf), "%d", currentDivisor);
            if ( ImGui::Button(divisorBuf, ImVec2(btnSize, btnSize)) ) {
                m_showDivisorPopup = !m_showDivisorPopup;
                if ( m_showDivisorPopup ) {
                    m_showKeyPopup = false;
                }
            }
            m_lastBtnY = ImGui::GetItemRectMin().y;
            if ( ImGui::IsItemHovered() ) {
                float wheel = ImGui::GetIO().MouseWheel;
                if ( std::abs(wheel) > 0.1f ) {
                    int delta = (wheel > 0) ? 1 : -1;
                    if ( ImGui::GetIO().KeyShift )
                        delta *= static_cast<int>(
                            editorCfg.settings.scrollSpeedMultiplier);
                    int newDivisor = std::clamp(currentDivisor + delta, 1, 64);
                    if ( newDivisor != currentDivisor ) {
                        auto newConfig                 = editorCfg;
                        newConfig.settings.beatDivisor = newDivisor;
                        engine.setEditorConfig(newConfig);
                    }
                }

                drawTooltip(TR("ui.toolbar.beat_divisor").data());
            }
            if ( contentFont ) ImGui::PopFont();
            ImGui::PopStyleColor(3);
        }

        if ( pushedIconFont ) ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleVar(3);  // WindowPadding, WindowBorderSize, WindowRounding

    // --- 绘制分拍数量设置悬浮窗 ---
    if ( m_showDivisorPopup ) {
        // 在 Toolbar 窗口左侧显示悬浮窗

        ImVec2 toolbarPos = ImGui::FindWindowByName(" ###Toolbar")->Pos;

        ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        float          viewportTop  = mainViewport->Pos.y;
        float viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
        float viewportLeft   = mainViewport->Pos.x;

        // X = 工具栏左边缘往左 4px
        // Y = 按钮的顶部对齐
        float targetX = toolbarPos.x - std::floor(4.0f * dpiScale);
        float targetY = m_lastBtnY;

        // 灵活微调 Y 和 X 的起始坐标，确保弹出菜单不会溢出视口边界而被截断
        float popupW =
            m_popupWidth > 0.0f ? m_popupWidth : std::floor(160.0f * dpiScale);
        float popupH  = m_popupHeight > 0.0f ? m_popupHeight
                                             : std::floor(120.0f * dpiScale);
        float padding = std::floor(8.0f * dpiScale);

        // 限制 X 以免溢出左侧边界
        targetX = std::max(targetX, viewportLeft + popupW + padding);
        // 限制 Y 以免溢出底部与顶部边界
        targetY = std::min(targetY, viewportBottom - popupH - padding);
        targetY = std::max(targetY, viewportTop + padding);

        ImVec2 popupPos = ImVec2(targetX, targetY);

        // Pivot(1.0, 0.0) 代表将弹窗的右上角对齐到 popupPos
        ImGui::SetNextWindowViewport(mainViewport->ID);
        ImGui::SetNextWindowPos(popupPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));

        ImGuiWindowFlags popupFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;

        auto& aesthetics =
            Config::AppConfig::instance().getEditorSettings().aesthetics;
        float winPadding    = std::floor(aesthetics.windowPadding * dpiScale);
        float winRounding   = std::floor(aesthetics.windowRounding * dpiScale);
        float frameRounding = std::floor(aesthetics.frameRounding * dpiScale);
        float itemSpacing   = std::floor(aesthetics.itemSpacing * dpiScale);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, winRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(winPadding, winPadding));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(itemSpacing, itemSpacing));

        if ( ImGui::Begin("##BeatDivisorPopup", nullptr, popupFlags) ) {
            auto editorCfg = Logic::EditorEngine::instance().getEditorConfig();
            int  currentDivisor = editorCfg.settings.beatDivisor;

            ImGui::TextUnformatted(TR("ui.toolbar.beat_divisor"));
            ImGui::Separator();

            ImGui::SetNextItemWidth(std::floor(120.0f * dpiScale));
            if ( ImGui::SliderInt("##DivisorSlider", &currentDivisor, 1, 64) ) {
                auto newConfig                 = editorCfg;
                newConfig.settings.beatDivisor = currentDivisor;
                Logic::EditorEngine::instance().setEditorConfig(newConfig);
            }
            if ( ImGui::IsItemHovered() ) {
                Utils::renderTooltip(
                    TR("ui.settings.editor.beat_divisor_tooltip").data(),
                    Utils::TooltipDir::Right);
            }

            // 可以加一些常用的快速设置按钮
            const auto& commonDivisors =
                Config::SkinManager::instance().getCommonDivisors();
            for ( size_t i = 0; i < commonDivisors.size(); ++i ) {
                if ( i > 0 && i % 4 != 0 ) ImGui::SameLine();
                char buf[16];
                snprintf(buf, sizeof(buf), "1/%d", commonDivisors[i]);
                if ( ImGui::Button(buf,
                                   ImVec2(std::floor(35.0f * dpiScale),
                                          std::floor(24.0f * dpiScale))) ) {
                    auto newConfig                 = editorCfg;
                    newConfig.settings.beatDivisor = commonDivisors[i];
                    Logic::EditorEngine::instance().setEditorConfig(newConfig);
                }
            }

            // 实时获取并记录当前帧计算出的真实尺寸，供下一帧定位计算参考，防止视口越界截断
            ImVec2 sz     = ImGui::GetWindowSize();
            m_popupWidth  = sz.x;
            m_popupHeight = sz.y;
        }
        ImGui::End();

        ImGui::PopStyleVar(4);
    }

    // --- 绘制Key数量设置悬浮窗 ---
    if ( m_showKeyPopup ) {
        ImVec2 toolbarPos = ImGui::FindWindowByName(" ###Toolbar")->Pos;

        ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        float          viewportTop  = mainViewport->Pos.y;
        float viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
        float viewportLeft   = mainViewport->Pos.x;

        // X = 工具栏左边缘往左 4px
        // Y = 按钮的顶部对齐
        float targetX = toolbarPos.x - std::floor(4.0f * dpiScale);
        float targetY = m_lastKeyBtnY;

        float popupW  = m_keyPopupWidth > 0.0f ? m_keyPopupWidth
                                               : std::floor(160.0f * dpiScale);
        float popupH  = m_keyPopupHeight > 0.0f ? m_keyPopupHeight
                                                : std::floor(120.0f * dpiScale);
        float padding = std::floor(8.0f * dpiScale);

        targetX = std::max(targetX, viewportLeft + popupW + padding);
        targetY = std::min(targetY, viewportBottom - popupH - padding);
        targetY = std::max(targetY, viewportTop + padding);

        ImVec2 popupPos = ImVec2(targetX, targetY);

        ImGui::SetNextWindowViewport(mainViewport->ID);
        ImGui::SetNextWindowPos(popupPos, ImGuiCond_Always, ImVec2(1.0f, 0.0f));

        ImGuiWindowFlags popupFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize;

        auto& aesthetics =
            Config::AppConfig::instance().getEditorSettings().aesthetics;
        float winPadding    = std::floor(aesthetics.windowPadding * dpiScale);
        float winRounding   = std::floor(aesthetics.windowRounding * dpiScale);
        float frameRounding = std::floor(aesthetics.frameRounding * dpiScale);
        float itemSpacing   = std::floor(aesthetics.itemSpacing * dpiScale);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, winRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(winPadding, winPadding));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(itemSpacing, itemSpacing));

        if ( ImGui::Begin("##KeyCountPopup", nullptr, popupFlags) ) {
            auto& engine = Logic::EditorEngine::instance();
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();

            if ( session && session->getContext().currentBeatmap ) {
                auto meta =
                    session->getContext().currentBeatmap->m_baseMapMetadata;
                int currentTracks = meta.track_count;

                ImGui::TextUnformatted(TR("ui.settings.beatmap.tracks").data());
                ImGui::Separator();

                ImGui::SetNextItemWidth(std::floor(120.0f * dpiScale));
                if ( ImGui::SliderInt(
                         "##TracksSlider", &currentTracks, 1, 32) ) {
                    meta.track_count = currentTracks;
                    engine.pushCommand(Logic::CmdUpdateBeatmapMetadata{ meta });
                }

                // 常用 Key 数快速设置按钮
                const std::vector<int> commonKeys = { 4, 5, 6, 7, 8 };
                for ( size_t i = 0; i < commonKeys.size(); ++i ) {
                    if ( i > 0 ) ImGui::SameLine();
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%dK", commonKeys[i]);
                    if ( ImGui::Button(buf,
                                       ImVec2(std::floor(28.0f * dpiScale),
                                              std::floor(24.0f * dpiScale))) ) {
                        meta.track_count = commonKeys[i];
                        engine.pushCommand(
                            Logic::CmdUpdateBeatmapMetadata{ meta });
                    }
                }
            } else {
                m_showKeyPopup = false;
            }

            ImVec2 sz        = ImGui::GetWindowSize();
            m_keyPopupWidth  = sz.x;
            m_keyPopupHeight = sz.y;
        }
        ImGui::End();

        ImGui::PopStyleVar(4);
    }
}

void ToolbarView::drawToolButton(const char* icon, Logic::EditTool tool,
                                 const char* tooltip, float width)
{
    Config::SkinManager& skinCfg  = Config::SkinManager::instance();
    bool                 isActive = (m_currentTool == tool);

    // 按钮高度与宽度保持一致，形成正方形
    float btnSize = width;


    if ( isActive ) {
        ImVec4 activeCol = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
    } else {
        Utils::UIThemeUtils::pushTransparentButtonStyles();
    }

    if ( ImGui::Button(icon, ImVec2(btnSize, btnSize)) ) {
        if ( m_currentTool != tool ) {
            m_currentTool = tool;
            Logic::EditorEngine::instance().pushCommand(
                Logic::CmdChangeTool{ tool });
        }
    }

    drawTooltip(tooltip);

    ImGui::PopStyleColor(3);
}

void ToolbarView::drawTooltip(const char* text)
{
    Utils::renderTooltip(text, Utils::TooltipDir::Left);
}

}  // namespace MMM::UI
