#include "ui/imgui/manager/ToolbarView.h"
#include "audio/AudioManager.h"
#include "canvas/TimelineCanvas.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/ColorPaletteFile.h"
#include "config/EditorConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <iterator>
#include <limits>
#include <nfd.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace MMM::UI
{

namespace
{
/// @brief 绘制文本空间不足时自动滚动的工具栏方形按钮。
/// @param id 不显示的 ImGui ID。
/// @param text 按钮显示文本。
/// @param size 按钮尺寸。
/// @return 本帧按钮被激活时返回 true。
/// @warning UI 热路径：只复用反馈按钮并追加一次局部裁剪文本绘制。
bool drawToolbarScrollingButton(const char* id, std::string_view text,
                                ImVec2 size)
{
    const bool   clicked = ::MMM::UI::FeedbackButton(id, size);
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    const float  padding = std::max(2.0f, ImGui::GetStyle().FramePadding.x);
    const float  availableWidth =
        std::max(0.0f, itemMax.x - itemMin.x - padding * 2.0f);
    Utils::drawScrollingText(text,
                             ImVec2(itemMin.x + padding, itemMin.y),
                             availableWidth,
                             itemMax.y - itemMin.y,
                             true);
    return clicked;
}

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

/// @brief 获取分拍线颜色槽位对应的翻译键。
const char* beatLineColorSlotLabelKey(std::size_t slotIndex)
{
    switch ( slotIndex ) {
    case 0: return "ui.toolbar.note_palette.beat_line_1";
    case 1: return "ui.toolbar.note_palette.beat_line_2";
    case 2: return "ui.toolbar.note_palette.beat_line_3";
    case 3: return "ui.toolbar.note_palette.beat_line_4";
    case 4: return "ui.toolbar.note_palette.beat_line_6";
    case 5: return "ui.toolbar.note_palette.beat_line_8";
    case 6: return "ui.toolbar.note_palette.beat_line_12";
    case 7: return "ui.toolbar.note_palette.beat_line_16";
    default: return "ui.toolbar.note_palette.beat_line_default";
    }
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

/// @brief 判断皮肤颜色查询结果是否为缺失键占位色。
bool isMissingSkinColor(const Config::Color& color)
{
    return color.r == 1.0f && color.g == 0.0f && color.b == 1.0f &&
           color.a == 1.0f;
}

/// @brief 获取分拍线槽位对应的皮肤颜色。
Config::Color skinBeatLineColor(std::size_t slotIndex)
{
    auto& skin = Config::SkinManager::instance();
    if ( slotIndex >= Config::BEAT_LINE_COLOR_PALETTE_DENOMINATORS.size() ) {
        return skin.getColor("beat_lines.default");
    }

    const int denominator =
        Config::BEAT_LINE_COLOR_PALETTE_DENOMINATORS[slotIndex];
    Config::Color color =
        skin.getColor("beat_lines.beat_" + std::to_string(denominator));
    return isMissingSkinColor(color) ? skin.getColor("beat_lines.default")
                                     : color;
}

/// @brief 用皮肤默认颜色填充调色盘缓存。
void fillPaletteWithSkinDefaults(
    std::array<glm::vec4, Logic::NOTE_COLOR_SLOT_COUNT>& colors)
{
    for ( std::size_t i = 0; i < Logic::NOTE_COLOR_SLOT_COUNT; ++i ) {
        auto slot = static_cast<Logic::NoteColorSlot>(i);
        colors[i] = toVec4(skinColorForSlot(slot));
    }
}

/// @brief 用皮肤默认颜色填充分拍线调色盘缓存。
void fillBeatLinePaletteWithSkinDefaults(
    std::array<glm::vec4, Config::BEAT_LINE_COLOR_PALETTE_SLOT_COUNT>& colors)
{
    for ( std::size_t i = 0; i < colors.size(); ++i ) {
        colors[i] = toVec4(skinBeatLineColor(i));
    }
}

/// @brief 在皮肤默认颜色基础上应用一个自定义调色盘方案。
void applyStoredPaletteScheme(
    std::array<glm::vec4, Logic::NOTE_COLOR_SLOT_COUNT>& colors,
    std::array<glm::vec4, Config::BEAT_LINE_COLOR_PALETTE_SLOT_COUNT>&
                                      beatLineColors,
    const Config::ColorPaletteScheme& scheme)
{
    for ( std::size_t i = 0; i < colors.size(); ++i ) {
        colors[i] = fromStoredColor(scheme.noteColors[i]);
    }
    for ( std::size_t i = 0; i < beatLineColors.size(); ++i ) {
        beatLineColors[i] = fromStoredColor(scheme.beatLineColors[i]);
    }
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
    return TR("ui.toolbar.note_palette.skin_default_scheme").data();
}

/// @brief 获取继承软件默认调色盘方案名。
std::string inheritedPaletteSchemeName()
{
    return TR("ui.settings.project.note_palette.inherit").data();
}

/// @brief 判断方案名是否保留给内置调色盘选项。
bool isReservedPaletteSchemeName(const std::string& name)
{
    return name.empty() ||
           name == Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID ||
           name == defaultPaletteSchemeName() ||
           name == inheritedPaletteSchemeName();
}

/// @brief 将方案名转换为可用作推荐导出文件名的文本。
/// @param name 当前调色方案名。
/// @return 已替换路径非法字符且移除结尾空格和点号的文件名主体。
std::string sanitizePaletteExportFileName(std::string name)
{
    for ( char& character : name ) {
        const auto byte = static_cast<unsigned char>(character);
        if ( byte < 32 || character == '<' || character == '>' ||
             character == ':' || character == '"' || character == '/' ||
             character == '\\' || character == '|' || character == '?' ||
             character == '*' ) {
            character = '_';
        }
    }
    while ( !name.empty() && (name.back() == ' ' || name.back() == '.') ) {
        name.pop_back();
    }
    if ( name.empty() || name == "." || name == ".." ) {
        name = "palette";
    }
    name += Config::COLOR_PALETTE_FILE_EXTENSION;
    return name;
}

/// @brief 规范化调色方案导出路径的扩展名。
/// @param path 文件选择器返回的 UTF-8 路径。
/// @return 使用 `.mmpalette` 扩展名的 UTF-8 路径。
std::string normalizePaletteExportPath(const std::string& path)
{
    if ( path.empty() ) {
        return {};
    }
    std::filesystem::path normalized = Config::utf8ToPath(path);
    normalized.replace_extension(Config::COLOR_PALETTE_FILE_EXTENSION);
    return Config::pathToUtf8(normalized);
}
}  // namespace

ToolbarView::ToolbarView(const std::string& name) : IUIView(name) {}

void ToolbarView::update(UIManager* sourceManager)
{
    Config::SkinManager& skinCfg = Config::SkinManager::instance();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();

    // 从逻辑引擎同步当前工具状态
    m_currentTool = Logic::EditorEngine::instance().getCurrentTool();
    if ( m_currentTool != Logic::EditTool::Layout ) {
        m_showLayoutPopup = false;
    }

    // 样式锁定
    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    auto& aesthetics     = editorSettings.aesthetics;
    const bool showToolLabels  = editorSettings.showToolLabels;
    const bool fixedToolWindow = editorSettings.fixedToolWindow;

    float windowPadding = std::floor(aesthetics.windowPadding * dpiScale);

    // 强制固定宽度 (增加 12px 左右各 6px 补白)
    float fixedBaseW   = 32.0f;
    float toolbarBaseW = fixedBaseW * dpiScale;
    float fixedW       = std::floor(fixedBaseW * dpiScale);
    float btnSize      = toolbarBaseW;
    float btnHeight   = showToolLabels ? std::floor(46.0f * dpiScale) : btnSize;
    float totalFixedW = fixedW + 2.0f * windowPadding;

    // 2. 锁定窗口尺寸约束
    ImGui::SetNextWindowSizeConstraints(ImVec2(totalFixedW, -1),
                                        ImVec2(totalFixedW, -1));

    // 3. 核心标志
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoResize;
    if ( fixedToolWindow ) {
        flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoDocking;
    } else if ( ImGuiID toolDockId = MainDockSpaceUI::getToolDockId();
                toolDockId != 0 ) {
        ImGui::SetNextWindowDockID(toolDockId, ImGuiCond_Always);
    }

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
            ImGui::PushFont(f, f->LegacySize);
            pushedIconFont = true;
        }

        const float itemSpacing = std::floor(aesthetics.itemSpacing * dpiScale);
        auto&       engine      = Logic::EditorEngine::instance();
        const auto& editorCfg = Config::AppConfig::instance().getEditorConfig();
        const auto& shortcutConfig = editorCfg.settings.shortcutConfig;
        m_beatLineDisplayModeHistory.observe(
            editorCfg.visual.beatLineDisplayMode);
        const bool shouldPlayAdjustmentFeedback =
            editorCfg.settings.stopPlaybackOnScroll;

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
             !ImGui::IsAnyItemActive() &&
             !ImGui::IsPopupOpen(
                 nullptr,
                 ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ) {
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
                              [this](Config::EditorConfig& config) {
                                  config.visual.beatLineDisplayMode =
                                      m_beatLineDisplayModeHistory.toggleTarget(
                                          config.visual.beatLineDisplayMode);
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
                                    const char*                    shortLabel,
                                    const Config::ShortcutBinding& binding,
                                    auto applyChange) {
            pushBtnStyle(active);
            ImGui::PushID(tooltip);
            if ( drawIconButton(icon,
                                "##ToolbarToggleButton",
                                shortLabel,
                                btnSize,
                                btnHeight,
                                showToolLabels) ) {
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
                const char*                    shortLabel,
                const Config::ShortcutBinding& binding,
                auto                           applyChange) {
                pushBtnStyle(active);
                ImGui::PushID(tooltip);
                if ( drawIconButton(icon,
                                    "##ToolbarRuntimeToggleButton",
                                    shortLabel,
                                    btnSize,
                                    btnHeight,
                                    showToolLabels) ) {
                    applyChange(!active);
                }
                ImGui::PopID();
                std::string tooltipText = tooltipWithShortcut(tooltip, binding);
                drawTooltip(tooltipText.c_str());
                ImGui::PopStyleColor(3);
                advanceItem();
            };

        const bool isLayoutEditing = m_currentTool == Logic::EditTool::Layout;
        ImGui::BeginDisabled(isLayoutEditing);
        drawToolButton(ICON_MMM_HAND,
                       Logic::EditTool::Move,
                       TR("ui.toolbar.move"),
                       btnSize,
                       btnHeight,
                       TR("ui.toolbar.short.move").data(),
                       showToolLabels,
                       sourceManager);
        advanceItem();
        drawToolButton(ICON_MMM_SQUARE_SELECT,
                       Logic::EditTool::Marquee,
                       TR("ui.toolbar.marquee"),
                       btnSize,
                       btnHeight,
                       TR("ui.toolbar.short.marquee").data(),
                       showToolLabels,
                       sourceManager);
        advanceItem();
        drawToolButton(ICON_MMM_PEN,
                       Logic::EditTool::Draw,
                       TR("ui.toolbar.draw"),
                       btnSize,
                       btnHeight,
                       TR("ui.toolbar.short.draw").data(),
                       showToolLabels,
                       sourceManager);
        advanceItem();
        drawToolButton(ICON_MMM_PAINT_BRUSH,
                       Logic::EditTool::ColorBrush,
                       TR("ui.toolbar.color_brush"),
                       btnSize,
                       btnHeight,
                       TR("ui.toolbar.short.color_brush").data(),
                       showToolLabels,
                       sourceManager);
        advanceItem();
        drawToolButton(ICON_MMM_ERASER,
                       Logic::EditTool::ColorEraser,
                       TR("ui.toolbar.color_eraser"),
                       btnSize,
                       btnHeight,
                       TR("ui.toolbar.short.color_eraser").data(),
                       showToolLabels,
                       sourceManager);
        advanceItem();
        ImGui::EndDisabled();

        ImVec2 sepPos = ImGui::GetCursorScreenPos();
        float  sepH   = 2.0f * dpiScale;
        ImGui::GetWindowDrawList()->AddLine(
            { sepPos.x + 4.0f * dpiScale, sepPos.y + sepH * 0.5f },
            { sepPos.x + btnSize - 4.0f * dpiScale, sepPos.y + sepH * 0.5f },
            IM_COL32(100, 100, 100, 150),
            1.0f * dpiScale);
        ImGui::Dummy(ImVec2(btnSize, sepH));
        advanceItem();

        drawLayoutButton(btnSize, btnHeight, showToolLabels);
        advanceItem();

        if ( !m_colorPaletteInitialized ) initializeColorPalette();
        applyProjectPalettePreference();

        pushBtnStyle(m_showColorPopup);
        if ( ::MMM::UI::FeedbackButton("##ToolbarNoteColor",
                                       ImVec2(btnSize, btnHeight)) ) {
            m_showColorPopup = !m_showColorPopup;
            if ( m_showColorPopup ) {
                m_showDivisorPopup    = false;
                m_showKeyPopup        = false;
                m_showSpeedPopup      = false;
                m_showBeatLinePopup   = false;
                m_showMagnetPopup     = false;
                m_showSoundEffectTool = false;
            }
        }
        {
            ImDrawList*  drawList   = ImGui::GetWindowDrawList();
            ImVec2       minPos     = ImGui::GetItemRectMin();
            ImVec2       maxPos     = ImGui::GetItemRectMax();
            const float  swatchSize = showToolLabels
                                          ? std::floor(btnSize * 0.62f)
                                          : std::floor(btnSize * 0.72f);
            const ImVec2 swatchMin  = {
                minPos.x + (btnSize - swatchSize) * 0.5f,
                minPos.y + (showToolLabels ? std::floor(5.0f * dpiScale)
                                           : (btnHeight - swatchSize) * 0.5f),
            };
            const ImVec2 swatchMax = { swatchMin.x + swatchSize,
                                       swatchMin.y + swatchSize };
            drawList->AddRectFilled(
                swatchMin,
                swatchMax,
                ImGui::ColorConvertFloat4ToU32(toImVec4(
                    m_paletteColors[colorSlotIndex(m_activeColorSlot)])),
                rounding);
            drawList->AddRect(swatchMin,
                              swatchMax,
                              ImGui::GetColorU32(ImGuiCol_Text),
                              rounding,
                              0,
                              std::floor(1.0f * dpiScale));
            if ( showToolLabels ) {
                if ( ImFont* labelFont = skinCfg.getFont("menu") ) {
                    const char* label =
                        TR("ui.toolbar.short.note_palette").data();
                    const float  labelFontSize = std::floor(std::min(
                        btnSize * 0.38f, ImGui::GetFontSize() * 0.72f));
                    const ImVec2 labelSize     = labelFont->CalcTextSizeA(
                        labelFontSize,
                        std::numeric_limits<float>::max(),
                        0.0f,
                        label);
                    const ImVec2 labelPos = {
                        minPos.x + (btnSize - labelSize.x) * 0.5f,
                        maxPos.y - labelSize.y - std::floor(3.0f * dpiScale),
                    };
                    drawList->AddText(labelFont,
                                      labelFontSize,
                                      labelPos,
                                      ImGui::GetColorU32(ImGuiCol_Text),
                                      label);
                }
            }
        }
        m_lastColorBtnY = ImGui::GetItemRectMin().y;
        drawTooltip(TR("ui.toolbar.note_palette").data());
        ImGui::PopStyleColor(3);
        advanceItem();

        drawToggleButton(ICON_MMM_ARROWS_UP_DOWN,
                         editorCfg.settings.reverseScroll,
                         TR("ui.toolbar.reverse_scroll").data(),
                         TR("ui.toolbar.short.reverse_scroll").data(),
                         shortcutConfig.toggleReverseScroll,
                         [](Config::EditorConfig& config) {
                             config.settings.reverseScroll =
                                 !config.settings.reverseScroll;
                         });

        pushBtnStyle(true);
        ImGui::PushID("MagnetTool");
        if ( drawIconButton(ICON_MMM_MAGNET,
                            "##ToolbarMagnetTool",
                            TR("ui.toolbar.short.magnet_tool").data(),
                            btnSize,
                            btnHeight,
                            showToolLabels) ) {
            m_showMagnetPopup = !m_showMagnetPopup;
            if ( m_showMagnetPopup ) {
                m_showColorPopup      = false;
                m_showDivisorPopup    = false;
                m_showKeyPopup        = false;
                m_showSpeedPopup      = false;
                m_showBeatLinePopup   = false;
                m_showSoundEffectTool = false;
            }
        }
        m_lastMagnetBtnY = ImGui::GetItemRectMin().y;
        ImGui::PopID();
        drawTooltip(TR("ui.toolbar.magnet_tool").data());
        ImGui::PopStyleColor(3);
        advanceItem();

        drawToggleButton(ICON_MMM_ARROW_DOWN,
                         editorCfg.settings.snapFloor,
                         TR("ui.toolbar.snap_floor").data(),
                         TR("ui.toolbar.short.snap_floor").data(),
                         shortcutConfig.toggleSnapFloor,
                         [](Config::EditorConfig& config) {
                             config.settings.snapFloor =
                                 !config.settings.snapFloor;
                         });

        drawToggleButton(ICON_MMM_EYE,
                         !editorCfg.visual.enableLinearScrollMapping,
                         TR("ui.toolbar.scroll_timing_mapping").data(),
                         TR("ui.toolbar.short.scroll_timing_mapping").data(),
                         shortcutConfig.toggleScrollTimingMapping,
                         [](Config::EditorConfig& config) {
                             config.visual.enableLinearScrollMapping =
                                 !config.visual.enableLinearScrollMapping;
                         });

        pushBtnStyle(true);
        ImGui::PushID("BeatLineDisplayMode");
        if ( drawIconButton(ICON_MMM_BARS,
                            "##ToolbarBeatLineDisplayMode",
                            TR("ui.toolbar.short.draw_beat_lines").data(),
                            btnSize,
                            btnHeight,
                            showToolLabels) ) {
            m_showBeatLinePopup = !m_showBeatLinePopup;
            if ( m_showBeatLinePopup ) {
                m_showColorPopup      = false;
                m_showDivisorPopup    = false;
                m_showKeyPopup        = false;
                m_showSpeedPopup      = false;
                m_showMagnetPopup     = false;
                m_showSoundEffectTool = false;
            }
        }
        m_lastBeatLineBtnY = ImGui::GetItemRectMin().y;
        ImGui::PopID();
        {
            const std::string tooltipText =
                tooltipWithShortcut(TR("ui.toolbar.draw_beat_lines").data(),
                                    shortcutConfig.toggleBeatLines);
            drawTooltip(tooltipText.c_str());
        }
        ImGui::PopStyleColor(3);
        advanceItem();

        drawToggleButton(ICON_MMM_STOP,
                         editorCfg.settings.stopPlaybackOnScroll,
                         TR("ui.toolbar.stop_on_scroll").data(),
                         TR("ui.toolbar.short.stop_on_scroll").data(),
                         shortcutConfig.toggleStopPlaybackOnScroll,
                         [](Config::EditorConfig& config) {
                             config.settings.stopPlaybackOnScroll =
                                 !config.settings.stopPlaybackOnScroll;
                         });

        pushBtnStyle(true);
        ImGui::PushID("SoundEffectTool");
        if ( drawIconButton(ICON_MMM_HIT_SFX,
                            "##ToolbarSoundEffectTool",
                            TR("ui.toolbar.short.key_sound_tool").data(),
                            btnSize,
                            btnHeight,
                            showToolLabels) ) {
            m_showSoundEffectTool = !m_showSoundEffectTool;
            if ( m_showSoundEffectTool ) {
                m_soundEffectGainDraftInitialized = false;
                m_showLayoutPopup                 = false;
                m_showColorPopup                  = false;
                m_showDivisorPopup                = false;
                m_showKeyPopup                    = false;
                m_showSpeedPopup                  = false;
                m_showBeatLinePopup               = false;
                m_showMagnetPopup                 = false;
            }
        }
        m_lastSoundEffectToolBtnY = ImGui::GetItemRectMin().y;
        ImGui::PopID();
        {
            std::string tooltipText = TR("ui.toolbar.key_sound_tool").data();
            const auto  shortcutText =
                ShortcutUtils::formatShortcut(shortcutConfig.toggleHitSfx);
            if ( !shortcutText.empty() ) {
                tooltipText += "\n";
                tooltipText +=
                    TR("ui.toolbar.key_sound_tool_shortcut_hint").data();
                tooltipText += " (";
                tooltipText += shortcutText;
                tooltipText += ")";
            }
            drawTooltip(tooltipText.c_str());
        }
        ImGui::PopStyleColor(3);
        advanceItem();

        drawToggleButton(ICON_MMM_VISUAL_EFFECTS,
                         editorCfg.visual.enableHitEffects,
                         TR("ui.toolbar.hit_effects").data(),
                         TR("ui.toolbar.short.hit_effects").data(),
                         shortcutConfig.toggleHitEffects,
                         [](Config::EditorConfig& config) {
                             config.visual.enableHitEffects =
                                 !config.visual.enableHitEffects;
                         });

        drawRuntimeToggleButton(
            ICON_MMM_LINK,
            engine.isSyncSameMainAudioCanvasesEnabled(),
            TR("ui.toolbar.sync_same_main_audio").data(),
            TR("ui.toolbar.short.sync_same_main_audio").data(),
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
                if ( contentFont ) {
                    ImGui::PushFont(contentFont, contentFont->LegacySize);
                }

                const double currentSpeed =
                    Audio::AudioManager::instance().getPlaybackSpeed();
                char speedText[32];
                if ( hasBeatmap ) {
                    snprintf(
                        speedText, sizeof(speedText), "%.2g", currentSpeed);
                } else {
                    snprintf(speedText, sizeof(speedText), "--");
                }

                if ( drawToolbarScrollingButton("###ToolbarPlaybackSpeed",
                                                speedText,
                                                ImVec2(btnSize, btnSize)) ) {
                    m_showSpeedPopup = !m_showSpeedPopup;
                    if ( m_showSpeedPopup ) {
                        m_showKeyPopup        = false;
                        m_showDivisorPopup    = false;
                        m_showBeatLinePopup   = false;
                        m_showMagnetPopup     = false;
                        m_showSoundEffectTool = false;
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
                            if ( shouldPlayAdjustmentFeedback ) {
                                ::MMM::UI::PlayInteractionMouseUpFeedback();
                            }
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
            if ( contentFont ) {
                ImGui::PushFont(contentFont, contentFont->LegacySize);
            }

            char keyBuf[64];
            if ( hasBeatmap ) {
                snprintf(keyBuf,
                         sizeof(keyBuf),
                         "%dK###ToolbarKeyCount",
                         currentTracks);
            } else {
                snprintf(keyBuf, sizeof(keyBuf), "--###ToolbarKeyCount");
            }

            if ( ::MMM::UI::FeedbackButton(keyBuf, ImVec2(btnSize, btnSize)) ) {
                m_showKeyPopup = !m_showKeyPopup;
                if ( m_showKeyPopup ) {
                    m_showDivisorPopup    = false;
                    m_showSpeedPopup      = false;
                    m_showBeatLinePopup   = false;
                    m_showMagnetPopup     = false;
                    m_showSoundEffectTool = false;
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
                        if ( shouldPlayAdjustmentFeedback ) {
                            ::MMM::UI::PlayInteractionMouseUpFeedback();
                        }
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
            if ( contentFont ) {
                ImGui::PushFont(contentFont, contentFont->LegacySize);
            }
            char divisorBuf[64];
            snprintf(divisorBuf,
                     sizeof(divisorBuf),
                     "%d###ToolbarBeatDivisor",
                     currentDivisor);
            if ( ::MMM::UI::FeedbackButton(divisorBuf,
                                           ImVec2(btnSize, btnSize)) ) {
                m_showDivisorPopup = !m_showDivisorPopup;
                if ( m_showDivisorPopup ) {
                    m_showKeyPopup        = false;
                    m_showSpeedPopup      = false;
                    m_showBeatLinePopup   = false;
                    m_showMagnetPopup     = false;
                    m_showSoundEffectTool = false;
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
                        if ( shouldPlayAdjustmentFeedback ) {
                            ::MMM::UI::PlayInteractionMouseUpFeedback();
                        }
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
    ImGui::PopStyleVar(3);  // 窗口内边距、窗口边框大小、窗口圆角
    Utils::popFixedButtonStyleVars();
    ImGui::PopStyleVar(4);

    renderColorPalettePopup(dpiScale);
    renderPaletteExportFileDialog(dpiScale);
    renderPaletteImportFileDialog(dpiScale);
    renderLayoutPopup(dpiScale);
    renderMagnetPopup(dpiScale);
    renderBeatLinePopup(dpiScale);
    renderSoundEffectTool(dpiScale);

    // --- 绘制分拍数量设置悬浮窗 ---
    if ( m_showDivisorPopup ) {
        // 在 Toolbar 窗口左侧显示悬浮窗

        ImVec2 toolbarPos = ImGui::FindWindowByName(" ###Toolbar")->Pos;

        ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        float          viewportTop  = mainViewport->Pos.y;
        float viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
        float viewportLeft   = mainViewport->Pos.x;

        // 横向位置 = 工具栏左边缘往左 4px
        // 纵向位置 = 按钮的顶部对齐
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

        // 枢轴点 (1.0, 0.0) 代表将弹窗的右上角对齐到 popupPos
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
            if ( ::MMM::UI::FeedbackSliderInt(
                     "##DivisorSlider", &currentDivisor, 1, 64) ) {
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
            float presetTextWidth = 0.0f;
            for ( const int divisor : commonDivisors ) {
                char previewBuf[32];
                snprintf(previewBuf, sizeof(previewBuf), "1/%d", divisor);
                presetTextWidth = std::max(presetTextWidth,
                                           ImGui::CalcTextSize(previewBuf).x);
            }
            const ImGuiStyle& popupStyle = ImGui::GetStyle();
            const float compactPaddingX = std::min(popupStyle.FramePadding.x,
                                                   std::floor(4.0f * dpiScale));
            const float presetButtonWidth =
                std::ceil(std::max(std::floor(40.0f * dpiScale),
                                   presetTextWidth + compactPaddingX * 2.0f +
                                       std::floor(2.0f * dpiScale)));
            const float presetButtonHeight = std::floor(24.0f * dpiScale);
            ImGui::PushStyleVar(
                ImGuiStyleVar_FramePadding,
                ImVec2(compactPaddingX, popupStyle.FramePadding.y));
            for ( size_t i = 0; i < commonDivisors.size(); ++i ) {
                if ( i > 0 && i % 4 != 0 ) ImGui::SameLine();
                char buf[64];
                snprintf(buf,
                         sizeof(buf),
                         "1/%d##ToolbarDivisorPreset%zu",
                         commonDivisors[i],
                         i);
                if ( ::MMM::UI::FeedbackButton(
                         buf, ImVec2(presetButtonWidth, presetButtonHeight)) ) {
                    auto newConfig                 = editorCfg;
                    newConfig.settings.beatDivisor = commonDivisors[i];
                    Logic::EditorEngine::instance().setEditorConfig(newConfig);
                }
            }
            ImGui::PopStyleVar();

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
                if ( ::MMM::UI::FeedbackSliderFloat(
                         "##PlaybackSpeedSlider",
                         &currentSpeed,
                         0.25f,
                         2.0f,
                         "%.4fx",
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
                    if ( ::MMM::UI::FeedbackButton(
                             buf, ImVec2(presetButtonW, presetButtonH)) ) {
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

        // 横向位置 = 工具栏左边缘往左 4px
        // 纵向位置 = 按钮的顶部对齐
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
                if ( ::MMM::UI::FeedbackSliderInt(
                         "##TracksSlider", &currentTracks, 1, 32) ) {
                    meta.track_count = currentTracks;
                    engine.pushCommand(Logic::CmdUpdateBeatmapMetadata{ meta });
                }

                // 常用 Key 数快速设置按钮
                const std::vector<int> commonKeys = { 4, 5, 6, 7, 8 };
                // 固定尺寸快捷按钮不继承主题内容内边距，避免 K
                // 标签被挤压或裁切。
                Utils::pushFixedButtonStyleVars();
                for ( size_t i = 0; i < commonKeys.size(); ++i ) {
                    if ( i > 0 ) ImGui::SameLine();
                    char buf[64];
                    snprintf(buf,
                             sizeof(buf),
                             "%dK##ToolbarKeyPreset%zu",
                             commonKeys[i],
                             i);
                    if ( ::MMM::UI::FeedbackButton(
                             buf,
                             ImVec2(std::floor(28.0f * dpiScale),
                                    std::floor(24.0f * dpiScale))) ) {
                        meta.track_count = commonKeys[i];
                        engine.pushCommand(
                            Logic::CmdUpdateBeatmapMetadata{ meta });
                    }
                }
                Utils::popFixedButtonStyleVars();
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

/// @brief 绘制锚定在工具栏按钮旁的音效逐轨与分类控制弹层。
/// @param dpiScale 当前 DPI 缩放。
/// @warning UI 热路径：弹层打开时每帧执行，仅绘制滚动区可见控制行。
void ToolbarView::renderSoundEffectTool(float dpiScale)
{
    if ( !m_showSoundEffectTool ) return;

    ImGuiWindow* toolbarWindow = ImGui::FindWindowByName(" ###Toolbar");
    if ( !toolbarWindow ) return;

    auto& engine = Logic::EditorEngine::instance();
    bool  hasBeatmap{ false };
    int   playerTrackCount{ 0 };
    int   bgmTrackCount{ 0 };
    {
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        const auto session = engine.getActiveSession();
        if ( session && session->getContext().currentBeatmap ) {
            const auto& metadata =
                session->getContext().currentBeatmap->m_baseMapMetadata;
            hasBeatmap       = true;
            playerTrackCount = std::max(0, metadata.track_count);
            bgmTrackCount    = std::max(0, metadata.bgm_track_count);
        }
    }

    const auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    const float windowPadding = std::floor(aesthetics.windowPadding * dpiScale);
    const float windowRounding =
        std::floor(aesthetics.windowRounding * dpiScale);
    const float frameRounding = std::floor(aesthetics.frameRounding * dpiScale);
    const float rowHeight     = ImGui::GetFrameHeightWithSpacing();
    const int   playerRows    = std::max(1, playerTrackCount);
    const int   bgmRows       = std::max(1, bgmTrackCount);
    const int   totalRows     = 7 + playerRows + bgmRows;

    ImGuiViewport* mainViewport   = ImGui::GetMainViewport();
    const float    viewportTop    = mainViewport->Pos.y;
    const float    viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
    const float    viewportLeft   = mainViewport->Pos.x;
    const float    edgePadding    = std::floor(8.0F * dpiScale);
    const float    popupWidth     = std::floor(420.0F * dpiScale);
    const float    titleHeight =
        ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    const float desiredHeight = windowPadding * 2.0F + titleHeight +
                                static_cast<float>(totalRows) * rowHeight;
    const float availableHeight =
        std::max(std::floor(180.0F * dpiScale),
                 viewportBottom - viewportTop - edgePadding * 2.0F);
    const float maximumHeight =
        std::min(availableHeight, std::floor(480.0F * dpiScale));
    const float minimumHeight =
        std::min(maximumHeight, std::floor(180.0F * dpiScale));
    const float popupHeight =
        std::clamp(desiredHeight, minimumHeight, maximumHeight);

    float targetX = toolbarWindow->Pos.x - std::floor(4.0F * dpiScale);
    float targetY = m_lastSoundEffectToolBtnY;
    targetX       = std::max(targetX, viewportLeft + popupWidth + edgePadding);
    targetY = std::clamp(targetY,
                         viewportTop + edgePadding,
                         std::max(viewportTop + edgePadding,
                                  viewportBottom - popupHeight - edgePadding));

    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(
        { targetX, targetY }, ImGuiCond_Always, { 1.0F, 0.0F });
    ImGui::SetNextWindowSize({ popupWidth, popupHeight }, ImGuiCond_Always);

    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(windowPadding, windowPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRounding);

    if ( !ImGui::Begin("##SoundEffectToolPopup", nullptr, popupFlags) ) {
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }

    ImGui::TextUnformatted(TR("ui.key_sound_tool.title").data());
    ImGui::Separator();

    auto&      audio        = Audio::AudioManager::instance();
    const auto editorConfig = engine.getEditorConfig();
    if ( !m_soundEffectGainDraftInitialized || !ImGui::IsAnyItemActive() ) {
        m_unboundHitSoundGainDraft =
            editorConfig.settings.sfxConfig.unboundHitSfxGain;
        m_boundHitSoundGainDraft =
            editorConfig.settings.sfxConfig.boundHitSfxGain;
        m_soundEffectGainDraftInitialized = true;
    }
    const float muteButtonSize  = std::floor(ImGui::GetFrameHeight());
    const float gainSliderWidth = std::floor(170.0F * dpiScale);
    const float controlSpacing  = ImGui::GetStyle().ItemSpacing.x;
    const float trackViewportHeight =
        std::max(1.0F, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild("##SoundEffectControlScroller",
                      { 0.0F, trackViewportHeight },
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);

    const auto drawMuteStateButton = [&](bool        muted,
                                         float       gain,
                                         const auto& applyChange) {
        const char* icon = ICON_MMM_VOLUME_MUTE;
        if ( !muted ) {
            if ( gain <= 0.0F )
                icon = ICON_MMM_VOLUME_OFF;
            else if ( gain < 1.0F )
                icon = ICON_MMM_VOLUME_LOW;
            else
                icon = ICON_MMM_VOLUME_HIGH;
        }

        char buttonLabel[64];
        std::snprintf(buttonLabel, sizeof(buttonLabel), "%s##MuteState", icon);
        if ( muted ) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  Utils::UIThemeUtils::getDangerColor());
        }
        Utils::pushFixedButtonStyleVars();
        const bool clicked = ::MMM::UI::FeedbackButton(
            buttonLabel, ImVec2(muteButtonSize, muteButtonSize));
        Utils::popFixedButtonStyleVars();
        if ( muted ) ImGui::PopStyleColor();

        if ( ImGui::IsItemHovered() ) {
            Utils::renderTooltip(muted ? TR("ui.audio_manager.unmute").data()
                                       : TR("ui.audio_manager.mute").data());
        }
        if ( clicked ) applyChange(!muted);
    };

    const auto drawMuteOnlyRow = [&](const char* label,
                                     const char* id,
                                     bool        muted,
                                     const auto& applyMute) {
        ImGui::PushID(id);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::SetCursorPosX(
            std::max(ImGui::GetCursorPosX(),
                     ImGui::GetWindowContentRegionMax().x - muteButtonSize));
        drawMuteStateButton(muted, 1.0F, applyMute);
        ImGui::PopID();
    };

    const auto drawMixRow = [&](const char* label,
                                const char* id,
                                bool        muted,
                                float       gain,
                                const auto& applyMute,
                                const auto& applyGain,
                                const auto& persistGain) {
        ImGui::PushID(id);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        const float controlWidth =
            muteButtonSize + controlSpacing + gainSliderWidth;
        ImGui::SetCursorPosX(
            std::max(ImGui::GetCursorPosX(),
                     ImGui::GetWindowContentRegionMax().x - controlWidth));
        drawMuteStateButton(muted, gain, applyMute);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(gainSliderWidth);
        float gainPercent = std::clamp(gain * 100.0F, 0.0F, 200.0F);
        if ( ::MMM::UI::FeedbackSliderFloat(
                 "##Gain", &gainPercent, 0.0F, 200.0F, "%.0f%%") ) {
            applyGain(gainPercent * 0.01F);
        }
        if ( ImGui::IsItemDeactivatedAfterEdit() ) {
            persistGain(gainPercent * 0.01F);
        }
        if ( ImGui::IsItemHovered() ) {
            Utils::renderTooltip(TR("ui.key_sound_tool.gain").data());
        }
        ImGui::PopID();
    };

    const int    hitSoundHeaderRow  = 0;
    const int    unboundHitSoundRow = 1;
    const int    boundHitSoundRow   = 2;
    const int    playerHeaderRow    = 3;
    const int    playerMasterRow    = 4;
    const int    playerTrackBegin   = 5;
    const int    bgmHeaderRow       = playerTrackBegin + playerRows;
    const int    bgmMasterRow       = bgmHeaderRow + 1;
    const int    bgmTrackBegin      = bgmMasterRow + 1;
    const ImVec2 contentStart       = ImGui::GetCursorPos();
    const float  scrollY            = ImGui::GetScrollY();
    const float  visibleHeight      = std::max(
        1.0F,
        ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y * 2.0F);
    const float visibleStart = std::max(0.0F, scrollY - contentStart.y);
    const float visibleEnd =
        std::max(visibleStart, scrollY + visibleHeight - contentStart.y);
    const int firstVisibleRow = std::clamp(
        static_cast<int>(std::floor(visibleStart / rowHeight)), 0, totalRows);
    const int lastVisibleRow =
        std::clamp(static_cast<int>(std::ceil(visibleEnd / rowHeight)) + 1,
                   firstVisibleRow,
                   totalRows);

    for ( int row = firstVisibleRow; row < lastVisibleRow; ++row ) {
        ImGui::SetCursorPos(
            { contentStart.x,
              contentStart.y + static_cast<float>(row) * rowHeight });

        if ( row == hitSoundHeaderRow ) {
            ImGui::SeparatorText(TR("ui.key_sound_tool.hit_sound_area").data());
            continue;
        }
        if ( row == unboundHitSoundRow ) {
            drawMixRow(
                TR("ui.key_sound_tool.unbound_hit_sound").data(),
                "UnboundHitSound",
                !editorConfig.settings.sfxConfig.enableUnboundHitSfx,
                m_unboundHitSoundGainDraft,
                [&engine](bool muted) {
                    auto config = engine.getEditorConfig();
                    config.settings.sfxConfig.enableUnboundHitSfx = !muted;
                    engine.setEditorConfig(config);
                },
                [this, &engine](float gain) {
                    m_unboundHitSoundGainDraft = gain;
                    engine.pushCommand(Logic::CmdSetKeySoundEffectGroupGain{
                        .group = Logic::KeySoundEffectGroup::Unbound,
                        .gain  = gain,
                    });
                },
                [&engine](float gain) {
                    auto config = engine.getEditorConfig();
                    config.settings.sfxConfig.unboundHitSfxGain = gain;
                    engine.setEditorConfig(config);
                });
            continue;
        }
        if ( row == boundHitSoundRow ) {
            drawMixRow(
                TR("ui.key_sound_tool.bound_hit_sound").data(),
                "BoundHitSound",
                !editorConfig.settings.sfxConfig.enableBoundHitSfx,
                m_boundHitSoundGainDraft,
                [&engine](bool muted) {
                    auto config = engine.getEditorConfig();
                    config.settings.sfxConfig.enableBoundHitSfx = !muted;
                    engine.setEditorConfig(config);
                },
                [this, &engine](float gain) {
                    m_boundHitSoundGainDraft = gain;
                    engine.pushCommand(Logic::CmdSetKeySoundEffectGroupGain{
                        .group = Logic::KeySoundEffectGroup::Bound,
                        .gain  = gain,
                    });
                },
                [&engine](float gain) {
                    auto config = engine.getEditorConfig();
                    config.settings.sfxConfig.boundHitSfxGain = gain;
                    engine.setEditorConfig(config);
                });
            continue;
        }
        if ( row == playerHeaderRow ) {
            ImGui::SeparatorText(TR("ui.key_sound_tool.player_area").data());
            continue;
        }
        if ( row == playerMasterRow ) {
            drawMuteOnlyRow(TR("ui.key_sound_tool.area_master").data(),
                            "PlayerArea",
                            !editorConfig.settings.sfxConfig.enableHitSfx,
                            [&engine](bool muted) {
                                auto config = engine.getEditorConfig();
                                config.settings.sfxConfig.enableHitSfx = !muted;
                                engine.setEditorConfig(config);
                            });
            continue;
        }
        if ( row < bgmHeaderRow ) {
            const int track = row - playerTrackBegin;
            if ( track >= playerTrackCount ) {
                ImGui::TextDisabled(
                    "%s",
                    TR(hasBeatmap ? "ui.key_sound_tool.no_player_tracks"
                                  : "ui.key_sound_tool.no_beatmap")
                        .data());
                continue;
            }

            ImGui::PushID("PlayerTrack");
            ImGui::PushID(track);
            char label[64];
            std::snprintf(label,
                          sizeof(label),
                          TR("ui.key_sound_tool.player_track").data(),
                          track + 1);
            const auto trackIndex = static_cast<std::uint32_t>(track);
            drawMixRow(
                label,
                "MixState",
                audio.isPlayerKeySoundTrackMuted(trackIndex),
                audio.getPlayerKeySoundTrackGain(trackIndex),
                [&engine, trackIndex](bool muted) {
                    engine.pushCommand(Logic::CmdSetKeySoundTrackMute{
                        .area       = Logic::KeySoundTrackArea::Player,
                        .trackIndex = trackIndex,
                        .muted      = muted,
                    });
                },
                [&engine, trackIndex](float gain) {
                    engine.pushCommand(Logic::CmdSetKeySoundTrackGain{
                        .area       = Logic::KeySoundTrackArea::Player,
                        .trackIndex = trackIndex,
                        .gain       = gain,
                    });
                },
                [](float) {});
            ImGui::PopID();
            ImGui::PopID();
            continue;
        }
        if ( row == bgmHeaderRow ) {
            ImGui::SeparatorText(TR("ui.key_sound_tool.bgm_area").data());
            continue;
        }
        if ( row == bgmMasterRow ) {
            if ( !hasBeatmap || bgmTrackCount == 0 ) ImGui::BeginDisabled();
            drawMuteOnlyRow(
                TR("ui.key_sound_tool.area_master").data(),
                "BgmArea",
                audio.isBgmKeySoundAreaMuted(),
                [&engine](bool muted) {
                    engine.pushCommand(
                        Logic::CmdSetBgmKeySoundAreaMute{ .muted = muted });
                });
            if ( !hasBeatmap || bgmTrackCount == 0 ) ImGui::EndDisabled();
            continue;
        }
        if ( row >= bgmTrackBegin ) {
            const int track = row - bgmTrackBegin;
            if ( track >= bgmTrackCount ) {
                ImGui::TextDisabled(
                    "%s",
                    TR(hasBeatmap ? "ui.key_sound_tool.no_bgm_tracks"
                                  : "ui.key_sound_tool.no_beatmap")
                        .data());
                continue;
            }

            ImGui::PushID("BgmTrack");
            ImGui::PushID(track);
            char label[64];
            std::snprintf(label,
                          sizeof(label),
                          TR("ui.key_sound_tool.bgm_track").data(),
                          track + 1);
            const auto trackIndex = static_cast<std::uint32_t>(track);
            drawMixRow(
                label,
                "MixState",
                audio.isBgmKeySoundTrackMuted(trackIndex),
                audio.getBgmKeySoundTrackGain(trackIndex),
                [&engine, trackIndex](bool muted) {
                    engine.pushCommand(Logic::CmdSetKeySoundTrackMute{
                        .area       = Logic::KeySoundTrackArea::Bgm,
                        .trackIndex = trackIndex,
                        .muted      = muted,
                    });
                },
                [&engine, trackIndex](float gain) {
                    engine.pushCommand(Logic::CmdSetKeySoundTrackGain{
                        .area       = Logic::KeySoundTrackArea::Bgm,
                        .trackIndex = trackIndex,
                        .gain       = gain,
                    });
                },
                [](float) {});
            ImGui::PopID();
            ImGui::PopID();
            continue;
        }
    }

    ImGui::SetCursorPos(
        { contentStart.x,
          contentStart.y + static_cast<float>(totalRows) * rowHeight });
    ImGui::Dummy({ 1.0F, 1.0F });
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void ToolbarView::initializeColorPalette()
{
    loadSoftwareDefaultPalette();
    m_colorPaletteInitialized = true;
}

void ToolbarView::loadSkinDefaultPalette()
{
    fillPaletteWithSkinDefaults(m_paletteColors);
    fillBeatLinePaletteWithSkinDefaults(m_beatLinePaletteColors);
    m_overrideBeatLinePalette  = false;
    m_activePaletteSchemeIndex = -1;
    m_activePaletteSelection   = PaletteSelectionKind::SkinDefault;
    m_paletteSchemeErrorKey.clear();
    setPaletteSchemeNameBuffer(defaultPaletteSchemeName());
    pushPaletteToBrush();
    pushBeatLinePaletteToRenderer();
}

void ToolbarView::loadSoftwareDefaultPalette()
{
    const auto& settings   = Config::AppConfig::instance().getEditorSettings();
    const auto& schemeName = settings.defaultColorPaletteSchemeName;
    bool        loaded     = false;

    if ( !schemeName.empty() &&
         schemeName != Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID ) {
        const auto& paletteConfig = settings.colorPalettes;
        auto it = std::find_if(paletteConfig.schemes.begin(),
                               paletteConfig.schemes.end(),
                               [&](const Config::ColorPaletteScheme& scheme) {
                                   return scheme.name == schemeName;
                               });
        if ( it != paletteConfig.schemes.end() ) {
            applyStoredPaletteScheme(
                m_paletteColors, m_beatLinePaletteColors, *it);
            m_overrideBeatLinePalette = true;
            loaded                    = true;
        }
    }

    if ( !loaded ) {
        fillPaletteWithSkinDefaults(m_paletteColors);
        fillBeatLinePaletteWithSkinDefaults(m_beatLinePaletteColors);
        m_overrideBeatLinePalette = false;
    }
    m_activePaletteSchemeIndex = -1;
    m_activePaletteSelection   = PaletteSelectionKind::InheritSoftwareDefault;
    m_paletteSchemeErrorKey.clear();
    setPaletteSchemeNameBuffer(inheritedPaletteSchemeName());
    pushPaletteToBrush();
    pushBeatLinePaletteToRenderer();
}

bool ToolbarView::loadPaletteSchemeByName(const std::string& schemeName)
{
    if ( schemeName.empty() ||
         schemeName == Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID ) {
        loadSkinDefaultPalette();
        return true;
    }

    auto& paletteConfig =
        Config::AppConfig::instance().getEditorSettings().colorPalettes;
    auto it = std::find_if(paletteConfig.schemes.begin(),
                           paletteConfig.schemes.end(),
                           [&](const Config::ColorPaletteScheme& scheme) {
                               return scheme.name == schemeName;
                           });
    if ( it == paletteConfig.schemes.end() ) return false;

    loadPaletteScheme(static_cast<std::size_t>(
        std::distance(paletteConfig.schemes.begin(), it)));
    return true;
}

void ToolbarView::applyProjectPalettePreference()
{
    auto&       engine   = Logic::EditorEngine::instance();
    const auto* project  = engine.getCurrentProject();
    const auto& settings = Config::AppConfig::instance().getEditorSettings();

    std::string projectKey;
    std::string preferenceSource = "inherit";
    std::string schemeName       = settings.defaultColorPaletteSchemeName;
    if ( project ) {
        projectKey = Config::pathToUtf8(project->m_projectRoot);
        if ( !project->m_settings.m_colorPaletteSchemeName.empty() ) {
            preferenceSource = "project";
            schemeName       = project->m_settings.m_colorPaletteSchemeName;
        }
    }
    if ( schemeName.empty() ) {
        schemeName = Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
    }

    std::string applyKey = projectKey;
    applyKey.push_back('\n');
    applyKey += preferenceSource;
    applyKey.push_back('\n');
    applyKey += schemeName;
    if ( applyKey == m_lastAppliedProjectPaletteKey ) return;

    m_lastAppliedProjectPaletteKey = applyKey;
    if ( preferenceSource == "inherit" ) {
        loadSoftwareDefaultPalette();
        return;
    }
    if ( !loadPaletteSchemeByName(schemeName) ) {
        loadSkinDefaultPalette();
    }
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

void ToolbarView::pushBeatLinePaletteToRenderer()
{
    auto& engine                         = Logic::EditorEngine::instance();
    auto  config                         = engine.getEditorConfig();
    config.visual.overrideBeatLineColors = m_overrideBeatLinePalette;
    for ( std::size_t i = 0; i < m_beatLinePaletteColors.size(); ++i ) {
        config.visual.beatLineColors[i] =
            toStoredColor(m_beatLinePaletteColors[i]);
    }
    engine.setEditorConfig(config);
}

void ToolbarView::loadPaletteScheme(std::size_t schemeIndex)
{
    auto& app           = Config::AppConfig::instance();
    auto& paletteConfig = app.getEditorSettings().colorPalettes;
    if ( schemeIndex >= paletteConfig.schemes.size() ) return;

    const auto& scheme = paletteConfig.schemes[schemeIndex];
    applyStoredPaletteScheme(m_paletteColors, m_beatLinePaletteColors, scheme);
    m_overrideBeatLinePalette = true;

    m_activePaletteSchemeIndex      = static_cast<int>(schemeIndex);
    m_activePaletteSelection        = PaletteSelectionKind::Custom;
    paletteConfig.activeSchemeIndex = schemeIndex;
    m_paletteSchemeErrorKey.clear();
    setPaletteSchemeNameBuffer(scheme.name);
    pushPaletteToBrush();
    pushBeatLinePaletteToRenderer();
}

bool ToolbarView::canManageActivePaletteScheme() const
{
    const auto& paletteConfig =
        Config::AppConfig::instance().getEditorSettings().colorPalettes;
    return m_activePaletteSelection == PaletteSelectionKind::Custom &&
           m_activePaletteSchemeIndex >= 0 &&
           static_cast<std::size_t>(m_activePaletteSchemeIndex) <
               paletteConfig.schemes.size();
}

bool ToolbarView::hasPaletteSchemeNameConflict(
    const std::string& name, std::optional<std::size_t> ignoredIndex) const
{
    const auto& paletteConfig =
        Config::AppConfig::instance().getEditorSettings().colorPalettes;
    for ( std::size_t i = 0; i < paletteConfig.schemes.size(); ++i ) {
        if ( ignoredIndex && *ignoredIndex == i ) continue;
        if ( paletteConfig.schemes[i].name == name ) return true;
    }
    return false;
}

bool ToolbarView::validatePaletteSchemeNameForSave(
    const std::string& name, std::optional<std::size_t> ignoredIndex)
{
    if ( isReservedPaletteSchemeName(name) ) {
        m_paletteSchemeErrorKey = "ui.toolbar.note_palette.name_reserved_error";
        return false;
    }
    if ( hasPaletteSchemeNameConflict(name, ignoredIndex) ) {
        m_paletteSchemeErrorKey = "ui.toolbar.note_palette.name_conflict_error";
        return false;
    }

    m_paletteSchemeErrorKey.clear();
    return true;
}

void ToolbarView::savePaletteScheme(bool createNew)
{
    auto& app           = Config::AppConfig::instance();
    auto& paletteConfig = app.getEditorSettings().colorPalettes;

    Config::ColorPaletteScheme scheme          = buildCurrentPaletteScheme();
    bool                       hasActiveScheme = canManageActivePaletteScheme();
    if ( !createNew && !hasActiveScheme ) return;

    std::optional<std::size_t> ignoredIndex;
    if ( !createNew && hasActiveScheme ) {
        ignoredIndex = static_cast<std::size_t>(m_activePaletteSchemeIndex);
    }
    if ( !validatePaletteSchemeNameForSave(scheme.name, ignoredIndex) ) {
        return;
    }

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
    m_activePaletteSelection  = PaletteSelectionKind::Custom;
    m_overrideBeatLinePalette = true;
    setPaletteSchemeNameBuffer(
        paletteConfig.schemes[paletteConfig.activeSchemeIndex].name);
    pushBeatLinePaletteToRenderer();
    app.save();
}

void ToolbarView::openPaletteExportFilePicker()
{
    auto&             app         = Config::AppConfig::instance();
    auto&             settings    = app.getEditorSettings();
    const std::string defaultPath = settings.lastFilePickerPath.empty()
                                        ? std::string(".")
                                        : settings.lastFilePickerPath;
    const std::string defaultFileName =
        sanitizePaletteExportFileName(currentPaletteSchemeName());
    m_paletteExportStatusKey.clear();

    if ( settings.filePickerStyle == Config::FilePickerStyle::Native ) {
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      outPath    = nullptr;
        nfdu8filteritem_t filters[1] = { { "MusicMapMaker Color Palette",
                                           "mmpalette" } };
        const nfdresult_t result     = NFD_SaveDialogU8(
            &outPath, filters, 1, defaultPath.c_str(), defaultFileName.c_str());
        if ( result == NFD_OKAY && outPath != nullptr ) {
            exportCurrentPaletteToPath(outPath);
            NFD_FreePathU8(outPath);
        } else if ( result == NFD_OKAY ) {
            m_paletteExportSucceeded = false;
            m_paletteExportStatusKey = "ui.toolbar.note_palette.export_failed";
        } else if ( result == NFD_ERROR ) {
            const char* error = NFD_GetError();
            XERROR("Failed to open color palette export dialog: {}",
                   error ? error : "Unknown NFD error");
            m_paletteExportSucceeded = false;
            m_paletteExportStatusKey = "ui.toolbar.note_palette.export_failed";
        }
        return;
    }

    IGFD::FileDialogConfig dialogConfig;
    dialogConfig.path              = defaultPath;
    dialogConfig.countSelectionMax = 1;
    dialogConfig.fileName          = defaultFileName;
    dialogConfig.flags =
        ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened("NotePaletteExportPicker");
    ImGuiFileDialog::Instance()->OpenDialog(
        "NotePaletteExportPicker",
        TR("ui.toolbar.note_palette.export_dialog_title"),
        ".mmpalette",
        dialogConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened("NotePaletteExportPicker") ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

void ToolbarView::openPaletteImportFilePicker()
{
    auto&             app         = Config::AppConfig::instance();
    auto&             settings    = app.getEditorSettings();
    const std::string defaultPath = settings.lastFilePickerPath.empty()
                                        ? std::string(".")
                                        : settings.lastFilePickerPath;
    m_paletteImportErrorKey.clear();
    m_paletteImportStatusKey.clear();

    if ( settings.filePickerStyle == Config::FilePickerStyle::Native ) {
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      outPath    = nullptr;
        nfdu8filteritem_t filters[1] = { { "MusicMapMaker Color Palette",
                                           "mmpalette" } };
        const nfdresult_t result =
            NFD_OpenDialogU8(&outPath, filters, 1, defaultPath.c_str());
        if ( result == NFD_OKAY && outPath != nullptr ) {
            preparePaletteImportFromPath(outPath);
            NFD_FreePathU8(outPath);
        } else if ( result == NFD_OKAY ) {
            m_paletteImportSucceeded = false;
            m_paletteImportStatusKey = "ui.toolbar.note_palette.import_failed";
        } else if ( result == NFD_ERROR ) {
            const char* error = NFD_GetError();
            XERROR("Failed to open color palette import dialog: {}",
                   error ? error : "Unknown NFD error");
            m_paletteImportSucceeded = false;
            m_paletteImportStatusKey = "ui.toolbar.note_palette.import_failed";
        }
        return;
    }

    IGFD::FileDialogConfig dialogConfig;
    dialogConfig.path              = defaultPath;
    dialogConfig.countSelectionMax = 1;
    dialogConfig.flags =
        ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened("ColorPaletteImportPicker");
    ImGuiFileDialog::Instance()->OpenDialog(
        "ColorPaletteImportPicker",
        TR("ui.toolbar.note_palette.import_dialog_title"),
        ".mmpalette",
        dialogConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened("ColorPaletteImportPicker") ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

void ToolbarView::renderPaletteExportFileDialog(float dpiScale)
{
    Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
    if ( ImGuiFileDialog::Instance()->IsOpened("NotePaletteExportPicker") ) {
        Utils::prepareCenteredModalWindow({ 600, 400 });
    }
    if ( ImGuiFileDialog::Instance()->Display(
             "NotePaletteExportPicker",
             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings,
             { 600, 400 }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            exportCurrentPaletteToPath(
                ImGuiFileDialog::Instance()->GetFilePathName());
        }
        ImGuiFileDialog::Instance()->Close();
    }
}

void ToolbarView::renderPaletteImportFileDialog(float dpiScale)
{
    Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
    if ( ImGuiFileDialog::Instance()->IsOpened("ColorPaletteImportPicker") ) {
        Utils::prepareCenteredModalWindow({ 600, 400 });
    }
    if ( ImGuiFileDialog::Instance()->Display(
             "ColorPaletteImportPicker",
             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings,
             { 600, 400 }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            preparePaletteImportFromPath(
                ImGuiFileDialog::Instance()->GetFilePathName());
        }
        ImGuiFileDialog::Instance()->Close();
    }
}

void ToolbarView::exportCurrentPaletteToPath(const std::string& path)
{
    const std::string normalizedPath = normalizePaletteExportPath(path);
    const bool        succeeded =
        !normalizedPath.empty() &&
        Config::exportColorPaletteFile(Config::utf8ToPath(normalizedPath),
                                       buildCurrentPaletteScheme());
    m_paletteExportSucceeded = succeeded;
    m_paletteExportStatusKey = succeeded
                                   ? "ui.toolbar.note_palette.export_success"
                                   : "ui.toolbar.note_palette.export_failed";
    if ( !succeeded ) {
        return;
    }

    auto&                       app      = Config::AppConfig::instance();
    auto&                       settings = app.getEditorSettings();
    const std::filesystem::path parent =
        Config::utf8ToPath(normalizedPath).parent_path();
    if ( !parent.empty() ) {
        settings.lastFilePickerPath = Config::pathToUtf8(parent);
        app.save();
    }
}

void ToolbarView::preparePaletteImportFromPath(const std::string& path)
{
    Config::ColorPaletteScheme importedScheme;
    const bool                 succeeded =
        !path.empty() && Config::importColorPaletteFile(
                             Config::utf8ToPath(path), importedScheme);
    m_paletteImportSucceeded = succeeded;
    m_paletteImportStatusKey =
        succeeded ? std::string{} : "ui.toolbar.note_palette.import_failed";
    if ( !succeeded ) {
        return;
    }

    m_pendingImportedPaletteScheme = std::move(importedScheme);
    m_importPaletteSchemeNameBuffer.fill('\0');
    const std::string& importedName = m_pendingImportedPaletteScheme->name;
    const std::size_t  copyCount    = std::min(
        importedName.size(), m_importPaletteSchemeNameBuffer.size() - 1);
    std::copy_n(importedName.begin(),
                copyCount,
                m_importPaletteSchemeNameBuffer.begin());
    m_paletteImportErrorKey.clear();

    auto&                       app      = Config::AppConfig::instance();
    auto&                       settings = app.getEditorSettings();
    const std::filesystem::path parent = Config::utf8ToPath(path).parent_path();
    if ( !parent.empty() ) {
        settings.lastFilePickerPath = Config::pathToUtf8(parent);
        app.save();
    }
}

void ToolbarView::confirmPaletteImport()
{
    if ( !m_pendingImportedPaletteScheme ) return;

    const std::string name(m_importPaletteSchemeNameBuffer.data());
    if ( isReservedPaletteSchemeName(name) ) {
        m_paletteImportErrorKey = "ui.toolbar.note_palette.name_reserved_error";
        return;
    }
    if ( hasPaletteSchemeNameConflict(name, std::nullopt) ) {
        m_paletteImportErrorKey = "ui.toolbar.note_palette.name_conflict_error";
        return;
    }

    auto& app           = Config::AppConfig::instance();
    auto& paletteConfig = app.getEditorSettings().colorPalettes;
    m_pendingImportedPaletteScheme->name = name;
    paletteConfig.schemes.push_back(std::move(*m_pendingImportedPaletteScheme));
    const std::size_t importedIndex = paletteConfig.schemes.size() - 1;
    m_pendingImportedPaletteScheme.reset();
    m_paletteImportErrorKey.clear();
    m_paletteImportSucceeded = true;
    m_paletteImportStatusKey = "ui.toolbar.note_palette.import_success";
    loadPaletteScheme(importedIndex);
    app.save();
}

Config::ColorPaletteScheme ToolbarView::buildCurrentPaletteScheme() const
{
    Config::ColorPaletteScheme scheme;
    scheme.name = currentPaletteSchemeName();
    for ( std::size_t i = 0; i < m_paletteColors.size(); ++i ) {
        scheme.noteColors[i] = toStoredColor(m_paletteColors[i]);
    }
    for ( std::size_t i = 0; i < m_beatLinePaletteColors.size(); ++i ) {
        scheme.beatLineColors[i] = toStoredColor(m_beatLinePaletteColors[i]);
    }
    return scheme;
}

void ToolbarView::renamePaletteScheme()
{
    auto& app           = Config::AppConfig::instance();
    auto& paletteConfig = app.getEditorSettings().colorPalettes;
    if ( !canManageActivePaletteScheme() ) return;

    std::size_t index = static_cast<std::size_t>(m_activePaletteSchemeIndex);
    if ( index >= paletteConfig.schemes.size() ) return;

    std::string name = currentPaletteSchemeName();
    if ( !validatePaletteSchemeNameForSave(name, index) ) return;

    paletteConfig.schemes[index].name = name;
    paletteConfig.activeSchemeIndex   = index;
    setPaletteSchemeNameBuffer(name);
    app.save();
}

void ToolbarView::deletePaletteScheme(std::size_t schemeIndex)
{
    auto& app           = Config::AppConfig::instance();
    auto& settings      = app.getEditorSettings();
    auto& paletteConfig = settings.colorPalettes;
    if ( schemeIndex >= paletteConfig.schemes.size() ) return;

    const std::string deletedName = paletteConfig.schemes[schemeIndex].name;
    paletteConfig.schemes.erase(paletteConfig.schemes.begin() +
                                static_cast<std::ptrdiff_t>(schemeIndex));

    const bool deletedNameStillExists =
        std::any_of(paletteConfig.schemes.begin(),
                    paletteConfig.schemes.end(),
                    [&](const Config::ColorPaletteScheme& scheme) {
                        return scheme.name == deletedName;
                    });
    if ( !deletedNameStillExists &&
         settings.defaultColorPaletteSchemeName == deletedName ) {
        settings.defaultColorPaletteSchemeName =
            Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
    }

    auto& engine         = Logic::EditorEngine::instance();
    auto* project        = engine.getCurrentProject();
    bool  projectChanged = false;
    if ( !deletedNameStillExists && project &&
         project->m_settings.m_colorPaletteSchemeName == deletedName ) {
        project->m_settings.m_colorPaletteSchemeName.clear();
        projectChanged = true;
    }

    if ( paletteConfig.schemes.empty() ) {
        paletteConfig.activeSchemeIndex = 0;
    } else {
        paletteConfig.activeSchemeIndex =
            std::min(schemeIndex, paletteConfig.schemes.size() - 1);
    }

    m_pendingDeletePaletteSchemeIndex.reset();
    m_lastAppliedProjectPaletteKey.clear();
    m_paletteSchemeErrorKey.clear();
    loadSkinDefaultPalette();
    app.save();
    if ( projectChanged ) {
        engine.saveProject();
    }
}

void ToolbarView::setPaletteSchemeNameBuffer(const std::string& name)
{
    m_paletteSchemeNameBuffer.fill('\0');
    std::size_t count =
        std::min(name.size(), m_paletteSchemeNameBuffer.size() - 1);
    std::copy_n(name.begin(), count, m_paletteSchemeNameBuffer.begin());
}

void ToolbarView::setColorHexBuffer(PaletteTab tab, std::size_t slotIndex,
                                    glm::vec4 color)
{
    m_colorHexBuffer.fill('\0');
    std::string text  = colorToHexString(color);
    std::size_t count = std::min(text.size(), m_colorHexBuffer.size() - 1);
    std::copy_n(text.begin(), count, m_colorHexBuffer.begin());
    m_colorHexBufferTab  = tab;
    m_colorHexBufferSlot = slotIndex;
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

    if ( ImGui::Begin("##ColorPalettePopup", nullptr, popupFlags) ) {
        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.title").data());
        ImGui::Separator();

        auto& paletteConfig =
            Config::AppConfig::instance().getEditorSettings().colorPalettes;
        std::string previewName;
        if ( m_activePaletteSelection ==
             PaletteSelectionKind::InheritSoftwareDefault ) {
            previewName = inheritedPaletteSchemeName();
        } else if ( m_activePaletteSelection ==
                    PaletteSelectionKind::SkinDefault ) {
            previewName = defaultPaletteSchemeName();
        } else {
            std::size_t activeIndex =
                m_activePaletteSchemeIndex >= 0
                    ? static_cast<std::size_t>(m_activePaletteSchemeIndex)
                    : paletteConfig.schemes.size();
            previewName = activeIndex < paletteConfig.schemes.size()
                              ? paletteConfig.schemes[activeIndex].name
                              : defaultPaletteSchemeName();
        }

        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.scheme").data());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::floor(210.0f * dpiScale));
        if ( ::MMM::UI::FeedbackBeginCombo("##ColorPaletteScheme",
                                           previewName.c_str()) ) {
            const bool inheritSelected =
                m_activePaletteSelection ==
                PaletteSelectionKind::InheritSoftwareDefault;
            if ( ::MMM::UI::FeedbackSelectable(
                     inheritedPaletteSchemeName().c_str(), inheritSelected) ) {
                loadSoftwareDefaultPalette();
            }
            if ( inheritSelected ) ImGui::SetItemDefaultFocus();

            const bool skinSelected =
                m_activePaletteSelection == PaletteSelectionKind::SkinDefault;
            if ( ::MMM::UI::FeedbackSelectable(
                     defaultPaletteSchemeName().c_str(), skinSelected) ) {
                loadSkinDefaultPalette();
            }
            if ( skinSelected ) ImGui::SetItemDefaultFocus();

            if ( !paletteConfig.schemes.empty() ) {
                ImGui::Separator();
            }
            for ( std::size_t i = 0; i < paletteConfig.schemes.size(); ++i ) {
                bool selected =
                    m_activePaletteSelection == PaletteSelectionKind::Custom &&
                    m_activePaletteSchemeIndex == static_cast<int>(i);
                if ( ::MMM::UI::FeedbackSelectable(
                         paletteConfig.schemes[i].name.c_str(), selected) ) {
                    loadPaletteScheme(i);
                    Config::AppConfig::instance().save();
                }
                if ( selected ) ImGui::SetItemDefaultFocus();
            }
            ::MMM::UI::FeedbackEndCombo();
        }

        ImGui::TextUnformatted(
            TR("ui.toolbar.note_palette.scheme_name").data());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::floor(210.0f * dpiScale));
        if ( ImGui::InputText("##ColorPaletteSchemeName",
                              m_paletteSchemeNameBuffer.data(),
                              m_paletteSchemeNameBuffer.size()) ) {
            m_paletteSchemeErrorKey.clear();
        }

        const float schemeButtonH   = std::floor(24.0f * dpiScale);
        const float schemeButtonW   = std::floor(78.0f * dpiScale);
        const bool  canManageScheme = canManageActivePaletteScheme();
        ImGui::BeginDisabled(!canManageScheme);
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.toolbar.note_palette.save_scheme").data(),
                 ImVec2(schemeButtonW, schemeButtonH)) ) {
            savePaletteScheme(false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.toolbar.note_palette.new_scheme").data(),
                 ImVec2(schemeButtonW, schemeButtonH)) ) {
            savePaletteScheme(true);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!canManageScheme);
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.toolbar.note_palette.rename_scheme").data(),
                 ImVec2(schemeButtonW, schemeButtonH)) ) {
            renamePaletteScheme();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!canManageScheme);
        if ( ::MMM::UI::FeedbackButton(TR("ui.common.delete").data(),
                                       ImVec2(schemeButtonW, schemeButtonH)) ) {
            m_pendingDeletePaletteSchemeIndex =
                static_cast<std::size_t>(m_activePaletteSchemeIndex);
            ::MMM::UI::FeedbackOpenPopup("DeleteColorPaletteConfirm");
        }
        ImGui::EndDisabled();
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.toolbar.note_palette.import_scheme").data(),
                 ImVec2(schemeButtonW, schemeButtonH)) ) {
            openPaletteImportFilePicker();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.toolbar.note_palette.export_scheme").data(),
                 ImVec2(schemeButtonW, schemeButtonH)) ) {
            openPaletteExportFilePicker();
        }

        if ( ImGui::BeginPopup("DeleteColorPaletteConfirm") ) {
            ImGui::TextWrapped(
                "%s",
                TR("ui.toolbar.note_palette.delete_scheme_confirm").data());
            const float confirmButtonW = std::floor(92.0f * dpiScale);
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.common.confirm").data(),
                     ImVec2(confirmButtonW, schemeButtonH)) ) {
                auto pendingIndex = m_pendingDeletePaletteSchemeIndex;
                ImGui::CloseCurrentPopup();
                if ( pendingIndex ) {
                    deletePaletteScheme(*pendingIndex);
                }
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.common.cancel").data(),
                     ImVec2(confirmButtonW, schemeButtonH)) ) {
                m_pendingDeletePaletteSchemeIndex.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if ( m_pendingImportedPaletteScheme &&
             !ImGui::IsPopupOpen("ImportColorPaletteRename") ) {
            ::MMM::UI::FeedbackOpenPopup("ImportColorPaletteRename");
        }
        if ( ImGui::BeginPopupModal("ImportColorPaletteRename",
                                    nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoSavedSettings) ) {
            ImGui::TextUnformatted(
                TR("ui.toolbar.note_palette.import_rename_prompt").data());
            ImGui::SetNextItemWidth(std::floor(240.0f * dpiScale));
            if ( ImGui::InputText("##ImportedColorPaletteName",
                                  m_importPaletteSchemeNameBuffer.data(),
                                  m_importPaletteSchemeNameBuffer.size()) ) {
                m_paletteImportErrorKey.clear();
            }
            if ( !m_paletteImportErrorKey.empty() ) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      Utils::UIThemeUtils::getDangerColor());
                ImGui::TextWrapped("%s",
                                   TR(m_paletteImportErrorKey.c_str()).data());
                ImGui::PopStyleColor();
            }

            const float importButtonW = std::floor(92.0f * dpiScale);
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.toolbar.note_palette.import_scheme").data(),
                     ImVec2(importButtonW, schemeButtonH)) ) {
                confirmPaletteImport();
                if ( !m_pendingImportedPaletteScheme ) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.common.cancel").data(),
                     ImVec2(importButtonW, schemeButtonH)) ) {
                m_pendingImportedPaletteScheme.reset();
                m_paletteImportErrorKey.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if ( !m_paletteSchemeErrorKey.empty() ) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  Utils::UIThemeUtils::getDangerColor());
            ImGui::TextWrapped("%s",
                               TR(m_paletteSchemeErrorKey.c_str()).data());
            ImGui::PopStyleColor();
        }
        if ( !m_paletteExportStatusKey.empty() ) {
            if ( !m_paletteExportSucceeded ) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      Utils::UIThemeUtils::getDangerColor());
            }
            ImGui::TextWrapped("%s",
                               TR(m_paletteExportStatusKey.c_str()).data());
            if ( !m_paletteExportSucceeded ) {
                ImGui::PopStyleColor();
            }
        }
        if ( !m_paletteImportStatusKey.empty() ) {
            if ( !m_paletteImportSucceeded ) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      Utils::UIThemeUtils::getDangerColor());
            }
            ImGui::TextWrapped("%s",
                               TR(m_paletteImportStatusKey.c_str()).data());
            if ( !m_paletteImportSucceeded ) {
                ImGui::PopStyleColor();
            }
        }

        ImGui::Separator();

        if ( ImGui::BeginTabBar("##ColorPaletteTabs") ) {
            if ( ImGui::BeginTabItem(
                     TR("ui.toolbar.note_palette.note_tab").data()) ) {
                m_activePaletteTab = PaletteTab::Note;
                ImGui::EndTabItem();
            }
            if ( ImGui::BeginTabItem(
                     TR("ui.toolbar.note_palette.beat_line_tab").data()) ) {
                m_activePaletteTab = PaletteTab::BeatLine;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        const float swatchSize = std::floor(24.0f * dpiScale);
        if ( m_activePaletteTab == PaletteTab::Note ) {
            for ( std::size_t i = 0; i < Logic::NOTE_COLOR_SLOT_COUNT; ++i ) {
                auto slot   = static_cast<Logic::NoteColorSlot>(i);
                bool active = slot == m_activeColorSlot;
                ImGui::PushID(static_cast<int>(i));
                if ( ::MMM::UI::FeedbackColorButton(
                         "##SlotColor",
                         toImVec4(m_paletteColors[i]),
                         ImGuiColorEditFlags_NoTooltip |
                             ImGuiColorEditFlags_NoPicker |
                             ImGuiColorEditFlags_AlphaPreviewHalf,
                         ImVec2(swatchSize, swatchSize)) ) {
                    m_activeColorSlot = slot;
                }
                ImGui::SameLine();
                if ( ::MMM::UI::FeedbackSelectable(
                         TR(colorSlotLabelKey(slot)).data(),
                         active,
                         0,
                         ImVec2(std::floor(150.0f * dpiScale), swatchSize)) ) {
                    m_activeColorSlot = slot;
                }
                ImGui::PopID();
            }
        } else {
            for ( std::size_t i = 0; i < m_beatLinePaletteColors.size(); ++i ) {
                const bool active = i == m_activeBeatLineColorSlot;
                ImGui::PushID(static_cast<int>(i));
                if ( ::MMM::UI::FeedbackColorButton(
                         "##BeatLineSlotColor",
                         toImVec4(m_beatLinePaletteColors[i]),
                         ImGuiColorEditFlags_NoTooltip |
                             ImGuiColorEditFlags_NoPicker |
                             ImGuiColorEditFlags_AlphaPreviewHalf,
                         ImVec2(swatchSize, swatchSize)) ) {
                    m_activeBeatLineColorSlot = i;
                }
                ImGui::SameLine();
                if ( ::MMM::UI::FeedbackSelectable(
                         TR(beatLineColorSlotLabelKey(i)).data(),
                         active,
                         0,
                         ImVec2(std::floor(150.0f * dpiScale), swatchSize)) ) {
                    m_activeBeatLineColorSlot = i;
                }
                ImGui::PopID();
            }
        }

        ImGui::Separator();

        const bool        editingNote = m_activePaletteTab == PaletteTab::Note;
        const std::size_t activeSlotIndex =
            editingNote ? colorSlotIndex(m_activeColorSlot)
                        : m_activeBeatLineColorSlot;
        glm::vec4& activeColor = editingNote
                                     ? m_paletteColors[activeSlotIndex]
                                     : m_beatLinePaletteColors[activeSlotIndex];
        if ( m_colorHexBufferTab != m_activePaletteTab ||
             m_colorHexBufferSlot != activeSlotIndex ||
             !m_colorHexInputActive ) {
            setColorHexBuffer(m_activePaletteTab, activeSlotIndex, activeColor);
        }

        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.color_mode").data());
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackRadioButton("RGB##ColorPaletteMode",
                                            !m_colorPickerUseHsv) ) {
            m_colorPickerUseHsv = false;
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackRadioButton("HSV##ColorPaletteMode",
                                            m_colorPickerUseHsv) ) {
            m_colorPickerUseHsv = true;
        }

        ImGui::TextUnformatted(TR("ui.toolbar.note_palette.hex").data());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::floor(148.0f * dpiScale));
        bool hexChanged = ImGui::InputText("##PaletteColorHex",
                                           m_colorHexBuffer.data(),
                                           m_colorHexBuffer.size(),
                                           ImGuiInputTextFlags_CharsNoBlank);
        m_colorHexInputActive = ImGui::IsItemActive();
        if ( hexChanged ) {
            glm::vec4 parsedColor;
            if ( parseHexColor(m_colorHexBuffer.data(), parsedColor) ) {
                activeColor = parsedColor;
                if ( editingNote ) {
                    pushPaletteToBrush();
                } else {
                    m_overrideBeatLinePalette = true;
                    pushBeatLinePaletteToRenderer();
                }
            }
        }
        if ( ImGui::IsItemDeactivatedAfterEdit() ) {
            glm::vec4 parsedColor;
            if ( parseHexColor(m_colorHexBuffer.data(), parsedColor) ) {
                activeColor = parsedColor;
                setColorHexBuffer(
                    m_activePaletteTab, activeSlotIndex, activeColor);
                if ( editingNote ) {
                    pushPaletteToSelection();
                } else {
                    m_overrideBeatLinePalette = true;
                    pushBeatLinePaletteToRenderer();
                }
            } else {
                setColorHexBuffer(
                    m_activePaletteTab, activeSlotIndex, activeColor);
            }
            m_colorHexInputActive = false;
        }

        ImGuiColorEditFlags pickerFlags =
            ImGuiColorEditFlags_AlphaBar |
            ImGuiColorEditFlags_AlphaPreviewHalf |
            (m_colorPickerUseHsv ? ImGuiColorEditFlags_DisplayHSV
                                 : ImGuiColorEditFlags_DisplayRGB);

        if ( ImGui::ColorPicker4(
                 "##PaletteColorPicker", &activeColor.r, pickerFlags) ) {
            if ( editingNote ) {
                pushPaletteToBrush();
            } else {
                m_overrideBeatLinePalette = true;
                pushBeatLinePaletteToRenderer();
            }
        }
        if ( editingNote && ImGui::IsItemDeactivatedAfterEdit() ) {
            pushPaletteToSelection();
        }

        ImGui::Separator();

        const float buttonH = std::floor(26.0f * dpiScale);
        if ( editingNote ) {
            ImGui::TextUnformatted(
                TR("ui.toolbar.note_palette.skin_defaults").data());

            for ( std::size_t i = 0; i < Logic::NOTE_COLOR_SLOT_COUNT; ++i ) {
                if ( i > 0 ) ImGui::SameLine();
                auto slot         = static_cast<Logic::NoteColorSlot>(i);
                auto defaultColor = toVec4(skinColorForSlot(slot));
                ImGui::PushID(static_cast<int>(i + 100));
                if ( ::MMM::UI::FeedbackColorButton(
                         "##SkinDefaultColor",
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

            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.toolbar.note_palette.apply_selected").data(),
                     ImVec2(std::floor(140.0f * dpiScale), buttonH)) ) {
                pushPaletteToSelection();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.toolbar.note_palette.clear_custom").data(),
                     ImVec2(std::floor(140.0f * dpiScale), buttonH)) ) {
                auto slot   = m_activeColorSlot;
                activeColor = toVec4(skinColorForSlot(slot));
                pushColorCommands(slot, std::nullopt, true);
            }
        } else {
            ImGui::TextUnformatted(
                TR("ui.toolbar.note_palette.beat_line_skin_defaults").data());
            for ( std::size_t i = 0; i < m_beatLinePaletteColors.size(); ++i ) {
                if ( i > 0 ) ImGui::SameLine();
                const glm::vec4 defaultColor = toVec4(skinBeatLineColor(i));
                ImGui::PushID(static_cast<int>(i + 200));
                if ( ::MMM::UI::FeedbackColorButton(
                         "##SkinDefaultBeatLineColor",
                         toImVec4(defaultColor),
                         ImGuiColorEditFlags_NoTooltip |
                             ImGuiColorEditFlags_NoPicker |
                             ImGuiColorEditFlags_AlphaPreviewHalf,
                         ImVec2(swatchSize, swatchSize)) ) {
                    m_activeBeatLineColorSlot  = i;
                    m_beatLinePaletteColors[i] = defaultColor;
                    m_overrideBeatLinePalette  = true;
                    pushBeatLinePaletteToRenderer();
                }
                if ( ImGui::IsItemHovered() ) {
                    drawTooltip(TR(beatLineColorSlotLabelKey(i)).data());
                }
                ImGui::PopID();
            }

            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.toolbar.note_palette.use_skin_beat_lines").data(),
                     ImVec2(std::floor(180.0f * dpiScale), buttonH)) ) {
                fillBeatLinePaletteWithSkinDefaults(m_beatLinePaletteColors);
                m_overrideBeatLinePalette = true;
                pushBeatLinePaletteToRenderer();
            }
        }

        ImVec2 sz          = ImGui::GetWindowSize();
        m_colorPopupWidth  = sz.x;
        m_colorPopupHeight = sz.y;
    }
    ImGui::End();

    ImGui::PopStyleVar(4);
}

bool ToolbarView::drawIconButton(const char* icon, const char* id,
                                 const char* shortLabel, float width,
                                 float height, bool showLabel) const
{
    const bool clicked = ::MMM::UI::FeedbackButton(id, ImVec2(width, height));

    Config::SkinManager& skinCfg   = Config::SkinManager::instance();
    ImFont*              iconFont  = skinCfg.getFont("pure_icons");
    ImFont*              labelFont = skinCfg.getFont("menu");
    ImDrawList*          drawList  = ImGui::GetWindowDrawList();
    const ImVec2         minPos    = ImGui::GetItemRectMin();
    const ImVec2         maxPos    = ImGui::GetItemRectMax();
    const ImU32          textCol   = ImGui::GetColorU32(ImGuiCol_Text);
    const float          dpiScale =
        Config::AppConfig::instance().getWindowContentScale();

    if ( icon && iconFont ) {
        const float iconFontSize =
            showLabel ? std::floor(width * 0.58f) : ImGui::GetFontSize();
        const ImVec2 iconSize = iconFont->CalcTextSizeA(
            iconFontSize, std::numeric_limits<float>::max(), 0.0f, icon);
        const float  iconTopPadding = showLabel ? std::floor(4.0f * dpiScale)
                                                : (height - iconSize.y) * 0.5f;
        const ImVec2 iconPos        = {
            minPos.x + (width - iconSize.x) * 0.5f,
            minPos.y + iconTopPadding,
        };
        drawList->AddText(iconFont, iconFontSize, iconPos, textCol, icon);
    }

    if ( showLabel && shortLabel && shortLabel[0] != '\0' && labelFont ) {
        const float labelFontSize =
            std::floor(std::min(width * 0.38f, ImGui::GetFontSize() * 0.72f));
        const ImVec2 labelSize = labelFont->CalcTextSizeA(
            labelFontSize, std::numeric_limits<float>::max(), 0.0f, shortLabel);
        const ImVec2 labelPos = {
            minPos.x + (width - labelSize.x) * 0.5f,
            maxPos.y - labelSize.y - std::floor(3.0f * dpiScale),
        };
        drawList->AddText(
            labelFont, labelFontSize, labelPos, textCol, shortLabel);
    }

    return clicked;
}

void ToolbarView::drawToolButton(const char* icon, Logic::EditTool tool,
                                 const char* tooltip, float width, float height,
                                 const char* shortLabel, bool showLabel,
                                 UIManager* sourceManager)
{
    bool isActive = (m_currentTool == tool);

    if ( isActive ) {
        ImVec4 activeCol = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
    } else {
        Utils::UIThemeUtils::pushTransparentButtonStyles();
    }

    ImGui::PushID(static_cast<int>(tool));
    if ( drawIconButton(icon,
                        "##ToolbarToolButton",
                        shortLabel,
                        width,
                        height,
                        showLabel) ) {
        if ( m_currentTool != tool ) {
            m_currentTool = tool;
            if ( tool == Logic::EditTool::ColorBrush ) {
                pushPaletteToBrush();
            }
            Logic::EditorEngine::instance().pushCommand(
                Logic::CmdChangeTool{ tool });
            if ( sourceManager ) {
                auto* timeline = sourceManager->getView<Canvas::TimelineCanvas>(
                    "TimelineWindow");
                if ( timeline && timeline->wasFocusedLastFrame() ) {
                    timeline->requestFocus();
                }
            }
        }
    }
    ImGui::PopID();

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

void ToolbarView::drawLayoutButton(float width, float height, bool showLabel)
{
    const bool isActive = m_currentTool == Logic::EditTool::Layout;
    if ( isActive ) {
        const ImVec4 activeCol =
            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeCol);
    } else {
        Utils::UIThemeUtils::pushTransparentButtonStyles();
    }

    ImGui::PushID("LayoutTool");
    if ( drawIconButton(ICON_MMM_TRACK_LAYOUT,
                        "##ToolbarLayoutButton",
                        TR("ui.toolbar.short.layout").data(),
                        width,
                        height,
                        showLabel) ) {
        Logic::EditTool nextTool = Logic::EditTool::Layout;
        if ( isActive ) {
            nextTool          = m_toolBeforeLayout == Logic::EditTool::Layout
                                    ? Logic::EditTool::Move
                                    : m_toolBeforeLayout;
            m_showLayoutPopup = false;
        } else {
            m_toolBeforeLayout    = m_currentTool;
            m_showLayoutPopup     = true;
            m_showSoundEffectTool = false;
        }
        m_currentTool = nextTool;
        Logic::EditorEngine::instance().pushCommand(
            Logic::CmdChangeTool{ nextTool });
    }
    m_lastLayoutBtnY = ImGui::GetItemRectMin().y;
    ImGui::PopID();
    drawTooltip(TR("ui.toolbar.layout").data());
    ImGui::PopStyleColor(3);
}

void ToolbarView::renderMagnetPopup(float dpiScale)
{
    if ( !m_showMagnetPopup ) return;

    ImGuiWindow* toolbarWindow = ImGui::FindWindowByName(" ###Toolbar");
    if ( !toolbarWindow ) return;

    ImGuiViewport* mainViewport   = ImGui::GetMainViewport();
    const float    viewportTop    = mainViewport->Pos.y;
    const float    viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
    const float    viewportLeft   = mainViewport->Pos.x;
    const float    padding        = std::floor(8.0f * dpiScale);
    const float    popupW         = m_magnetPopupWidth > 0.0f
                                        ? m_magnetPopupWidth
                                        : std::floor(280.0f * dpiScale);
    const float    popupH         = m_magnetPopupHeight > 0.0f
                                        ? m_magnetPopupHeight
                                        : std::floor(360.0f * dpiScale);
    float          targetX = toolbarWindow->Pos.x - std::floor(4.0f * dpiScale);
    float          targetY = m_lastMagnetBtnY;
    targetX                = std::max(targetX, viewportLeft + popupW + padding);
    const float minTargetY = viewportTop + padding;
    const float maxTargetY =
        std::max(minTargetY, viewportBottom - popupH - padding);
    targetY = std::clamp(targetY, minTargetY, maxTargetY);

    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(
        ImVec2(targetX, targetY), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;
    auto&       appConfig  = Config::AppConfig::instance();
    const auto& aesthetics = appConfig.getEditorSettings().aesthetics;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        std::floor(aesthetics.windowRounding * dpiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(aesthetics.windowPadding * dpiScale),
               std::floor(aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                        std::floor(aesthetics.frameRounding * dpiScale));

    if ( ImGui::Begin("##MagnetToolPopup", nullptr, popupFlags) ) {
        ImGui::TextUnformatted(TR("ui.magnet_tool.title").data());
        ImGui::Separator();

        auto editorConfig  = appConfig.getEditorConfig();
        auto persistConfig = [&]() {
            Logic::EditorEngine::instance().setEditorConfig(editorConfig);
            appConfig.save();
        };

        bool scrollSnap = editorConfig.settings.scrollSnap;
        if ( ::MMM::UI::FeedbackCheckbox(
                 TR("ui.magnet_tool.scroll_canvas_snap").data(),
                 &scrollSnap) ) {
            editorConfig.settings.scrollSnap = scrollSnap;
            persistConfig();
        }

        bool objectPlacementSnap = editorConfig.settings.objectPlacementSnap;
        if ( ::MMM::UI::FeedbackCheckbox(
                 TR("ui.magnet_tool.object_placement_snap").data(),
                 &objectPlacementSnap) ) {
            editorConfig.settings.objectPlacementSnap = objectPlacementSnap;
            persistConfig();
        }

        if ( objectPlacementSnap ) {
            ImGui::Separator();
            auto mode = editorConfig.settings.objectPlacementSnapMode;
            if ( ::MMM::UI::FeedbackRadioButton(
                     TR("ui.magnet_tool.current_beat_lines").data(),
                     mode == Config::ObjectPlacementSnapMode::
                                 CurrentBeatDivisor) ) {
                mode = Config::ObjectPlacementSnapMode::CurrentBeatDivisor;
                editorConfig.settings.objectPlacementSnapMode = mode;
                persistConfig();
            }
            if ( ::MMM::UI::FeedbackRadioButton(
                     TR("ui.magnet_tool.common_beat_lines").data(),
                     mode == Config::ObjectPlacementSnapMode::
                                 CommonBeatDivisors) ) {
                mode = Config::ObjectPlacementSnapMode::CommonBeatDivisors;
                editorConfig.settings.objectPlacementSnapMode = mode;
                persistConfig();
            }

            if ( mode == Config::ObjectPlacementSnapMode::CommonBeatDivisors ) {
                ImGui::Spacing();
                ImGui::TextDisabled(
                    "%s", TR("ui.magnet_tool.common_beat_lines_hint").data());
                if ( ImGui::BeginTable("##CommonBeatDivisorSelection",
                                       4,
                                       ImGuiTableFlags_SizingFixedFit |
                                           ImGuiTableFlags_NoSavedSettings) ) {
                    for ( int divisor = Config::COMMON_BEAT_DIVISOR_MIN;
                          divisor <= Config::COMMON_BEAT_DIVISOR_MAX;
                          ++divisor ) {
                        ImGui::TableNextColumn();
                        bool selected = Config::isCommonBeatDivisorEnabled(
                            editorConfig.settings.commonBeatDivisorMask,
                            divisor);
                        char label[32];
                        std::snprintf(label,
                                      sizeof(label),
                                      "1/%d##CommonBeatDivisor%d",
                                      divisor,
                                      divisor);
                        if ( ::MMM::UI::FeedbackCheckbox(label, &selected) ) {
                            Config::setCommonBeatDivisorEnabled(
                                editorConfig.settings.commonBeatDivisorMask,
                                divisor,
                                selected);
                            persistConfig();
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }

        const ImVec2 size   = ImGui::GetWindowSize();
        m_magnetPopupWidth  = size.x;
        m_magnetPopupHeight = size.y;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void ToolbarView::renderBeatLinePopup(float dpiScale)
{
    if ( !m_showBeatLinePopup ) {
        if ( m_beatLinePopupConfigDirty ) {
            Config::AppConfig::instance().save();
            m_beatLinePopupConfigDirty = false;
        }
        return;
    }

    ImGuiWindow* toolbarWindow = ImGui::FindWindowByName(" ###Toolbar");
    if ( !toolbarWindow ) return;

    ImGuiViewport* mainViewport   = ImGui::GetMainViewport();
    const float    viewportTop    = mainViewport->Pos.y;
    const float    viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
    const float    viewportLeft   = mainViewport->Pos.x;
    const float    padding        = std::floor(8.0f * dpiScale);
    const float    popupW         = m_beatLinePopupWidth > 0.0f
                                        ? m_beatLinePopupWidth
                                        : std::floor(260.0f * dpiScale);
    const float    popupH         = m_beatLinePopupHeight > 0.0f
                                        ? m_beatLinePopupHeight
                                        : std::floor(220.0f * dpiScale);
    float          targetX = toolbarWindow->Pos.x - std::floor(4.0f * dpiScale);
    float          targetY = m_lastBeatLineBtnY;
    targetX                = std::max(targetX, viewportLeft + popupW + padding);
    const float minTargetY = viewportTop + padding;
    const float maxTargetY =
        std::max(minTargetY, viewportBottom - popupH - padding);
    targetY = std::clamp(targetY, minTargetY, maxTargetY);

    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(
        ImVec2(targetX, targetY), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;
    const auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        std::floor(aesthetics.windowRounding * dpiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(aesthetics.windowPadding * dpiScale),
               std::floor(aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                        std::floor(aesthetics.frameRounding * dpiScale));

    if ( ImGui::Begin("##BeatLineDisplayModePopup", nullptr, popupFlags) ) {
        ImGui::TextUnformatted(TR("ui.toolbar.beat_lines.title").data());
        ImGui::Separator();

        auto& appConfig  = Config::AppConfig::instance();
        auto  mode       = appConfig.getVisualConfig().beatLineDisplayMode;
        auto  selectMode = [&](Config::BeatLineDisplayMode candidate,
                               const char*                 labelKey) {
            if ( !::MMM::UI::FeedbackRadioButton(TR(labelKey).data(),
                                                 mode == candidate) ) {
                return;
            }
            auto updatedConfig = appConfig.getEditorConfig();
            updatedConfig.visual.beatLineDisplayMode = candidate;
            Logic::EditorEngine::instance().setEditorConfig(updatedConfig);
            m_beatLineDisplayModeHistory.observe(candidate);
            appConfig.save();
            m_beatLinePopupConfigDirty = false;
            mode                       = candidate;
        };

        selectMode(Config::BeatLineDisplayMode::Always,
                   "ui.toolbar.beat_lines.always");
        selectMode(Config::BeatLineDisplayMode::NearCursor,
                   "ui.toolbar.beat_lines.near_cursor");
        selectMode(Config::BeatLineDisplayMode::Hidden,
                   "ui.toolbar.beat_lines.hidden");

        if ( mode == Config::BeatLineDisplayMode::NearCursor ) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                                   std::floor(230.0f * dpiScale));
            ImGui::TextDisabled("%s",
                                TR("ui.toolbar.beat_lines.auto_hint").data());
            ImGui::PopTextWrapPos();

            float visiblePercent =
                appConfig.getVisualConfig().beatLineCursorVisibleRatio * 100.0f;
            ImGui::TextUnformatted(
                TR("ui.toolbar.beat_lines.visible_range").data());
            ImGui::SetNextItemWidth(std::floor(230.0f * dpiScale));
            if ( ::MMM::UI::FeedbackSliderFloat("##BeatLineVisibleRange",
                                                &visiblePercent,
                                                5.0f,
                                                50.0f,
                                                "%.0f%%") ) {
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual.beatLineCursorVisibleRatio =
                    visiblePercent * 0.01f;
                Logic::EditorEngine::instance().setEditorConfig(updatedConfig);
                m_beatLinePopupConfigDirty = true;
            }
            if ( ImGui::IsItemDeactivatedAfterEdit() &&
                 m_beatLinePopupConfigDirty ) {
                appConfig.save();
                m_beatLinePopupConfigDirty = false;
            }

            float fadePercent =
                appConfig.getVisualConfig().beatLineCursorFadeRatio * 100.0f;
            ImGui::TextUnformatted(
                TR("ui.toolbar.beat_lines.fade_range").data());
            ImGui::SetNextItemWidth(std::floor(230.0f * dpiScale));
            if ( ::MMM::UI::FeedbackSliderFloat("##BeatLineFadeRange",
                                                &fadePercent,
                                                2.0f,
                                                40.0f,
                                                "%.0f%%") ) {
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual.beatLineCursorFadeRatio =
                    fadePercent * 0.01f;
                Logic::EditorEngine::instance().setEditorConfig(updatedConfig);
                m_beatLinePopupConfigDirty = true;
            }
            if ( ImGui::IsItemDeactivatedAfterEdit() &&
                 m_beatLinePopupConfigDirty ) {
                appConfig.save();
                m_beatLinePopupConfigDirty = false;
            }
        }

        const ImVec2 size     = ImGui::GetWindowSize();
        m_beatLinePopupWidth  = size.x;
        m_beatLinePopupHeight = size.y;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void ToolbarView::renderLayoutPopup(float dpiScale)
{
    if ( !m_showLayoutPopup || m_currentTool != Logic::EditTool::Layout ) {
        if ( m_layoutComponentColorDirty ) {
            Config::AppConfig::instance().save();
            m_layoutComponentColorDirty = false;
        }
        if ( m_layoutSpectrumConfigDirty ) {
            Config::AppConfig::instance().save();
            m_layoutSpectrumConfigDirty = false;
        }
        if ( m_layoutVisualConfigDirty ) {
            Config::AppConfig::instance().save();
            m_layoutVisualConfigDirty = false;
        }
        m_layoutComponentColorPickerOpen = false;
        return;
    }

    ImGuiWindow* toolbarWindow = ImGui::FindWindowByName(" ###Toolbar");
    if ( !toolbarWindow ) return;

    ImGuiViewport* mainViewport   = ImGui::GetMainViewport();
    const float    viewportTop    = mainViewport->Pos.y;
    const float    viewportBottom = mainViewport->Pos.y + mainViewport->Size.y;
    const float    viewportLeft   = mainViewport->Pos.x;
    const float    padding        = std::floor(8.0f * dpiScale);
    const float    popupW         = m_layoutPopupWidth > 0.0f
                                        ? m_layoutPopupWidth
                                        : std::floor(260.0f * dpiScale);
    const float    popupH         = m_layoutPopupHeight > 0.0f
                                        ? m_layoutPopupHeight
                                        : std::floor(100.0f * dpiScale);
    float          targetX = toolbarWindow->Pos.x - std::floor(4.0f * dpiScale);
    float          targetY = m_lastLayoutBtnY;
    targetX                = std::max(targetX, viewportLeft + popupW + padding);
    const float minTargetY = viewportTop + padding;
    const float maxTargetY =
        std::max(minTargetY, viewportBottom - popupH - padding);
    targetY = std::clamp(targetY, minTargetY, maxTargetY);

    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(
        ImVec2(targetX, targetY), ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    const ImGuiWindowFlags popupFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;
    const auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                        std::floor(aesthetics.windowRounding * dpiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(aesthetics.windowPadding * dpiScale),
               std::floor(aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,
                        std::floor(aesthetics.frameRounding * dpiScale));

    if ( ImGui::Begin("##LayoutComponentsPopup", nullptr, popupFlags) ) {
        ImGui::TextUnformatted(TR("ui.toolbar.layout_settings").data());
        ImGui::Separator();

        auto&       appConfig          = Config::AppConfig::instance();
        const float colorButtonSize    = std::floor(22.0f * dpiScale);
        const float visualControlWidth = std::floor(230.0f * dpiScale);
        const auto  resetButtonLabel = TR("ui.toolbar.layout_component_reset");
        const float resetButtonWidth =
            ImGui::CalcTextSize(resetButtonLabel.data()).x +
            ImGui::GetStyle().FramePadding.x * 2.0f;
        bool       anyColorPickerOpen = false;
        const auto applyVisualConfig = [&](const Config::VisualConfig& visual) {
            auto updatedConfig   = appConfig.getEditorConfig();
            updatedConfig.visual = visual;
            Logic::EditorEngine::instance().setEditorConfig(updatedConfig);
        };
        const auto saveVisualAfterEdit = [&]() {
            if ( ImGui::IsItemDeactivatedAfterEdit() &&
                 m_layoutVisualConfigDirty ) {
                appConfig.save();
                m_layoutVisualConfigDirty = false;
            }
        };
        const auto drawRenderingResetButton = [&](const char* id,
                                                  auto&&      resetConfig) {
            ImGui::PushID(id);
            const float remainingWidth = ImGui::GetContentRegionAvail().x;
            if ( remainingWidth > resetButtonWidth ) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + remainingWidth -
                                     resetButtonWidth);
            }
            if ( ::MMM::UI::FeedbackSmallButton(resetButtonLabel.data()) ) {
                auto updatedConfig = appConfig.getEditorConfig();
                resetConfig(updatedConfig);
                appConfig.getEditorConfig() = updatedConfig;
                Logic::EditorEngine::instance().setEditorConfig(updatedConfig);
                appConfig.save();
                m_layoutVisualConfigDirty = false;
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(TR("ui.toolbar.layout_render_reset_hint").data());
            }
            ImGui::PopID();
        };

        if ( ::MMM::UI::FeedbackCollapsingHeader(
                 TR("ui.settings.visual.note").data()) ) {
            drawRenderingResetButton("NoteRenderingReset",
                                     [](Config::EditorConfig& config) {
                                         config.resetNoteRenderingToDefaults();
                                     });
            auto visual = appConfig.getVisualConfig();
            ImGui::TextUnformatted(
                TR("ui.settings.visual.note_scale_x").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat("##LayoutNoteScaleX",
                                                &visual.noteScaleX,
                                                0.5f,
                                                3.0f,
                                                "%.4f") ) {
                applyVisualConfig(visual);
                m_layoutVisualConfigDirty = true;
            }
            saveVisualAfterEdit();

            visual = appConfig.getVisualConfig();
            ImGui::TextUnformatted(
                TR("ui.settings.visual.note_scale_y").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat("##LayoutNoteScaleY",
                                                &visual.noteScaleY,
                                                0.5f,
                                                3.0f,
                                                "%.4f") ) {
                applyVisualConfig(visual);
                m_layoutVisualConfigDirty = true;
            }
            saveVisualAfterEdit();

            visual = appConfig.getVisualConfig();
            if ( ::MMM::UI::FeedbackCheckbox(
                     TR("ui.settings.visual.note_bound_sample_labels").data(),
                     &visual.showBoundSampleLabels) ) {
                applyVisualConfig(visual);
                appConfig.save();
                m_layoutVisualConfigDirty = false;
            }

            visual                   = appConfig.getVisualConfig();
            int         noteFillMode = static_cast<int>(visual.noteFillMode);
            const char* fillModes[]  = {
                TR("ui.settings.visual.fill_mode.stretch").data(),
                TR("ui.settings.visual.fill_mode.aspect_fit").data(),
                TR("ui.settings.visual.fill_mode.aspect_fill").data(),
                TR("ui.settings.visual.fill_mode.center").data(),
            };
            ImGui::TextUnformatted(
                TR("ui.settings.visual.note_fill_mode").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackCombo("##LayoutNoteFillMode",
                                          &noteFillMode,
                                          fillModes,
                                          IM_ARRAYSIZE(fillModes)) ) {
                visual.noteFillMode =
                    static_cast<Config::BackgroundFillMode>(noteFillMode);
                applyVisualConfig(visual);
                appConfig.save();
                m_layoutVisualConfigDirty = false;
            }

            auto&       settings      = appConfig.getEditorSettings();
            auto&       defaultScheme = settings.defaultColorPaletteSchemeName;
            const auto& paletteConfig = settings.colorPalettes;
            const std::string previewName =
                defaultScheme == Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID
                    ? std::string(
                          TR("ui.toolbar.note_palette.skin_default_scheme")
                              .data())
                    : defaultScheme;
            ImGui::TextUnformatted(
                TR("ui.settings.visual.note_palette_default").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackBeginCombo("##LayoutDefaultNotePalette",
                                               previewName.c_str()) ) {
                const bool skinSelected =
                    defaultScheme ==
                    Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
                if ( ::MMM::UI::FeedbackSelectable(
                         TR("ui.toolbar.note_palette.skin_default_scheme")
                             .data(),
                         skinSelected) ) {
                    defaultScheme =
                        Config::COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID;
                    appConfig.save();
                }
                if ( skinSelected ) ImGui::SetItemDefaultFocus();
                for ( const auto& scheme : paletteConfig.schemes ) {
                    const bool selected = defaultScheme == scheme.name;
                    if ( ::MMM::UI::FeedbackSelectable(scheme.name.c_str(),
                                                       selected) ) {
                        defaultScheme = scheme.name;
                        appConfig.save();
                    }
                    if ( selected ) ImGui::SetItemDefaultFocus();
                }
                ::MMM::UI::FeedbackEndCombo();
            }
            ImGui::TextDisabled(
                "%s", TR("ui.toolbar.layout_note_resize_hint").data());
        }

        if ( ::MMM::UI::FeedbackCollapsingHeader(
                 TR("ui.settings.visual.background").data()) ) {
            drawRenderingResetButton(
                "BackgroundRenderingReset", [](Config::EditorConfig& config) {
                    config.resetBackgroundRenderingToDefaults();
                });
            auto visual     = appConfig.getVisualConfig();
            int  bgFillMode = static_cast<int>(visual.background.fillMode);
            const char* fillModes[] = {
                TR("ui.settings.visual.fill_mode.stretch").data(),
                TR("ui.settings.visual.fill_mode.aspect_fit").data(),
                TR("ui.settings.visual.fill_mode.aspect_fill").data(),
                TR("ui.settings.visual.fill_mode.center").data(),
            };
            ImGui::TextUnformatted(
                TR("ui.settings.visual.bg_fill_mode").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackCombo("##LayoutBackgroundFillMode",
                                          &bgFillMode,
                                          fillModes,
                                          IM_ARRAYSIZE(fillModes)) ) {
                visual.background.fillMode =
                    static_cast<Config::BackgroundFillMode>(bgFillMode);
                applyVisualConfig(visual);
                appConfig.save();
                m_layoutVisualConfigDirty = false;
            }

            visual = appConfig.getVisualConfig();
            ImGui::TextUnformatted(TR("ui.settings.visual.bg_opaque").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat("##LayoutBackgroundOpaque",
                                                &visual.background.opaque_ratio,
                                                0.0f,
                                                1.0f,
                                                "%.4f") ) {
                applyVisualConfig(visual);
                m_layoutVisualConfigDirty = true;
            }
            saveVisualAfterEdit();

            visual = appConfig.getVisualConfig();
            ImGui::TextUnformatted(TR("ui.settings.visual.bg_darken").data());
            ImGui::SetNextItemWidth(visualControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat("##LayoutBackgroundDarken",
                                                &visual.background.darken_ratio,
                                                0.0f,
                                                1.0f,
                                                "%.4f") ) {
                applyVisualConfig(visual);
                m_layoutVisualConfigDirty = true;
            }
            saveVisualAfterEdit();
        }

        ImGui::TextUnformatted(TR("ui.toolbar.layout_components").data());
        ImGui::Separator();

        const auto drawComponentControl = [&](Config::CanvasComponentType type,
                                              std::string_view            label,
                                              bool showColor = true) {
            ImGui::PushID(static_cast<int>(type));
            bool visible = appConfig.getVisualConfig()
                               .canvasComponents.placement(type)
                               .visible;
            if ( ::MMM::UI::FeedbackCheckbox(label.data(), &visible) ) {
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual.canvasComponents.placement(type).visible =
                    visible;
                if ( type == Config::CanvasComponentType::BackgroundSpectrum ) {
                    updatedConfig.visual.background.spectrum.enabled = visible;
                }
                Logic::EditorEngine::instance().setEditorConfig(updatedConfig);
                appConfig.save();
            }

            if ( showColor ) {
                ImGui::SameLine();
                // 颜色按钮固定在复位列左侧，避免组件名称长度改变颜色列位置。
                const float contentRight =
                    ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
                const float colorColumnX = contentRight - resetButtonWidth -
                                           ImGui::GetStyle().ItemSpacing.x -
                                           colorButtonSize;
                if ( ImGui::GetCursorPosX() < colorColumnX ) {
                    ImGui::SetCursorPosX(colorColumnX);
                }
                const auto componentColor =
                    fromStoredColor(appConfig.getVisualConfig()
                                        .canvasComponents.placement(type)
                                        .color);
                if ( ::MMM::UI::FeedbackColorButton(
                         "##Color",
                         toImVec4(componentColor),
                         ImGuiColorEditFlags_NoTooltip |
                             ImGuiColorEditFlags_NoPicker |
                             ImGuiColorEditFlags_AlphaPreviewHalf,
                         ImVec2(colorButtonSize, colorButtonSize)) ) {
                    ::MMM::UI::FeedbackOpenPopup("ColorPicker");
                }
                if ( ImGui::IsItemHovered() ) {
                    drawTooltip(TR("ui.toolbar.layout_component_color").data());
                }
            }

            ImGui::SameLine();
            // 将所有复位按钮锚定到弹窗内容区右侧，避免标签长度造成错位。
            const float remainingWidth = ImGui::GetContentRegionAvail().x;
            if ( remainingWidth > resetButtonWidth ) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + remainingWidth -
                                     resetButtonWidth);
            }
            if ( ::MMM::UI::FeedbackSmallButton(resetButtonLabel.data()) ) {
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual.canvasComponents.resetPlacementToDefault(
                    type);
                if ( type == Config::CanvasComponentType::BackgroundSpectrum ) {
                    const Config::BackgroundSpectrumConfig defaults;
                    updatedConfig.visual.background.spectrum.widthRatio =
                        defaults.widthRatio;
                    updatedConfig.visual.background.spectrum.heightRatio =
                        defaults.heightRatio;
                    updatedConfig.visual.background.spectrum.baselineRatio =
                        defaults.baselineRatio;
                }
                Logic::EditorEngine::instance().setEditorConfig(updatedConfig);
                appConfig.save();
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(
                    TR("ui.toolbar.layout_component_reset_hint").data());
            }

            if ( showColor && ImGui::BeginPopup("ColorPicker") ) {
                anyColorPickerOpen = true;
                auto editableColor =
                    fromStoredColor(appConfig.getVisualConfig()
                                        .canvasComponents.placement(type)
                                        .color);
                if ( ImGui::ColorPicker4(
                         "##Value",
                         &editableColor.r,
                         ImGuiColorEditFlags_AlphaBar |
                             ImGuiColorEditFlags_AlphaPreviewHalf |
                             ImGuiColorEditFlags_DisplayRGB) ) {
                    auto updatedConfig = appConfig.getEditorConfig();
                    updatedConfig.visual.canvasComponents.placement(type)
                        .color = toStoredColor(editableColor);
                    Logic::EditorEngine::instance().setEditorConfig(
                        updatedConfig);
                    m_layoutComponentColorDirty = true;
                }
                if ( ImGui::IsItemDeactivatedAfterEdit() &&
                     m_layoutComponentColorDirty ) {
                    appConfig.save();
                    m_layoutComponentColorDirty = false;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        };

        drawComponentControl(Config::CanvasComponentType::JudgmentLineTime,
                             TR("ui.toolbar.layout_current_judgment_time"));
        drawComponentControl(Config::CanvasComponentType::BeatNumber,
                             TR("ui.toolbar.layout_beat_number"));
        drawComponentControl(Config::CanvasComponentType::BeatLineTime,
                             TR("ui.toolbar.layout_beat_line_time"));
        drawComponentControl(Config::CanvasComponentType::BackgroundSpectrum,
                             TR("ui.toolbar.layout_background_spectrum"),
                             false);
        if ( ::MMM::UI::FeedbackCollapsingHeader(
                 TR("ui.toolbar.layout_background_spectrum_settings")
                     .data()) ) {
            const float spectrumControlWidth = std::floor(230.0F * dpiScale);
            const auto  applySpectrumConfig =
                [&](const Config::BackgroundSpectrumConfig& spectrum) {
                    auto updatedConfig = appConfig.getEditorConfig();
                    updatedConfig.visual.background.spectrum = spectrum;
                    updatedConfig.visual.background.spectrum.enabled =
                        updatedConfig.visual.canvasComponents.backgroundSpectrum
                            .visible;
                    Logic::EditorEngine::instance().setEditorConfig(
                        updatedConfig);
                };
            const auto saveSpectrumAfterEdit = [&]() {
                if ( ImGui::IsItemDeactivatedAfterEdit() &&
                     m_layoutSpectrumConfigDirty ) {
                    appConfig.save();
                    m_layoutSpectrumConfigDirty = false;
                }
            };
            const auto drawSpectrumColorControl =
                [&](std::string_view label,
                    const char*      id,
                    std::array<float, 4>
                        Config::BackgroundSpectrumConfig::* colorMember) {
                    auto spectrum =
                        appConfig.getVisualConfig().background.spectrum;
                    auto editableColor = fromStoredColor(spectrum.*colorMember);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(label.data());
                    ImGui::SameLine();
                    const float remainingWidth =
                        ImGui::GetContentRegionAvail().x;
                    if ( remainingWidth > colorButtonSize ) {
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                             remainingWidth - colorButtonSize);
                    }
                    ImGui::PushID(id);
                    if ( ::MMM::UI::FeedbackColorButton(
                             "##Color",
                             toImVec4(editableColor),
                             ImGuiColorEditFlags_NoTooltip |
                                 ImGuiColorEditFlags_NoPicker |
                                 ImGuiColorEditFlags_AlphaPreviewHalf,
                             ImVec2(colorButtonSize, colorButtonSize)) ) {
                        ::MMM::UI::FeedbackOpenPopup("ColorPicker");
                    }
                    if ( ImGui::BeginPopup("ColorPicker") ) {
                        anyColorPickerOpen = true;
                        if ( ImGui::ColorPicker4(
                                 "##Value",
                                 &editableColor.r,
                                 ImGuiColorEditFlags_AlphaBar |
                                     ImGuiColorEditFlags_AlphaPreviewHalf |
                                     ImGuiColorEditFlags_DisplayRGB) ) {
                            spectrum =
                                appConfig.getVisualConfig().background.spectrum;
                            spectrum.*colorMember =
                                toStoredColor(editableColor);
                            applySpectrumConfig(spectrum);
                            m_layoutSpectrumConfigDirty = true;
                        }
                        saveSpectrumAfterEdit();
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                };

            auto spectrum = appConfig.getVisualConfig().background.spectrum;
            ImGui::TextUnformatted(
                TR("ui.settings.visual.background_spectrum.band_count").data());
            ImGui::SetNextItemWidth(spectrumControlWidth);
            if ( ::MMM::UI::FeedbackSliderInt(
                     "##LayoutBackgroundSpectrumBands",
                     &spectrum.bandCount,
                     Config::BACKGROUND_SPECTRUM_MIN_BANDS,
                     Config::BACKGROUND_SPECTRUM_MAX_BANDS) ) {
                applySpectrumConfig(spectrum);
                m_layoutSpectrumConfigDirty = true;
            }
            saveSpectrumAfterEdit();

            spectrum = appConfig.getVisualConfig().background.spectrum;
            ImGui::TextUnformatted(
                TR("ui.settings.visual.background_spectrum.width_ratio")
                    .data());
            ImGui::SetNextItemWidth(spectrumControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##LayoutBackgroundSpectrumWidth",
                     &spectrum.widthRatio,
                     0.10F,
                     1.0F,
                     "%.2f") ) {
                applySpectrumConfig(spectrum);
                m_layoutSpectrumConfigDirty = true;
            }
            saveSpectrumAfterEdit();

            spectrum = appConfig.getVisualConfig().background.spectrum;
            ImGui::TextUnformatted(
                TR("ui.settings.visual.background_spectrum.height_ratio")
                    .data());
            ImGui::SetNextItemWidth(spectrumControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##LayoutBackgroundSpectrumHeight",
                     &spectrum.heightRatio,
                     0.05F,
                     1.0F,
                     "%.2f") ) {
                applySpectrumConfig(spectrum);
                m_layoutSpectrumConfigDirty = true;
            }
            saveSpectrumAfterEdit();

            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.settings.visual.background_spectrum."
                        "match_canvas_aspect")
                         .data(),
                     ImVec2(spectrumControlWidth, 0.0F)) ) {
                spectrum.widthRatio  = 1.0F;
                spectrum.heightRatio = 1.0F;
                applySpectrumConfig(spectrum);
                appConfig.save();
                m_layoutSpectrumConfigDirty = false;
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(TR("ui.settings.visual.background_spectrum."
                               "match_canvas_aspect_hint")
                                .data());
            }

            drawSpectrumColorControl(
                TR("ui.settings.visual.background_spectrum.left_bar_color"),
                "BackgroundLevelLeft",
                &Config::BackgroundSpectrumConfig::leftBarColor);
            drawSpectrumColorControl(
                TR("ui.settings.visual.background_spectrum.right_bar_color"),
                "BackgroundLevelRight",
                &Config::BackgroundSpectrumConfig::rightBarColor);

            spectrum = appConfig.getVisualConfig().background.spectrum;
            ImGui::TextUnformatted(
                TR("ui.settings.visual.background_spectrum.opacity").data());
            ImGui::SetNextItemWidth(spectrumControlWidth);
            if ( ::MMM::UI::FeedbackSliderFloat(
                     "##LayoutBackgroundSpectrumOpacity",
                     &spectrum.opacity,
                     0.0F,
                     1.0F,
                     "%.2f") ) {
                applySpectrumConfig(spectrum);
                m_layoutSpectrumConfigDirty = true;
            }
            saveSpectrumAfterEdit();

            spectrum = appConfig.getVisualConfig().background.spectrum;
            if ( ::MMM::UI::FeedbackCheckbox(
                     TR("ui.settings.visual.background_spectrum."
                        "include_hit_effects")
                         .data(),
                     &spectrum.includeHitEffects) ) {
                applySpectrumConfig(spectrum);
                appConfig.save();
                m_layoutSpectrumConfigDirty = false;
            }
        }
        drawComponentControl(Config::CanvasComponentType::Kps,
                             TR("ui.toolbar.layout_kps"));
        if ( ::MMM::UI::FeedbackCollapsingHeader(
                 TR("ui.toolbar.layout_kps_sync_settings").data(),
                 ImGuiTreeNodeFlags_DefaultOpen) ) {
            bool syncKpsTrackSizes =
                appConfig.getVisualConfig().canvasComponents.syncKpsTrackSizes;
            if ( ::MMM::UI::FeedbackCheckbox(
                     TR("ui.toolbar.layout_kps_sync_track_sizes").data(),
                     &syncKpsTrackSizes) ) {
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual.canvasComponents.syncKpsTrackSizes =
                    syncKpsTrackSizes;
                Logic::EditorEngine::instance().setEditorConfig(updatedConfig);
                appConfig.save();
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(
                    TR("ui.toolbar.layout_kps_sync_track_sizes_hint").data());
            }

            bool syncKpsTrackRelativePositions =
                appConfig.getVisualConfig()
                    .canvasComponents.syncKpsTrackRelativePositions;
            if ( ::MMM::UI::FeedbackCheckbox(
                     TR("ui.toolbar.layout_kps_sync_track_positions").data(),
                     &syncKpsTrackRelativePositions) ) {
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual.canvasComponents
                    .setSyncKpsTrackRelativePositions(
                        syncKpsTrackRelativePositions);
                Logic::EditorEngine::instance().setEditorConfig(updatedConfig);
                appConfig.save();
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(
                    TR("ui.toolbar.layout_kps_sync_track_positions_hint")
                        .data());
            }

            bool syncAllKpsComponentPositions =
                appConfig.getVisualConfig()
                    .canvasComponents.syncAllKpsComponentPositions;
            if ( ::MMM::UI::FeedbackCheckbox(
                     TR("ui.toolbar.layout_kps_sync_all_positions").data(),
                     &syncAllKpsComponentPositions) ) {
                auto updatedConfig = appConfig.getEditorConfig();
                updatedConfig.visual.canvasComponents
                    .setSyncAllKpsComponentPositions(
                        syncAllKpsComponentPositions);
                Logic::EditorEngine::instance().setEditorConfig(updatedConfig);
                appConfig.save();
            }
            if ( ImGui::IsItemHovered() ) {
                drawTooltip(
                    TR("ui.toolbar.layout_kps_sync_all_positions_hint").data());
            }
        }

        if ( m_layoutComponentColorPickerOpen && !anyColorPickerOpen ) {
            if ( m_layoutComponentColorDirty || m_layoutSpectrumConfigDirty ||
                 m_layoutVisualConfigDirty ) {
                appConfig.save();
                m_layoutComponentColorDirty = false;
                m_layoutSpectrumConfigDirty = false;
                m_layoutVisualConfigDirty   = false;
            }
        }
        m_layoutComponentColorPickerOpen = anyColorPickerOpen;

        ImGui::TextDisabled("%s",
                            TR("ui.toolbar.layout_component_drag_hint").data());

        const ImVec2 size   = ImGui::GetWindowSize();
        m_layoutPopupWidth  = size.x;
        m_layoutPopupHeight = size.y;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void ToolbarView::drawTooltip(const char* text)
{
    Utils::renderTooltip(text, Utils::TooltipDir::Left);
}

}  // namespace MMM::UI
