#include "ui/imgui/manager/ToolbarView.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <imgui_internal.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace MMM::UI
{

namespace
{
/// @brief 将颜色槽位转换为数组索引。
std::size_t colorSlotIndex(Logic::NoteColorSlot slot)
{
    return static_cast<std::size_t>(slot);
}

/// @brief 获取颜色槽位对应的翻译键。
const char* colorSlotLabelKey(Logic::NoteColorSlot slot)
{
    switch ( slot ) {
    case Logic::NoteColorSlot::Tap: return "ui.toolbar.note_palette.note";
    case Logic::NoteColorSlot::Head: return "ui.toolbar.note_palette.note_head";
    case Logic::NoteColorSlot::Hold: return "ui.toolbar.note_palette.note_hold";
    case Logic::NoteColorSlot::End: return "ui.toolbar.note_palette.note_end";
    case Logic::NoteColorSlot::FlickArrow:
        return "ui.toolbar.note_palette.note_flick_arrow";
    case Logic::NoteColorSlot::Node: return "ui.toolbar.note_palette.note_node";
    }
    return "ui.toolbar.note_palette.note";
}

/// @brief 获取颜色槽位对应的皮肤颜色键。
const char* colorSlotSkinKey(Logic::NoteColorSlot slot)
{
    switch ( slot ) {
    case Logic::NoteColorSlot::Tap: return "note_tap";
    case Logic::NoteColorSlot::Head: return "note_head";
    case Logic::NoteColorSlot::Hold: return "note_hold";
    case Logic::NoteColorSlot::End: return "note_end";
    case Logic::NoteColorSlot::FlickArrow: return "note_flick_arrow";
    case Logic::NoteColorSlot::Node: return "note_node";
    }
    return "note_tap";
}

/// @brief 获取颜色槽位对应的皮肤颜色，兼容旧皮肤的头部和尾部颜色。
Config::Color skinColorForSlot(Logic::NoteColorSlot slot)
{
    auto& skin = Config::SkinManager::instance();
    if ( slot == Logic::NoteColorSlot::Head &&
         !skin.getData().colors.contains("note_head") ) {
        return skin.getColor("note_hold");
    }
    if ( slot == Logic::NoteColorSlot::End &&
         !skin.getData().colors.contains("note_end") ) {
        return skin.getColor("note_hold");
    }
    return skin.getColor(colorSlotSkinKey(slot));
}

/// @brief 将皮肤颜色转换为 glm 颜色。
glm::vec4 toVec4(const Config::Color& color)
{
    return { color.r, color.g, color.b, color.a };
}

/// @brief 将 glm 颜色转换为 ImGui 颜色。
ImVec4 toImVec4(glm::vec4 color)
{
    return { color.r, color.g, color.b, color.a };
}

/// @brief 将 glm 颜色转换为配置存储颜色。
std::array<float, 4> toStoredColor(glm::vec4 color)
{
    return { color.r, color.g, color.b, color.a };
}

/// @brief 将配置存储颜色转换为 glm 颜色。
glm::vec4 fromStoredColor(const std::array<float, 4>& color)
{
    return { color[0], color[1], color[2], color[3] };
}

/// @brief 将 0 到 1 的颜色通道转换为 8 位整数。
int colorChannelToByte(float value)
{
    return static_cast<int>(
        std::clamp(std::round(value * 255.0f), 0.0f, 255.0f));
}

/// @brief 将颜色转换为 #RRGGBBAA 文本。
std::string colorToHexString(glm::vec4 color)
{
    char buffer[10]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "#%02X%02X%02X%02X",
                  colorChannelToByte(color.r),
                  colorChannelToByte(color.g),
                  colorChannelToByte(color.b),
                  colorChannelToByte(color.a));
    return std::string(buffer);
}

/// @brief 判断字符是否为空白。
bool isHexInputSpace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

/// @brief 获取单个十六进制字符值。
int hexDigitValue(char ch)
{
    if ( ch >= '0' && ch <= '9' ) return ch - '0';
    if ( ch >= 'a' && ch <= 'f' ) return ch - 'a' + 10;
    if ( ch >= 'A' && ch <= 'F' ) return ch - 'A' + 10;
    return -1;
}

/// @brief 解析两个十六进制字符为 8 位通道值。
bool parseHexByte(std::string_view text, std::size_t offset, int& value)
{
    int hi = hexDigitValue(text[offset]);
    int lo = hexDigitValue(text[offset + 1]);
    if ( hi < 0 || lo < 0 ) return false;
    value = hi * 16 + lo;
    return true;
}

/// @brief 解析 #RRGGBB 或 #RRGGBBAA 颜色文本。
bool parseHexColor(std::string_view text, glm::vec4& color)
{
    std::size_t begin = 0;
    std::size_t end   = text.size();
    while ( begin < end && isHexInputSpace(text[begin]) ) {
        ++begin;
    }
    while ( end > begin && isHexInputSpace(text[end - 1]) ) {
        --end;
    }
    text = text.substr(begin, end - begin);
    if ( !text.empty() && text.front() == '#' ) {
        text.remove_prefix(1);
    }
    if ( text.size() != 6 && text.size() != 8 ) return false;

    int r = 0;
    int g = 0;
    int b = 0;
    int a = 255;
    if ( !parseHexByte(text, 0, r) || !parseHexByte(text, 2, g) ||
         !parseHexByte(text, 4, b) ) {
        return false;
    }
    if ( text.size() == 8 && !parseHexByte(text, 6, a) ) {
        return false;
    }

    color = { static_cast<float>(r) / 255.0f,
              static_cast<float>(g) / 255.0f,
              static_cast<float>(b) / 255.0f,
              static_cast<float>(a) / 255.0f };
    return true;
}

/// @brief 获取默认调色盘方案名。
std::string defaultPaletteSchemeName()
{
    return std::string(TR("ui.toolbar.note_palette.skin_default_scheme"));
}
}  // namespace

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
    Utils::pushFixedButtonStyleVars();

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
        const auto& shortcutConfig = editorCfg.settings.shortcutConfig;

        auto tooltipWithShortcut =
            [](const char*                    tooltip,
               const Config::ShortcutBinding& binding) -> std::string {
            std::string tooltipText  = tooltip ? tooltip : "";
            std::string shortcutText = ShortcutUtils::formatShortcut(binding);
            if ( !shortcutText.empty() ) {
                tooltipText += " (";
                tooltipText += shortcutText;
                tooltipText += ")";
            }
            return tooltipText;
        };

        auto applyConfigToggleShortcut =
            [&](const Config::ShortcutBinding& binding,
                auto                           applyChange) -> bool {
            if ( !ShortcutUtils::isShortcutPressed(binding) ) {
                return false;
            }
            auto newConfig = editorCfg;
            applyChange(newConfig);
            engine.setEditorConfig(newConfig);
            return true;
        };

        if ( !ImGui::GetIO().WantTextInput &&
             !ShortcutUtils::isShortcutRecordingActive() &&
             !ImGui::IsAnyItemActive() ) {
            bool handledShortcut   = false;
            auto tryToggleShortcut = [&](const Config::ShortcutBinding& binding,
                                         auto applyChange) {
                if ( handledShortcut ) {
                    return;
                }
                handledShortcut =
                    applyConfigToggleShortcut(binding, applyChange);
            };
            tryToggleShortcut(shortcutConfig.toggleReverseScroll,
                              [](Config::EditorConfig& config) {
                                  config.settings.reverseScroll =
                                      !config.settings.reverseScroll;
                              });
            tryToggleShortcut(shortcutConfig.toggleScrollSnap,
                              [](Config::EditorConfig& config) {
                                  config.settings.scrollSnap =
                                      !config.settings.scrollSnap;
                              });
            tryToggleShortcut(shortcutConfig.toggleSnapFloor,
                              [](Config::EditorConfig& config) {
                                  config.settings.snapFloor =
                                      !config.settings.snapFloor;
                              });
            tryToggleShortcut(shortcutConfig.toggleScrollTimingMapping,
                              [](Config::EditorConfig& config) {
                                  config.visual.enableLinearScrollMapping =
                                      !config.visual.enableLinearScrollMapping;
                              });
            tryToggleShortcut(shortcutConfig.toggleBeatLines,
                              [](Config::EditorConfig& config) {
                                  config.visual.drawBeatLines =
                                      !config.visual.drawBeatLines;
                              });
            tryToggleShortcut(shortcutConfig.toggleStopPlaybackOnScroll,
                              [](Config::EditorConfig& config) {
                                  config.settings.stopPlaybackOnScroll =
                                      !config.settings.stopPlaybackOnScroll;
                              });
            tryToggleShortcut(shortcutConfig.toggleHitSfx,
                              [](Config::EditorConfig& config) {
                                  config.settings.sfxConfig.enableHitSfx =
                                      !config.settings.sfxConfig.enableHitSfx;
                              });
            tryToggleShortcut(shortcutConfig.toggleHitEffects,
                              [](Config::EditorConfig& config) {
                                  config.visual.enableHitEffects =
                                      !config.visual.enableHitEffects;
                              });
            if ( !handledShortcut &&
                 ShortcutUtils::isShortcutPressed(
                     shortcutConfig.toggleSyncSameMainAudio) ) {
                engine.setSyncSameMainAudioCanvases(
                    !engine.isSyncSameMainAudioCanvasesEnabled());
            }
        }

        auto advanceItem = [&]() {
            if ( itemSpacing > 0.0f ) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + itemSpacing);
            }
        };

        auto drawToggleButton = [&](const char*                    icon,
                                    bool                           active,
                                    const char*                    tooltip,
                                    const Config::ShortcutBinding& binding,
                                    auto applyChange) {
            pushBtnStyle(active);
            ImGui::PushID(tooltip);
            if ( ImGui::Button(icon, ImVec2(btnSize, btnSize)) ) {
                auto newConfig = editorCfg;
                applyChange(newConfig);
                engine.setEditorConfig(newConfig);
            }
            ImGui::PopID();
            std::string tooltipText = tooltipWithShortcut(tooltip, binding);
            drawTooltip(tooltipText.c_str());
            ImGui::PopStyleColor(3);
            advanceItem();
        };

        auto drawRuntimeToggleButton =
            [&](const char*                    icon,
                bool                           active,
                const char*                    tooltip,
                const Config::ShortcutBinding& binding,
                auto                           applyChange) {
                pushBtnStyle(active);
                ImGui::PushID(tooltip);
                if ( ImGui::Button(icon, ImVec2(btnSize, btnSize)) ) {
                    applyChange(!active);
                }
                ImGui::PopID();
                std::string tooltipText = tooltipWithShortcut(tooltip, binding);
                drawTooltip(tooltipText.c_str());
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
        drawToolButton(ICON_MMM_PAINT_BRUSH,
                       Logic::EditTool::ColorBrush,
                       TR("ui.toolbar.color_brush"),
                       btnSize);
        advanceItem();
        drawToolButton(ICON_MMM_ERASER,
                       Logic::EditTool::ColorEraser,
                       TR("ui.toolbar.color_eraser"),
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

        if ( !m_colorPaletteInitialized ) initializeColorPalette();

        ImGuiColorEditFlags colorButtonFlags =
            ImGuiColorEditFlags_NoTooltip |
            ImGuiColorEditFlags_AlphaPreviewHalf;
        if ( ImGui::ColorButton(
                 "##ToolbarNoteColor",
                 toImVec4(m_paletteColors[colorSlotIndex(m_activeColorSlot)]),
                 colorButtonFlags,
                 ImVec2(btnSize, btnSize)) ) {
            m_showColorPopup = !m_showColorPopup;
            if ( m_showColorPopup ) {
                m_showDivisorPopup = false;
                m_showKeyPopup     = false;
                m_showSpeedPopup   = false;
            }
        }
        m_lastColorBtnY = ImGui::GetItemRectMin().y;
        drawTooltip(TR("ui.toolbar.note_palette").data());
        advanceItem();

        drawToggleButton(ICON_MMM_ARROWS_UP_DOWN,
                         editorCfg.settings.reverseScroll,
                         TR("ui.toolbar.reverse_scroll").data(),
                         shortcutConfig.toggleReverseScroll,
                         [](Config::EditorConfig& config) {
                             config.settings.reverseScroll =
                                 !config.settings.reverseScroll;
                         });

        drawToggleButton(ICON_MMM_MAGNET,
                         editorCfg.settings.scrollSnap,
                         TR("ui.toolbar.scroll_snap").data(),
                         shortcutConfig.toggleScrollSnap,
                         [](Config::EditorConfig& config) {
                             config.settings.scrollSnap =
                                 !config.settings.scrollSnap;
                         });

        drawToggleButton(ICON_MMM_ARROW_DOWN,
                         editorCfg.settings.snapFloor,
                         TR("ui.toolbar.snap_floor").data(),
                         shortcutConfig.toggleSnapFloor,
                         [](Config::EditorConfig& config) {
                             config.settings.snapFloor =
                                 !config.settings.snapFloor;
                         });

        drawToggleButton(ICON_MMM_EYE,
                         !editorCfg.visual.enableLinearScrollMapping,
                         TR("ui.toolbar.scroll_timing_mapping").data(),
                         shortcutConfig.toggleScrollTimingMapping,
                         [](Config::EditorConfig& config) {
                             config.visual.enableLinearScrollMapping =
                                 !config.visual.enableLinearScrollMapping;
                         });

        drawToggleButton(ICON_MMM_BARS,
                         editorCfg.visual.drawBeatLines,
                         TR("ui.toolbar.draw_beat_lines").data(),
                         shortcutConfig.toggleBeatLines,
                         [](Config::EditorConfig& config) {
                             config.visual.drawBeatLines =
                                 !config.visual.drawBeatLines;
                         });

        drawToggleButton(ICON_MMM_STOP,
                         editorCfg.settings.stopPlaybackOnScroll,
                         TR("ui.toolbar.stop_on_scroll").data(),
                         shortcutConfig.toggleStopPlaybackOnScroll,
                         [](Config::EditorConfig& config) {
                             config.settings.stopPlaybackOnScroll =
                                 !config.settings.stopPlaybackOnScroll;
                         });

        drawToggleButton(ICON_MMM_HIT_SFX,
                         editorCfg.settings.sfxConfig.enableHitSfx,
                         TR("ui.toolbar.hit_sfx").data(),
                         shortcutConfig.toggleHitSfx,
                         [](Config::EditorConfig& config) {
                             config.settings.sfxConfig.enableHitSfx =
                                 !config.settings.sfxConfig.enableHitSfx;
                         });

        drawToggleButton(ICON_MMM_VISUAL_EFFECTS,
                         editorCfg.visual.enableHitEffects,
                         TR("ui.toolbar.hit_effects").data(),
                         shortcutConfig.toggleHitEffects,
                         [](Config::EditorConfig& config) {
                             config.visual.enableHitEffects =
                                 !config.visual.enableHitEffects;
                         });

        drawRuntimeToggleButton(
            ICON_MMM_LINK,
            engine.isSyncSameMainAudioCanvasesEnabled(),
            TR("ui.toolbar.sync_same_main_audio").data(),
            shortcutConfig.toggleSyncSameMainAudio,
            [&engine](bool enabled) {
                engine.setSyncSameMainAudioCanvases(enabled);
            });

        float bottomButtonsH = btnSize * 3.0f + itemSpacing * 2.0f;
        float bottomStartY = ImGui::GetCursorPosY() +
                             ImGui::GetContentRegionAvail().y - bottomButtonsH;
        if ( bottomStartY > ImGui::GetCursorPosY() ) {
            ImGui::SetCursorPosY(bottomStartY);
        }

        auto applyPlaybackSpeed = [&engine](double speed) {
            engine.pushCommand(
                Logic::CmdSetPlaybackSpeed{ std::clamp(speed, 0.25, 2.0) });
        };

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

            {
                pushBtnStyle(m_showSpeedPopup);
                ImFont* contentFont = skinCfg.getFont("content");
                if ( contentFont ) ImGui::PushFont(contentFont);

                const double currentSpeed =
                    Audio::AudioManager::instance().getPlaybackSpeed();
                char speedBuf[64];
                if ( hasBeatmap ) {
                    snprintf(speedBuf,
                             sizeof(speedBuf),
                             "%.2g##ToolbarPlaybackSpeed",
                             currentSpeed);
                } else {
                    snprintf(
                        speedBuf, sizeof(speedBuf), "--##ToolbarPlaybackSpeed");
                }

                if ( ImGui::Button(speedBuf, ImVec2(btnSize, btnSize)) ) {
                    m_showSpeedPopup = !m_showSpeedPopup;
                    if ( m_showSpeedPopup ) {
                        m_showKeyPopup     = false;
                        m_showDivisorPopup = false;
                    }
                }
                m_lastSpeedBtnY = ImGui::GetItemRectMin().y;

                if ( hasBeatmap && ImGui::IsItemHovered() ) {
                    float wheel = ImGui::GetIO().MouseWheel;
                    if ( std::abs(wheel) > 0.1f ) {
                        constexpr std::array<double, 4> presets = {
                            0.25, 0.5, 0.75, 1.0
                        };
                        size_t bestIdx = 0;
                        double minDiff = std::abs(currentSpeed - presets[0]);
                        for ( size_t i = 1; i < presets.size(); ++i ) {
                            double diff = std::abs(currentSpeed - presets[i]);
                            if ( diff < minDiff ) {
                                minDiff = diff;
                                bestIdx = i;
                            }
                        }

                        if ( wheel > 0.0f && bestIdx + 1 < presets.size() ) {
                            ++bestIdx;
                        } else if ( wheel < 0.0f && bestIdx > 0 ) {
                            --bestIdx;
                        }

                        double newSpeed = presets[bestIdx];
                        if ( std::abs(newSpeed - currentSpeed) > 0.0001 ) {
                            applyPlaybackSpeed(newSpeed);
                        }
                    }
                    drawTooltip(TR("ui.toolbar.playback_speed").data());
                }

                if ( contentFont ) ImGui::PopFont();
                ImGui::PopStyleColor(3);
            }
            advanceItem();

            pushBtnStyle(m_showKeyPopup);
            ImFont* contentFont = skinCfg.getFont("content");
            if ( contentFont ) ImGui::PushFont(contentFont);

            char keyBuf[64];
            if ( hasBeatmap ) {
                snprintf(keyBuf,
                         sizeof(keyBuf),
                         "%dK##ToolbarKeyCount",
                         currentTracks);
            } else {
                snprintf(keyBuf, sizeof(keyBuf), "--##ToolbarKeyCount");
            }

            if ( ImGui::Button(keyBuf, ImVec2(btnSize, btnSize)) ) {
                m_showKeyPopup = !m_showKeyPopup;
                if ( m_showKeyPopup ) {
                    m_showDivisorPopup = false;
                    m_showSpeedPopup   = false;
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
            char divisorBuf[64];
            snprintf(divisorBuf,
                     sizeof(divisorBuf),
                     "%d##ToolbarBeatDivisor",
                     currentDivisor);
            if ( ImGui::Button(divisorBuf, ImVec2(btnSize, btnSize)) ) {
                m_showDivisorPopup = !m_showDivisorPopup;
                if ( m_showDivisorPopup ) {
                    m_showKeyPopup   = false;
                    m_showSpeedPopup = false;
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
    ImGui::PopStyleVar(3);  // WindowPadding, WindowBorderSize, WindowRounding
    Utils::popFixedButtonStyleVars();
    ImGui::PopStyleVar(4);

    renderColorPalettePopup(dpiScale);

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
                char buf[64];
                snprintf(buf,
                         sizeof(buf),
                         "1/%d##ToolbarDivisorPreset%zu",
                         commonDivisors[i],
                         i);
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

    // --- 绘制主音轨倍速设置悬浮窗 ---
    if ( m_showSpeedPopup ) {
        ImVec2 toolbarPos = ImGui::FindWindowByName(" ###Toolbar")->Pos;

        ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        float          viewportTop  = mainViewport->Pos.y;
        float viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
        float viewportLeft   = mainViewport->Pos.x;

        float targetX = toolbarPos.x - std::floor(4.0f * dpiScale);
        float targetY = m_lastSpeedBtnY;

        float popupW = m_speedPopupWidth > 0.0f ? m_speedPopupWidth
                                                : std::floor(160.0f * dpiScale);
        float popupH = m_speedPopupHeight > 0.0f
                           ? m_speedPopupHeight
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

        if ( ImGui::Begin("##PlaybackSpeedPopup", nullptr, popupFlags) ) {
            auto& engine = Logic::EditorEngine::instance();
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();

            if ( session && session->getContext().currentBeatmap ) {
                auto applyPopupSpeed = [&engine](double speed) {
                    engine.pushCommand(Logic::CmdSetPlaybackSpeed{
                        std::clamp(speed, 0.25, 2.0) });
                };

                float currentSpeed = static_cast<float>(std::clamp(
                    Audio::AudioManager::instance().getPlaybackSpeed(),
                    0.25,
                    2.0));

                ImGui::TextUnformatted(TR("ui.toolbar.playback_speed").data());
                ImGui::Separator();

                ImGui::SetNextItemWidth(std::floor(140.0f * dpiScale));
                if ( ImGui::SliderFloat("##PlaybackSpeedSlider",
                                        &currentSpeed,
                                        0.25f,
                                        2.0f,
                                        "%.2fx",
                                        ImGuiSliderFlags_AlwaysClamp) ) {
                    applyPopupSpeed(static_cast<double>(currentSpeed));
                }

                constexpr std::array<double, 4> presets = {
                    0.25, 0.5, 0.75, 1.0
                };
                const float presetButtonH = std::floor(26.0f * dpiScale);
                const float presetButtonW =
                    std::max(std::floor(64.0f * dpiScale),
                             ImGui::CalcTextSize("0.75x").x +
                                 ImGui::GetStyle().FramePadding.x * 2.0f);
                for ( size_t i = 0; i < presets.size(); ++i ) {
                    if ( i > 0 && i % 2 != 0 ) ImGui::SameLine();
                    char buf[64];
                    snprintf(buf,
                             sizeof(buf),
                             "%.2gx##ToolbarSpeedPreset%zu",
                             presets[i],
                             i);
                    if ( ImGui::Button(buf,
                                       ImVec2(presetButtonW, presetButtonH)) ) {
                        applyPopupSpeed(presets[i]);
                    }
                }
            } else {
                m_showSpeedPopup = false;
            }

            ImVec2 sz          = ImGui::GetWindowSize();
            m_speedPopupWidth  = sz.x;
            m_speedPopupHeight = sz.y;
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
                    char buf[64];
                    snprintf(buf,
                             sizeof(buf),
                             "%dK##ToolbarKeyPreset%zu",
                             commonKeys[i],
                             i);
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

void ToolbarView::initializeColorPalette()
{
    for ( std::size_t i = 0; i < Logic::NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot          = static_cast<Logic::NoteColorSlot>(i);
        m_paletteColors[i] = toVec4(skinColorForSlot(slot));
    }

    auto& paletteConfig =
        Config::AppConfig::instance().getEditorSettings().noteColorPalettes;
    if ( !paletteConfig.schemes.empty() ) {
        std::size_t index = std::min(paletteConfig.activeSchemeIndex,
                                     paletteConfig.schemes.size() - 1);
        loadPaletteScheme(index);
    } else {
        m_activePaletteSchemeIndex = -1;
        setPaletteSchemeNameBuffer(defaultPaletteSchemeName());
        pushPaletteToBrush();
    }
    m_colorPaletteInitialized = true;
}

void ToolbarView::pushPaletteToBrush()
{
    Logic::EditorEngine::instance().pushCommand(
        Logic::CmdSetBrushNotePalette{ m_paletteColors });
}

void ToolbarView::pushPaletteToSelection()
{
    Logic::EditorEngine::instance().pushCommand(
        Logic::CmdApplyNotePaletteToSelection{ m_paletteColors });
}

void ToolbarView::loadPaletteScheme(std::size_t schemeIndex)
{
    auto& app           = Config::AppConfig::instance();
    auto& paletteConfig = app.getEditorSettings().noteColorPalettes;
    if ( schemeIndex >= paletteConfig.schemes.size() ) return;

    for ( std::size_t i = 0; i < Logic::NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot          = static_cast<Logic::NoteColorSlot>(i);
        m_paletteColors[i] = toVec4(skinColorForSlot(slot));
    }

    const auto& scheme = paletteConfig.schemes[schemeIndex];
    std::size_t count  = std::min(scheme.colors.size(), m_paletteColors.size());
    for ( std::size_t i = 0; i < count; ++i ) {
        m_paletteColors[i] = fromStoredColor(scheme.colors[i]);
    }

    m_activePaletteSchemeIndex      = static_cast<int>(schemeIndex);
    paletteConfig.activeSchemeIndex = schemeIndex;
    setPaletteSchemeNameBuffer(scheme.name);
    pushPaletteToBrush();
}

void ToolbarView::savePaletteScheme(bool createNew)
{
    auto& app           = Config::AppConfig::instance();
    auto& paletteConfig = app.getEditorSettings().noteColorPalettes;

    Config::NoteColorPaletteScheme scheme;
    scheme.name = currentPaletteSchemeName();
    scheme.colors.reserve(Logic::NOTE_COLOR_SLOT_COUNT);
    for ( const auto& color : m_paletteColors ) {
        scheme.colors.push_back(toStoredColor(color));
    }

    bool hasActiveScheme =
        m_activePaletteSchemeIndex >= 0 &&
        static_cast<std::size_t>(m_activePaletteSchemeIndex) <
            paletteConfig.schemes.size();
    if ( createNew || !hasActiveScheme ) {
        paletteConfig.schemes.push_back(std::move(scheme));
        m_activePaletteSchemeIndex =
            static_cast<int>(paletteConfig.schemes.size() - 1);
    } else {
        paletteConfig
            .schemes[static_cast<std::size_t>(m_activePaletteSchemeIndex)] =
            std::move(scheme);
    }

    paletteConfig.activeSchemeIndex =
        static_cast<std::size_t>(m_activePaletteSchemeIndex);
    app.save();
}

void ToolbarView::renamePaletteScheme()
{
    auto& app           = Config::AppConfig::instance();
    auto& paletteConfig = app.getEditorSettings().noteColorPalettes;
    if ( m_activePaletteSchemeIndex < 0 ) return;

    std::size_t index = static_cast<std::size_t>(m_activePaletteSchemeIndex);
    if ( index >= paletteConfig.schemes.size() ) return;

    paletteConfig.schemes[index].name = currentPaletteSchemeName();
    paletteConfig.activeSchemeIndex   = index;
    app.save();
}

void ToolbarView::setPaletteSchemeNameBuffer(const std::string& name)
{
    m_paletteSchemeNameBuffer.fill('\0');
    std::size_t count =
        std::min(name.size(), m_paletteSchemeNameBuffer.size() - 1);
    std::copy_n(name.begin(), count, m_paletteSchemeNameBuffer.begin());
}

void ToolbarView::setColorHexBuffer(Logic::NoteColorSlot slot, glm::vec4 color)
{
    m_colorHexBuffer.fill('\0');
    std::string text  = colorToHexString(color);
    std::size_t count = std::min(text.size(), m_colorHexBuffer.size() - 1);
    std::copy_n(text.begin(), count, m_colorHexBuffer.begin());
    m_colorHexBufferSlot = slot;
}

std::string ToolbarView::currentPaletteSchemeName() const
{
    std::string name(m_paletteSchemeNameBuffer.data());
    if ( name.empty() ) return defaultPaletteSchemeName();
    return name;
}

void ToolbarView::pushColorCommands(Logic::NoteColorSlot     slot,
                                    std::optional<glm::vec4> color,
                                    bool                     applyToSelection)
{
    auto& engine = Logic::EditorEngine::instance();
    engine.pushCommand(Logic::CmdSetBrushNoteColor{ slot, color });
    if ( applyToSelection ) {
        engine.pushCommand(Logic::CmdApplyNoteColorToSelection{ slot, color });
    }
}

void ToolbarView::renderColorPalettePopup(float dpiScale)
{
    if ( !m_showColorPopup ) return;

    ImGuiWindow* toolbarWindow = ImGui::FindWindowByName(" ###Toolbar");
    if ( !toolbarWindow ) return;

    ImVec2 toolbarPos = toolbarWindow->Pos;

    ImGuiViewport* mainViewport   = ImGui::GetMainViewport();
    float          viewportTop    = mainViewport->Pos.y;
    float          viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
    float          viewportLeft   = mainViewport->Pos.x;

    float targetX = toolbarPos.x - std::floor(4.0f * dpiScale);
    float targetY = m_lastColorBtnY;

    float popupW  = m_colorPopupWidth > 0.0f ? m_colorPopupWidth
                                             : std::floor(360.0f * dpiScale);
    float popupH  = m_colorPopupHeight > 0.0f ? m_colorPopupHeight
                                              : std::floor(360.0f * dpiScale);
    float padding = std::floor(8.0f * dpiScale);

    targetX = std::max(targetX, viewportLeft + popupW + padding);
    targetY = std::min(targetY, viewportBottom - popupH - padding);
    targetY = std::max(targetY, viewportTop + padding);

    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(
        ImVec2(targetX, targetY), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

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

    if ( ImGui::Begin("##NoteColorPalettePopup", nullptr, popupFlags) ) {
        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.title").data());
        ImGui::Separator();

        auto& paletteConfig =
            Config::AppConfig::instance().getEditorSettings().noteColorPalettes;
        std::string previewName = defaultPaletteSchemeName();
        if ( m_activePaletteSchemeIndex >= 0 ) {
            std::size_t activeIndex =
                static_cast<std::size_t>(m_activePaletteSchemeIndex);
            if ( activeIndex < paletteConfig.schemes.size() ) {
                previewName = paletteConfig.schemes[activeIndex].name;
            }
        }

        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.scheme").data());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::floor(210.0f * dpiScale));
        ImGui::BeginDisabled(paletteConfig.schemes.empty());
        if ( ImGui::BeginCombo("##NoteColorPaletteScheme",
                               previewName.c_str()) ) {
            for ( std::size_t i = 0; i < paletteConfig.schemes.size(); ++i ) {
                bool selected =
                    m_activePaletteSchemeIndex == static_cast<int>(i);
                if ( ImGui::Selectable(paletteConfig.schemes[i].name.c_str(),
                                       selected) ) {
                    loadPaletteScheme(i);
                    Config::AppConfig::instance().save();
                }
                if ( selected ) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        ImGui::TextUnformatted(
            TR("ui.toolbar.note_palette.scheme_name").data());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::floor(210.0f * dpiScale));
        ImGui::InputText("##NoteColorPaletteSchemeName",
                         m_paletteSchemeNameBuffer.data(),
                         m_paletteSchemeNameBuffer.size());

        const float schemeButtonH = std::floor(24.0f * dpiScale);
        if ( ImGui::Button(
                 TR("ui.toolbar.note_palette.save_scheme").data(),
                 ImVec2(std::floor(84.0f * dpiScale), schemeButtonH)) ) {
            savePaletteScheme(false);
        }
        ImGui::SameLine();
        if ( ImGui::Button(
                 TR("ui.toolbar.note_palette.new_scheme").data(),
                 ImVec2(std::floor(84.0f * dpiScale), schemeButtonH)) ) {
            savePaletteScheme(true);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_activePaletteSchemeIndex < 0);
        if ( ImGui::Button(
                 TR("ui.toolbar.note_palette.rename_scheme").data(),
                 ImVec2(std::floor(84.0f * dpiScale), schemeButtonH)) ) {
            renamePaletteScheme();
        }
        ImGui::EndDisabled();

        ImGui::Separator();

        const float swatchSize = std::floor(24.0f * dpiScale);
        for ( std::size_t i = 0; i < Logic::NOTE_COLOR_SLOT_COUNT; ++i ) {
            auto slot   = static_cast<Logic::NoteColorSlot>(i);
            bool active = slot == m_activeColorSlot;
            ImGui::PushID(static_cast<int>(i));
            if ( ImGui::ColorButton("##SlotColor",
                                    toImVec4(m_paletteColors[i]),
                                    ImGuiColorEditFlags_NoTooltip |
                                        ImGuiColorEditFlags_NoPicker |
                                        ImGuiColorEditFlags_AlphaPreviewHalf,
                                    ImVec2(swatchSize, swatchSize)) ) {
                m_activeColorSlot = slot;
            }
            ImGui::SameLine();
            if ( ImGui::Selectable(
                     TR(colorSlotLabelKey(slot)).data(),
                     active,
                     0,
                     ImVec2(std::floor(150.0f * dpiScale), swatchSize)) ) {
                m_activeColorSlot = slot;
            }
            ImGui::PopID();
        }

        ImGui::Separator();

        glm::vec4& activeColor =
            m_paletteColors[colorSlotIndex(m_activeColorSlot)];
        if ( m_colorHexBufferSlot != m_activeColorSlot ||
             !m_colorHexInputActive ) {
            setColorHexBuffer(m_activeColorSlot, activeColor);
        }

        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.color_mode").data());
        ImGui::SameLine();
        if ( ImGui::RadioButton("RGB##NotePaletteMode",
                                !m_colorPickerUseHsv) ) {
            m_colorPickerUseHsv = false;
        }
        ImGui::SameLine();
        if ( ImGui::RadioButton("HSV##NotePaletteMode", m_colorPickerUseHsv) ) {
            m_colorPickerUseHsv = true;
        }

        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.hex").data());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::floor(148.0f * dpiScale));
        bool hexChanged = ImGui::InputText("##NoteColorHex",
                                           m_colorHexBuffer.data(),
                                           m_colorHexBuffer.size(),
                                           ImGuiInputTextFlags_CharsNoBlank);
        m_colorHexInputActive = ImGui::IsItemActive();
        if ( hexChanged ) {
            glm::vec4 parsedColor;
            if ( parseHexColor(m_colorHexBuffer.data(), parsedColor) ) {
                activeColor = parsedColor;
                pushPaletteToBrush();
            }
        }
        if ( ImGui::IsItemDeactivatedAfterEdit() ) {
            glm::vec4 parsedColor;
            if ( parseHexColor(m_colorHexBuffer.data(), parsedColor) ) {
                activeColor = parsedColor;
                setColorHexBuffer(m_activeColorSlot, activeColor);
                pushPaletteToSelection();
            } else {
                setColorHexBuffer(m_activeColorSlot, activeColor);
            }
            m_colorHexInputActive = false;
        }

        ImGuiColorEditFlags pickerFlags =
            ImGuiColorEditFlags_AlphaBar |
            ImGuiColorEditFlags_AlphaPreviewHalf |
            (m_colorPickerUseHsv ? ImGuiColorEditFlags_DisplayHSV
                                 : ImGuiColorEditFlags_DisplayRGB);

        if ( ImGui::ColorPicker4(
                 "##NoteColorPicker", &activeColor.r, pickerFlags) ) {
            pushPaletteToBrush();
        }
        if ( ImGui::IsItemDeactivatedAfterEdit() ) {
            pushPaletteToSelection();
        }

        ImGui::Separator();
        ImGui::TextUnformatted(
            TR("ui.toolbar.note_palette.skin_defaults").data());

        for ( std::size_t i = 0; i < Logic::NOTE_COLOR_SLOT_COUNT; ++i ) {
            if ( i > 0 ) ImGui::SameLine();
            auto slot         = static_cast<Logic::NoteColorSlot>(i);
            auto defaultColor = toVec4(skinColorForSlot(slot));
            ImGui::PushID(static_cast<int>(i + 100));
            if ( ImGui::ColorButton("##SkinDefaultColor",
                                    toImVec4(defaultColor),
                                    ImGuiColorEditFlags_NoTooltip |
                                        ImGuiColorEditFlags_NoPicker |
                                        ImGuiColorEditFlags_AlphaPreviewHalf,
                                    ImVec2(swatchSize, swatchSize)) ) {
                m_activeColorSlot  = slot;
                m_paletteColors[i] = defaultColor;
                pushPaletteToBrush();
                pushPaletteToSelection();
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(TR(colorSlotLabelKey(slot)).data());
            }
            ImGui::PopID();
        }

        const float buttonH = std::floor(26.0f * dpiScale);
        if ( ImGui::Button(TR("ui.toolbar.note_palette.apply_selected").data(),
                           ImVec2(std::floor(140.0f * dpiScale), buttonH)) ) {
            pushPaletteToSelection();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.toolbar.note_palette.clear_custom").data(),
                           ImVec2(std::floor(140.0f * dpiScale), buttonH)) ) {
            auto slot   = m_activeColorSlot;
            activeColor = toVec4(skinColorForSlot(slot));
            pushColorCommands(slot, std::nullopt, true);
        }

        ImVec2 sz          = ImGui::GetWindowSize();
        m_colorPopupWidth  = sz.x;
        m_colorPopupHeight = sz.y;
    }
    ImGui::End();

    ImGui::PopStyleVar(4);
}

void ToolbarView::drawToolButton(const char* icon, Logic::EditTool tool,
                                 const char* tooltip, float width)
{
    bool isActive = (m_currentTool == tool);

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
            if ( tool == Logic::EditTool::ColorBrush ) {
                pushPaletteToBrush();
            }
            Logic::EditorEngine::instance().pushCommand(
                Logic::CmdChangeTool{ tool });
        }
    }

    std::string tooltipText = tooltip ? tooltip : "";
    const auto& settings    = Config::AppConfig::instance().getEditorSettings();
    std::string shortcutText = ShortcutUtils::formatShortcut(
        ShortcutUtils::getToolShortcut(settings, tool));
    if ( !shortcutText.empty() ) {
        tooltipText += " (";
        tooltipText += shortcutText;
        tooltipText += ")";
    }
    drawTooltip(tooltipText.c_str());

    ImGui::PopStyleColor(3);
}

void ToolbarView::drawTooltip(const char* text)
{
    Utils::renderTooltip(text, Utils::TooltipDir::Left);
}

}  // namespace MMM::UI
