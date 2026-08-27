#include "canvas/TimelineCanvas.h"
#include "audio/AudioManager.h"
#include "canvas/TimelineTimingTooltip.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/system/render/Batcher.h"
#include "ui/Icons.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace MMM::Canvas
{
namespace
{
/// @brief 判断 ImGui 窗口是否为左侧工具栏窗口。
/// @param window 待判断的 ImGui 窗口。
/// @return 指向工具栏窗口时返回 true。
bool isToolbarWindow(const ImGuiWindow* window)
{
    return window && std::strcmp(window->Name, " ###Toolbar") == 0;
}

/// @brief 判断当前 ImGui 焦点或悬浮窗口是否为工具栏。
/// @return 工具栏正处理鼠标或键盘焦点时返回 true。
bool isToolbarFocusedOrHovered()
{
    const ImGuiContext* context = ImGui::GetCurrentContext();
    if ( !context ) {
        return false;
    }
    return isToolbarWindow(context->NavWindow) ||
           isToolbarWindow(context->HoveredWindow);
}

/// @brief 在鼠标附近绘制播放速度临时提示窗口。
/// @param speedValue 当前播放速度倍率。
/// @warning UI 热路径：仅在速度提示计时器生效时绘制一个轻量 ImGui 窗口。
void renderPlaybackSpeedTooltip(float speedValue)
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2         mousePos = ImGui::GetMousePos();

    ImVec2 pivot = ImVec2(0.0f, 0.0f);
    if ( mousePos.x > viewport->WorkPos.x + viewport->WorkSize.x * 0.7f ) {
        pivot.x = 1.0f;
    }
    if ( mousePos.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.7f ) {
        pivot.y = 1.0f;
    }

    const float offsetX = (pivot.x == 0.0f) ? 20.0f : -20.0f;
    const float offsetY = (pivot.y == 0.0f) ? 20.0f : -20.0f;

    ImGui::SetNextWindowPos(ImVec2(mousePos.x + offsetX, mousePos.y + offsetY),
                            ImGuiCond_Always,
                            pivot);
    ImGui::SetNextWindowBgAlpha(0.7f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 10.0f));
    if ( ImGui::Begin("##TimelineSpeedTooltip", nullptr, flags) ) {
        ImFont* font = Config::SkinManager::instance().getFont("content");
        if ( font ) ImGui::PushFont(font, font->LegacySize);
        ImGui::Text(TR("ui.toolbar.playback_speed_value").data(), speedValue);
        if ( font ) ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

/// @brief 处理 Timeline 窗格上的修饰键滚轮操作。
/// @param timelineId Timeline 对应的画布 ID，用于隔离 Alt 滚轮累积量。
/// @param wheel 当前帧滚轮增量。
/// @param isCommandPressed Ctrl 或 Command 是否按下。
/// @param isAltPressed Alt 是否按下。
/// @param isShiftPressed Shift 是否按下。
/// @param speedTooltipValue 输出速度提示窗口需要显示的速度倍率。
/// @param speedTooltipTimer 输出速度提示窗口剩余显示时间。
/// @return 已处理修饰滚轮时返回 true。
/// @warning UI 热路径：Timeline 悬停滚轮时调用；会按播放配置短暂锁定
/// SessionRegistry，并发布轻量命令。
bool handleTimelineModifierWheel(const std::string& timelineId, float wheel,
                                 bool isCommandPressed, bool isAltPressed,
                                 bool isShiftPressed, float& speedTooltipValue,
                                 float& speedTooltipTimer)
{
    if ( std::abs(wheel) <= 0.01f || (!isCommandPressed && !isAltPressed) ) {
        return false;
    }

    const bool shouldPlayAdjustmentFeedback =
        Logic::EditorEngine::instance()
            .getEditorConfig()
            .settings.stopPlaybackOnScroll;
    Event::EventBus::instance().publish(Event::LogicCommandEvent(
        Logic::CmdScroll{ timelineId,
                          0.0f,
                          false,
                          Logic::ScrollCommandIntent::ModifierAdjustment }));

    if ( isCommandPressed && isAltPressed ) {
        constexpr std::array<double, 4> presets = { 0.25, 0.50, 0.75, 1.0 };
        double                          currentSpeed =
            Audio::AudioManager::instance().getPlaybackSpeed();

        std::size_t bestIdx = 0;
        double      minDiff = std::abs(currentSpeed - presets[0]);
        for ( std::size_t i = 1; i < presets.size(); ++i ) {
            const double diff = std::abs(currentSpeed - presets[i]);
            if ( diff < minDiff ) {
                minDiff = diff;
                bestIdx = i;
            }
        }

        if ( wheel > 0.01f ) {
            if ( bestIdx < presets.size() - 1U ) bestIdx++;
        } else if ( wheel < -0.01f ) {
            if ( bestIdx > 0U ) bestIdx--;
        }

        const double newSpeed = presets[bestIdx];
        if ( std::abs(newSpeed - currentSpeed) > 1e-4 ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdSetPlaybackSpeed{ newSpeed }));
            speedTooltipValue = static_cast<float>(newSpeed);
            speedTooltipTimer = 2.0f;
            if ( shouldPlayAdjustmentFeedback ) {
                ::MMM::UI::PlayInteractionMouseUpFeedback();
            }
        }
        return true;
    }

    if ( isCommandPressed ) {
        auto  editorCfg = Logic::EditorEngine::instance().getEditorConfig();
        float step      = 0.1f;
        if ( isShiftPressed ) {
            step *= editorCfg.settings.scrollSpeedMultiplier;
        }
        const float currentZoom = editorCfg.visual.timelineZoom;
        const float newZoom =
            std::clamp(currentZoom + wheel * step, 0.1f, 10.0f);
        if ( std::abs(newZoom - currentZoom) > 0.0001f ) {
            editorCfg.visual.timelineZoom = newZoom;
            Logic::EditorEngine::instance().setEditorConfig(editorCfg);
            if ( shouldPlayAdjustmentFeedback ) {
                ::MMM::UI::PlayInteractionMouseUpFeedback();
            }
        }
        return true;
    }

    auto      editorCfg = Logic::EditorEngine::instance().getEditorConfig();
    const int originalDivisor = editorCfg.settings.beatDivisor;
    static std::unordered_map<std::string, float> wheelAccumulator;
    float& acc = wheelAccumulator[timelineId];
    acc += wheel;

    int steps = 0;
    if ( acc >= 1.0f ) {
        steps = static_cast<int>(acc);
        acc -= static_cast<float>(steps);
    } else if ( acc <= -1.0f ) {
        steps = static_cast<int>(acc);
        acc -= static_cast<float>(steps);
    }

    if ( steps == 0 ) {
        return true;
    }

    if ( isShiftPressed ) {
        constexpr std::array<int, 8> presets = { 1, 2, 3, 4, 6, 8, 12, 16 };
        int                          current = editorCfg.settings.beatDivisor;
        if ( steps > 0 ) {
            for ( int i = 0; i < steps; ++i ) {
                auto it =
                    std::upper_bound(presets.begin(), presets.end(), current);
                current = it != presets.end() ? *it : presets.back();
            }
        } else {
            for ( int i = 0; i < -steps; ++i ) {
                auto it =
                    std::lower_bound(presets.begin(), presets.end(), current);
                current = it != presets.begin() ? *(--it) : presets.front();
            }
        }
        editorCfg.settings.beatDivisor = current;
    } else {
        editorCfg.settings.beatDivisor += steps;
    }

    editorCfg.settings.beatDivisor =
        std::clamp(editorCfg.settings.beatDivisor, 1, 64);
    if ( editorCfg.settings.beatDivisor != originalDivisor ) {
        Logic::EditorEngine::instance().setEditorConfig(editorCfg);
        if ( shouldPlayAdjustmentFeedback ) {
            ::MMM::UI::PlayInteractionMouseUpFeedback();
        }
    }
    return true;
}

/// @brief 根据齿轮颜色选择具有稳定对比度的中性底板颜色。
/// @param gearColor 齿轮文字颜色。
/// @param hovered 齿轮当前是否悬浮。
/// @return 对比色底板的 ImGui 打包颜色。
/// @warning UI 热路径：每个可见齿轮每帧调用，只执行常量算术。
ImU32 timelineGearBackgroundColor(const ImVec4& gearColor, bool hovered)
{
    const float luminance = 0.2126f * gearColor.x * gearColor.x +
                            0.7152f * gearColor.y * gearColor.y +
                            0.0722f * gearColor.z * gearColor.z;
    const int   alpha     = hovered ? 242 : 218;
    if ( luminance < 0.18f ) {
        return IM_COL32(248, 250, 255, alpha);
    }
    return IM_COL32(8, 11, 18, alpha);
}

/// @brief Timeline 画布齿轮按钮的类型信息
struct TimelineGearInfo {
    /// @brief 对应 TimelineInteractiveElement 的效果掩码。
    uint32_t mask;

    /// @brief 对应 Timing 类型。
    ::MMM::TimingEffect effect;

    /// @brief 对应 TimelineInteractiveElement 的实体字段。
    entt::entity Logic::TimelineInteractiveElement::* entity;

    /// @brief 对应 TimelineInteractiveElement 的参数值字段。
    double Logic::TimelineInteractiveElement::* value;

    /// @brief 显示标签。
    const char* label;

    /// @brief 编辑弹窗类型。
    const char* editType;

    /// @brief 齿轮文字颜色。
    ImVec4 color;

    /// @brief 是否显示在 Timeline 右侧。
    bool rightSide;
};

/// @brief 将创建弹窗索引转换为 Timeline Timing 类型。
::MMM::TimingEffect timelineEffectFromCreateType(int createType)
{
    switch ( createType ) {
    case 0: return ::MMM::TimingEffect::BPM;
    case 2: return ::MMM::TimingEffect::JUMP;
    case 3: return ::MMM::TimingEffect::HS;
    case 1:
    default: return ::MMM::TimingEffect::SCROLL;
    }
}

/// @brief 获取 Timeline Timing 类型的渲染颜色。
glm::vec4 timelineEffectColor(::MMM::TimingEffect effect, float alpha)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return { 1.0f, 0.28f, 0.28f, alpha };
    case ::MMM::TimingEffect::SCROLL: return { 0.28f, 1.0f, 0.34f, alpha };
    case ::MMM::TimingEffect::JUMP: return { 0.32f, 0.53f, 1.0f, alpha };
    case ::MMM::TimingEffect::HS: return { 1.0f, 0.87f, 0.28f, alpha };
    }
    return { 1.0f, 1.0f, 1.0f, alpha };
}

/// @brief 获取专业模式中指定 Timing 类型所属的轨道索引。
int professionalTimingLane(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return 0;
    case ::MMM::TimingEffect::SCROLL: return 1;
    case ::MMM::TimingEffect::JUMP: return 2;
    case ::MMM::TimingEffect::HS: return 3;
    }
    return 0;
}

/// @brief 生成用于去重同一个 marker glow 命令的键。
uint64_t timelineMarkerKey(uint32_t indexOffset, uint32_t indexCount)
{
    return (static_cast<uint64_t>(indexOffset) << 32U) |
           static_cast<uint64_t>(indexCount);
}

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

/// @brief 提交总时间轴最近一次连续 Seek。
/// @warning UI 热路径：仅在拖动结束或窗口中断交互时发布一条命令。
void TimelineCanvas::commitAudioTimeSliderScrub()
{
    if ( !m_isAudioTimeSliderScrubbing ) return;
    Event::EventBus::instance().publish(Event::LogicCommandEvent(Logic::CmdSeek{
        .time        = m_audioTimeSliderScrubTarget,
        .isScrubbing = false,
    }));
    m_isAudioTimeSliderScrubbing = false;
}

/// @brief 更新 Timeline 窗口、画布交互与叠加控件。
/// @param sourceManager UI 管理器。
/// @warning UI 热路径：窗口可见时每帧调用，只做绘制和输入处理。
/// 禁止引入文件系统访问、阻塞操作或全量排序。
void TimelineCanvas::update(UI::UIManager* sourceManager)
{
    auto& appConfig      = Config::AppConfig::instance();
    auto& editorSettings = appConfig.getEditorSettings();
    if ( !editorSettings.showTimelineWindow ) {
        commitAudioTimeSliderScrub();
        return;
    }

    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;

    std::string windowName =
        fmt::format("{}###{}", TR("canvas.timeline"), m_name);
    bool windowOpen = editorSettings.showTimelineWindow;

    if ( m_shouldFocusNextFrame ) {
        ImGui::SetNextWindowFocus();
        m_shouldFocusNextFrame = false;
    }
    UI::LayoutContext lctx(m_layoutCtx,
                           windowName,
                           true,
                           ImGuiWindowFlags_NoScrollbar,
                           &windowOpen);
    m_lastDockId = ImGui::IsWindowDocked() ? ImGui::GetWindowDockID() : 0;
    if ( ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ) {
        m_hasTimingInteractionFocus = true;
    }
    m_wasFocusedLastFrame = m_hasTimingInteractionFocus;
    if ( !windowOpen ) {
        commitAudioTimeSliderScrub();
        m_wasFocusedLastFrame             = false;
        m_hasTimingInteractionFocus       = false;
        editorSettings.showTimelineWindow = false;
        appConfig.save();
        return;
    }

    ImVec2 size = ImGui::GetContentRegionAvail();
    if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ||
         m_currentSnapshot->totalTime <= 0.0 ) {
        commitAudioTimeSliderScrub();
    }

    if ( m_currentSnapshot ) {
        // 1. 绘制垂直音频时间滚动条及时间点表格按钮
        if ( m_currentSnapshot->hasBeatmap &&
             m_currentSnapshot->totalTime > 0.0 ) {
            float time  = static_cast<float>(m_currentSnapshot->currentTime);
            float total = static_cast<float>(m_currentSnapshot->totalTime);

            float sliderWidth  = 24.0f;
            float sliderHeight = size.y;

            ImGui::BeginGroup();

            ImVec2     sliderSize(sliderWidth, sliderHeight);
            const bool sliderChanged = ::MMM::UI::FeedbackVSliderFloat(
                "##AudioTimeSlider", sliderSize, &time, 0.0f, total, "");
            const bool sliderActive = ImGui::IsItemActive();
            const bool sliderDeactivatedAfterEdit =
                ImGui::IsItemDeactivatedAfterEdit();
            if ( sliderChanged ) {
                float  visualOffset = Config::AppConfig::instance()
                                          .getVisualConfig()
                                          .getEffectiveVisualOffset();
                double targetTime   = static_cast<double>(time);
                if ( ImGui::GetIO().KeyShift ) {
                    targetTime = std::clamp(snapTimeToBeatLine(targetTime),
                                            0.0,
                                            static_cast<double>(total));
                    time       = static_cast<float>(targetTime);
                }
                const double commandTime =
                    targetTime - static_cast<double>(visualOffset);
                m_isAudioTimeSliderScrubbing = sliderActive;
                m_audioTimeSliderScrubTarget = commandTime;
                Event::EventBus::instance().publish(
                    Event::LogicCommandEvent(Logic::CmdSeek{
                        .time        = commandTime,
                        .isScrubbing = sliderActive,
                    }));
            }
            if ( sliderDeactivatedAfterEdit && m_isAudioTimeSliderScrubbing ) {
                commitAudioTimeSliderScrub();
            }

            if ( sliderActive || ImGui::IsItemHovered() ) {
                const auto timeText = MMM::UI::Utils::formatCanvasTimePair(
                    static_cast<double>(time),
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
                          framebufferScale.x,
                          framebufferScale.y);
        }

        vk::DescriptorSet texID = getDescriptorSet();
        if ( texID != VK_NULL_HANDLE ) {
            ImGui::Image((ImTextureID)(VkDescriptorSet)texID, size);

            ImVec2 canvasPos = ImGui::GetItemRectMin();
            ImVec2 mousePos  = ImGui::GetMousePos();
            bool   isHovered =
                ImGui::IsItemHovered(
                    ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
                (ImGui::IsWindowHovered(
                     ImGuiHoveredFlags_RootAndChildWindows |
                     ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                 mousePos.x >= canvasPos.x &&
                 mousePos.x <= canvasPos.x + size.x &&
                 mousePos.y >= canvasPos.y &&
                 mousePos.y <= canvasPos.y + size.y);
            const ImGuiIO& io    = ImGui::GetIO();
            float          wheel = io.MouseWheel;
            if ( isHovered && std::abs(wheel) > 0.01f ) {
                if ( !handleTimelineModifierWheel(m_name,
                                                  wheel,
                                                  io.KeyCtrl || io.KeySuper,
                                                  io.KeyAlt,
                                                  io.KeyShift,
                                                  m_speedTooltipValue,
                                                  m_speedTooltipTimer) ) {
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(
                            Logic::CmdScroll{ m_name, -wheel, io.KeyShift }));
                }
            }

            // 3. 处理 Timeline Timing 的工具交互和反馈
            bool windowFocused =
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            const bool toolbarFocusedOrHovered = isToolbarFocusedOrHovered();
            const bool timelineMouseClicked =
                isHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                              ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                              ImGui::IsMouseClicked(ImGuiMouseButton_Middle));
            const bool outsideMouseClicked =
                !isHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                               ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                               ImGui::IsMouseClicked(ImGuiMouseButton_Middle));
            if ( windowFocused || timelineMouseClicked ) {
                m_hasTimingInteractionFocus = true;
                if ( timelineMouseClicked ) {
                    ImGui::SetWindowFocus();
                    m_shouldFocusNextFrame = true;
                }
            } else if ( outsideMouseClicked && !toolbarFocusedOrHovered ) {
                m_hasTimingInteractionFocus = false;
            }
            m_wasFocusedLastFrame = m_hasTimingInteractionFocus;
            const bool hasTimingInteractionFocus =
                windowFocused || m_hasTimingInteractionFocus;
            ImVec2 gearGlyphSize = ImGui::CalcTextSize(UI::ICON_MMM_COG);
            float  iconSize      = std::ceil(
                std::max({ 20.0f, gearGlyphSize.x, gearGlyphSize.y }) + 4.0f);
            float padding = 5.0f;

            const auto& visual =
                Config::AppConfig::instance().getVisualConfig();
            float proximity   = visual.snapThreshold;
            float localMouseX = mousePos.x - canvasPos.x;
            float localMouseY = mousePos.y - canvasPos.y;
            bool  overMenuButton =
                localMouseX >= size.x - 56.0f && localMouseY <= 56.0f;
            const bool timelineHoveringForSnap = isHovered && !overMenuButton;
            const bool timelineDragging =
                ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
                ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
                ImGui::IsMouseDragging(ImGuiMouseButton_Middle);
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdSetMousePosition{
                    .cameraId       = m_name,
                    .mouseX         = localMouseX,
                    .mouseY         = localMouseY - m_lastAppliedYOffset,
                    .viewportWidth  = size.x,
                    .viewportHeight = size.y,
                    .isHovering     = timelineHoveringForSnap,
                    .isDragging     = timelineDragging }));
            bool   hoveredSnapped = false;
            double hoveredTime    = 0.0;
            if ( timelineHoveringForSnap ) {
                double rawHoveredTime = canvasTimeAtLocalY(size, localMouseY);
                hoveredTime           = snapTimingTime(
                    size, rawHoveredTime, localMouseY, hoveredSnapped);
            }

            const TimelineGearInfo gears[] = {
                { Logic::System::SCROLL_EFFECT_BPM,
                  ::MMM::TimingEffect::BPM,
                  &Logic::TimelineInteractiveElement::bpmEntity,
                  &Logic::TimelineInteractiveElement::bpmValue,
                  "BPM",
                  "BPM",
                  ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                  false },
                { Logic::System::SCROLL_EFFECT_SCROLL,
                  ::MMM::TimingEffect::SCROLL,
                  &Logic::TimelineInteractiveElement::scrollEntity,
                  &Logic::TimelineInteractiveElement::scrollValue,
                  "Scroll",
                  "Scroll",
                  ImVec4(0.2f, 1.0f, 0.2f, 1.0f),
                  true },
                { Logic::System::SCROLL_EFFECT_JUMP,
                  ::MMM::TimingEffect::JUMP,
                  &Logic::TimelineInteractiveElement::jumpEntity,
                  &Logic::TimelineInteractiveElement::jumpValue,
                  "Jump",
                  "Jump",
                  ImVec4(0.2f, 0.45f, 1.0f, 1.0f),
                  false },
                { Logic::System::SCROLL_EFFECT_HS,
                  ::MMM::TimingEffect::HS,
                  &Logic::TimelineInteractiveElement::hsEntity,
                  &Logic::TimelineInteractiveElement::hsValue,
                  "HS",
                  "HS",
                  ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                  true },
            };

            auto isNearInlineGearTime =
                [&](const Logic::TimelineInteractiveElement& el) {
                    bool isNearTime  = hoveredSnapped &&
                                       std::abs(el.time - hoveredTime) < 1e-5;
                    bool isNearPixel = std::abs(localMouseY - el.y) < proximity;
                    return isNearTime || isNearPixel;
                };

            auto countInlineGears =
                [&](const Logic::TimelineInteractiveElement& el,
                    bool                                     rightSide) {
                    int count = 0;
                    for ( const auto& gear : gears ) {
                        if ( gear.rightSide != rightSide ) continue;
                        if ( (el.effects & gear.mask) == 0 ) continue;
                        auto entity = el.*(gear.entity);
                        if ( entity == entt::null ) continue;
                        ++count;
                    }
                    return count;
                };

            auto inlineGearPos = [&](const Logic::TimelineInteractiveElement&
                                                             el,
                                     const TimelineGearInfo& gear,
                                     int                     index,
                                     int                     count) {
                if ( editorSettings.timelineProfessionalMode ) {
                    constexpr float laneCount = 4.0f;
                    const int       lane = professionalTimingLane(gear.effect);
                    const float     centerX =
                        canvasPos.x +
                        size.x * (static_cast<float>(lane) + 0.5f) / laneCount;
                    const float minX = canvasPos.x + padding;
                    const float maxX = std::max(
                        minX, canvasPos.x + size.x - iconSize - padding);
                    const float minY = canvasPos.y;
                    const float maxY =
                        std::max(minY, canvasPos.y + size.y - iconSize);
                    return ImVec2(
                        std::clamp(centerX - iconSize * 0.5f, minX, maxX),
                        std::clamp(
                            canvasPos.y + el.y - iconSize * 0.5f, minY, maxY));
                }

                float yOffset = 0.0f;
                if ( count > 1 ) {
                    yOffset = (static_cast<float>(index) -
                               (static_cast<float>(count) - 1.0f) * 0.5f) *
                              (iconSize + 4.0f);
                }

                float x    = gear.rightSide
                                 ? canvasPos.x + size.x - iconSize - padding
                                 : canvasPos.x + padding;
                float minY = canvasPos.y;
                float maxY = std::max(minY, canvasPos.y + size.y - iconSize);
                float y    = std::clamp(
                    canvasPos.y + el.y + yOffset - iconSize * 0.5f, minY, maxY);
                return ImVec2(x, y);
            };

            /// @brief 当前鼠标命中的 Timeline 齿轮按钮。
            struct InlineGearHit {
                /// @brief Timing 实体。
                entt::entity entity{ entt::null };

                /// @brief Timing 时间戳，单位秒。
                double time{ 0.0 };

                /// @brief Timing 参数值。
                double value{ 0.0 };

                /// @brief 编辑类型。
                const char* editType{ "" };

                /// @brief 显示标签。
                const char* label{ "" };
            };

            auto openInlineGearEditor = [&](const InlineGearHit& hit) {
                if ( hit.entity == entt::null ) return;
                XINFO("{} gear clicked at time: {}", hit.label, hit.time);
                m_editingEntity          = hit.entity;
                m_editTime               = hit.time;
                m_editValue              = hit.value;
                m_editType               = hit.editType;
                m_isPopupOpen            = true;
                m_isCreatePopupOpen      = false;
                m_isTimingDrawPreviewing = false;
                m_isTimingDragging       = false;
                m_isTimingErasing        = false;
                m_shouldFocusNextFrame   = false;
                m_timingEraseTargetEntities.clear();
                ::MMM::UI::FeedbackOpenPopup("TimelineEventEditor");
            };

            const bool inlineGearCanOpenEditor =
                !m_currentSnapshot->isPlaying &&
                Logic::EditorEngine::instance().getCurrentTool() ==
                    Logic::EditTool::Draw;
            const bool showInlineTimingEditors =
                inlineGearCanOpenEditor && isHovered && !overMenuButton &&
                m_currentSnapshot->hasBeatmap && !m_isTimingDragging &&
                !m_isTimingErasing && !m_isTimingDrawPreviewing &&
                !m_isPopupOpen && !m_isCreatePopupOpen;
            std::optional<InlineGearHit> inlineGearHit;
            bool                         inlineGearEditorOpened = false;
            if ( showInlineTimingEditors ) {
                for ( const auto& el : m_currentSnapshot->timelineElements ) {
                    if ( !isNearInlineGearTime(el) ) continue;

                    const int leftGearCount  = countInlineGears(el, false);
                    const int rightGearCount = countInlineGears(el, true);
                    int       leftGearIndex  = 0;
                    int       rightGearIndex = 0;
                    for ( const auto& gear : gears ) {
                        if ( (el.effects & gear.mask) == 0 ) continue;
                        auto entity = el.*(gear.entity);
                        if ( entity == entt::null ) continue;

                        int count =
                            gear.rightSide ? rightGearCount : leftGearCount;
                        int index =
                            gear.rightSide ? rightGearIndex++ : leftGearIndex++;
                        ImVec2 pos = inlineGearPos(el, gear, index, count);
                        if ( mousePos.x >= pos.x &&
                             mousePos.x <= pos.x + iconSize &&
                             mousePos.y >= pos.y &&
                             mousePos.y <= pos.y + iconSize ) {
                            inlineGearHit = InlineGearHit{ entity,
                                                           el.time,
                                                           el.*(gear.value),
                                                           gear.editType,
                                                           gear.label };
                            break;
                        }
                    }
                    if ( inlineGearHit ) break;
                }
            }
            if ( inlineGearCanOpenEditor && inlineGearHit &&
                 ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
                openInlineGearEditor(*inlineGearHit);
                inlineGearEditorOpened = true;
            }

            handleTimingCanvasInteraction(
                canvasPos, size, isHovered, hasTimingInteractionFocus);
            refreshTimelineInteractionDecoration(size);
            if ( editorSettings.timelineProfessionalMode ) {
                renderProfessionalTimelineOverlay(canvasPos, size);
            }
            renderTimingInteractionOverlay(canvasPos, size);

            // 4. 绘制交互层元件 (齿轮按钮)
            for ( const auto& el : m_currentSnapshot->timelineElements ) {
                if ( showInlineTimingEditors && isNearInlineGearTime(el) ) {
                    int leftGearCount  = countInlineGears(el, false);
                    int rightGearCount = countInlineGears(el, true);

                    int leftGearIndex  = 0;
                    int rightGearIndex = 0;
                    for ( const auto& gear : gears ) {
                        if ( (el.effects & gear.mask) == 0 ) continue;
                        auto entity = el.*(gear.entity);
                        if ( entity == entt::null ) continue;

                        int count =
                            gear.rightSide ? rightGearCount : leftGearCount;
                        int index =
                            gear.rightSide ? rightGearIndex++ : leftGearIndex++;
                        ImVec2 pos = inlineGearPos(el, gear, index, count);
                        ImGui::SetCursorScreenPos(pos);

                        std::string id =
                            fmt::format("{}_{}_{}",
                                        gear.label,
                                        el.time,
                                        static_cast<uint32_t>(entity));
                        const std::string buttonId =
                            "##TimelineInlineGear_" + id;
                        ImGui::SetNextItemAllowOverlap();
                        const bool gearClicked = ImGui::InvisibleButton(
                            buttonId.c_str(), ImVec2(iconSize, iconSize));
                        ::MMM::UI::FeedbackLastItem(
                            ImGui::GetID(buttonId.c_str()), gearClicked);
                        if ( inlineGearCanOpenEditor && gearClicked &&
                             !inlineGearEditorOpened ) {
                            openInlineGearEditor(
                                InlineGearHit{ entity,
                                               el.time,
                                               el.*(gear.value),
                                               gear.editType,
                                               gear.label });
                            inlineGearEditorOpened = true;
                        }

                        const bool   gearHovered  = ImGui::IsItemHovered();
                        const ImVec2 buttonMin    = ImGui::GetItemRectMin();
                        const ImVec2 buttonMax    = ImGui::GetItemRectMax();
                        ImDrawList*  drawList     = ImGui::GetWindowDrawList();
                        const float  gearRounding = iconSize * 0.25f;
                        const ImU32  gearBackground =
                            timelineGearBackgroundColor(gear.color,
                                                        gearHovered);
                        ImVec4 gearBorderColor = gear.color;
                        gearBorderColor.w      = gearHovered ? 1.0f : 0.82f;

                        // 对比色底板隔离底层同色 glow，确保齿轮轮廓始终清晰。
                        drawList->AddRectFilled(
                            buttonMin, buttonMax, gearBackground, gearRounding);
                        drawList->AddRect(
                            buttonMin,
                            buttonMax,
                            ImGui::ColorConvertFloat4ToU32(gearBorderColor),
                            gearRounding,
                            0,
                            gearHovered ? 2.0f : 1.0f);
                        drawList->AddText(
                            ImVec2(buttonMin.x +
                                       (iconSize - gearGlyphSize.x) * 0.5f,
                                   buttonMin.y +
                                       (iconSize - gearGlyphSize.y) * 0.5f),
                            ImGui::ColorConvertFloat4ToU32(gear.color),
                            UI::ICON_MMM_COG);

                        if ( gearHovered ) {
                            const auto timeText =
                                MMM::UI::Utils::formatCanvasTime(
                                    el.time, m_currentSnapshot);
                            const auto descriptor =
                                timelineTimingTooltipDescriptor(gear.effect);
                            ImGui::SetTooltip(
                                "%s Event: %s\n%.*s: %.6g%.*s",
                                gear.label,
                                timeText.c_str(),
                                static_cast<int>(descriptor.label.size()),
                                descriptor.label.data(),
                                el.*(gear.value),
                                static_cast<int>(descriptor.valueSuffix.size()),
                                descriptor.valueSuffix.data());
                        }
                    }
                }
            }
            // 绘制右上角时间点面板汉堡按钮
            ImVec2 menuBtnPos = ImVec2(canvasPos.x + size.x - 30.0f - 10.0f,
                                       canvasPos.y + 10.0f);
            ImGui::SetCursorScreenPos(menuBtnPos);

            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_Button));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonActive,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 15.0f);
            UI::Utils::pushFixedButtonStyleVars();
            if ( ::MMM::UI::FeedbackButton(UI::ICON_MMM_BARS,
                                           ImVec2(30.0f, 30.0f)) ) {
                ImGui::OpenPopup("TimelineOptionsMenu");
            }
            UI::Utils::popFixedButtonStyleVars();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            if ( ImGui::IsItemHovered() ) {
                ImGui::SetTooltip("%s", TR("ui.timeline.menu.tooltip").data());
            }

            if ( ImGui::BeginPopup("TimelineOptionsMenu") ) {
                const bool professionalMode =
                    editorSettings.timelineProfessionalMode;
                if ( ::MMM::UI::FeedbackMenuItem(
                         TR("ui.timeline.menu.professional_mode").data(),
                         nullptr,
                         professionalMode) ) {
                    editorSettings.timelineProfessionalMode = !professionalMode;
                    appConfig.save();
                }
                if ( ::MMM::UI::FeedbackMenuItem(
                         TR("ui.timeline.menu.open_timing_table").data(),
                         nullptr,
                         m_isTableWindowOpen) ) {
                    if ( !m_isTableWindowOpen ) {
                        ::MMM::UI::PlayPopupOpenFeedback();
                    }
                    m_isTableWindowOpen = true;
                }
                ImGui::EndPopup();
            }

            // 5. 渲染弹窗

            renderEventEditorPopup();
            renderEventCreationPopup();
            renderTimingPointsTableWindow();
            renderAnnotationTableWindow();
        }
    }

    if ( m_speedTooltipTimer > 0.0f ) {
        m_speedTooltipTimer -= ImGui::GetIO().DeltaTime;
        renderPlaybackSpeedTooltip(m_speedTooltipValue);
    }
}

/// @brief 绘制 Timeline 专业模式分轨覆盖层。
/// @param canvasPos 画布左上角屏幕坐标。
/// @param size 当前 Timeline 画布尺寸。
void TimelineCanvas::renderProfessionalTimelineOverlay(const ImVec2& canvasPos,
                                                       const ImVec2& size)
{
    if ( !m_currentSnapshot || size.x <= 1.0f || size.y <= 1.0f ) {
        return;
    }

    ImDrawList*  drawList = ImGui::GetWindowDrawList();
    const ImVec2 clipMax(canvasPos.x + size.x, canvasPos.y + size.y);
    drawList->PushClipRect(canvasPos, clipMax, true);

    constexpr int laneCount = 4;
    const float   laneWidth = size.x / static_cast<float>(laneCount);
    const char*   laneLabels[laneCount] = { "BPM", "Scroll", "Jump", "HS" };
    const ImU32   laneTextColor         = IM_COL32(240, 235, 225, 210);

    for ( int lane = 0; lane < laneCount; ++lane ) {
        const float laneX0 = canvasPos.x + laneWidth * lane;
        const float laneX1 =
            lane == laneCount - 1 ? clipMax.x : laneX0 + laneWidth;
        const ImVec2 textSize = ImGui::CalcTextSize(laneLabels[lane]);
        const float  labelX =
            laneX0 + std::max(4.0f, (laneX1 - laneX0 - textSize.x) * 0.5f);
        const float labelY = clipMax.y - textSize.y - 8.0f;
        drawList->PushClipRect(ImVec2(laneX0 + 3.0f, clipMax.y - 32.0f),
                               ImVec2(laneX1 - 3.0f, clipMax.y),
                               true);
        drawList->AddText(
            ImVec2(labelX, labelY), laneTextColor, laneLabels[lane]);
        drawList->PopClipRect();
    }

    drawList->PopClipRect();
}

/// @brief 请求下一帧将时间线窗口聚焦到前台。
void TimelineCanvas::requestFocus()
{
    m_shouldFocusNextFrame      = true;
    m_hasTimingInteractionFocus = true;
    m_wasFocusedLastFrame       = true;
}

/// @brief 获取时间线窗口当前所在的 ImGui Dock 节点。
/// @return 当前窗口停靠节点 ID；未停靠时返回 0。
ImGuiID TimelineCanvas::getDockId() const
{
    return m_lastDockId;
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
    return Config::AppConfig::instance().getEditorSettings().showTimelineWindow;
}

/// @brief 判断当前帧是否需要准备时间线快照。
/// @param snapshot 当前帧 UI 快照。
/// @return 需要准备时返回 true。
bool TimelineCanvas::needsParallelUiPrepare(
    const UI::UiFrameSnapshot& snapshot) const
{
    (void)snapshot;
    return m_syncBuffer && m_isOpen &&
           Config::AppConfig::instance().getEditorSettings().showTimelineWindow;
}

/// @brief 在线程池中拉取并准备时间线快照。
/// @param snapshot 当前帧 UI 快照。
void TimelineCanvas::prepareUiFrameData(const UI::UiFrameSnapshot& snapshot)
{
    (void)snapshot;
    resetTimelineInteractionDecoration();
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

    auto&         engine      = Logic::EditorEngine::instance();
    const int32_t activeIndex = engine.getActiveSessionIndex();
    const auto*   activeEntry = engine.getSessionEntry(activeIndex);
    if ( !activeEntry || activeEntry->isLogoPlaceholder ) {
        m_currentSnapshot     = nullptr;
        m_lastOffsetSnapshot  = nullptr;
        m_lastAppliedYOffset  = 0.0f;
        m_hasPreparedSnapshot = false;
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

/// @brief 清除上一帧追加到 Timeline 快照中的交互修饰。
void TimelineCanvas::resetTimelineInteractionDecoration()
{
    if ( !m_decoratedTimelineSnapshot ) {
        m_timelineColorRestore.clear();
        return;
    }

    auto* snapshot = m_decoratedTimelineSnapshot;
    for ( const auto& restore : m_timelineColorRestore ) {
        if ( restore.vertexIndex < snapshot->vertices.size() ) {
            snapshot->vertices[restore.vertexIndex].color = restore.color;
        }
    }

    if ( snapshot->vertices.size() >= m_decoratedTimelineVertexCount ) {
        snapshot->vertices.resize(m_decoratedTimelineVertexCount);
    }
    if ( snapshot->indices.size() >= m_decoratedTimelineIndexCount ) {
        snapshot->indices.resize(m_decoratedTimelineIndexCount);
    }
    if ( snapshot->cmds.size() >= m_decoratedTimelineCmdCount ) {
        snapshot->cmds.resize(m_decoratedTimelineCmdCount);
    }
    if ( snapshot->glowCmds.size() >= m_decoratedTimelineGlowCmdCount ) {
        snapshot->glowCmds.resize(m_decoratedTimelineGlowCmdCount);
    }

    m_decoratedTimelineSnapshot     = nullptr;
    m_decoratedTimelineVertexCount  = 0;
    m_decoratedTimelineIndexCount   = 0;
    m_decoratedTimelineCmdCount     = 0;
    m_decoratedTimelineGlowCmdCount = 0;
    m_timelineColorRestore.clear();
}

/// @brief 根据当前 Timeline 交互状态刷新快照半透明与发光命令。
/// @param size 当前 Timeline 画布尺寸。
void TimelineCanvas::refreshTimelineInteractionDecoration(const ImVec2& size)
{
    resetTimelineInteractionDecoration();
    if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ) {
        return;
    }

    m_decoratedTimelineSnapshot     = m_currentSnapshot;
    m_decoratedTimelineVertexCount  = m_currentSnapshot->vertices.size();
    m_decoratedTimelineIndexCount   = m_currentSnapshot->indices.size();
    m_decoratedTimelineCmdCount     = m_currentSnapshot->cmds.size();
    m_decoratedTimelineGlowCmdCount = m_currentSnapshot->glowCmds.size();

    bool                         hasDecoration = false;
    std::unordered_set<uint64_t> glowMarkers;
    std::unordered_set<uint64_t> dimMarkers;
    const bool professionalMode = Config::AppConfig::instance()
                                      .getEditorSettings()
                                      .timelineProfessionalMode;
    float      paddingX         = 30.0f;

    /// @brief Timeline marker 的绘制矩形参数。
    struct MarkerDrawRect {
        /// @brief 左侧 X 坐标。
        float x{ 0.0f };
        /// @brief 宽度。
        float w{ 0.0f };
        /// @brief 高度。
        float h{ 0.0f };
    };

    auto markerDrawRect = [&](::MMM::TimingEffect effect) {
        float noteW = std::max(1.0f, size.x - paddingX * 2.0f);
        float noteX = paddingX;
        if ( professionalMode ) {
            constexpr float laneCount = 4.0f;
            const float     laneWidth = size.x / laneCount;
            const int       lane      = professionalTimingLane(effect);
            noteW                     = std::max(1.0f, laneWidth - 2.0f);
            noteX                     = laneWidth * static_cast<float>(lane) +
                                        (laneWidth - noteW) * 0.5f;
        }

        float noteH = noteW * 0.36f;
        if ( auto uvIt = m_currentSnapshot->uvMap.find(
                 static_cast<uint32_t>(Logic::TextureID::Note));
             uvIt != m_currentSnapshot->uvMap.end() && uvIt->second.w > 0.0f ) {
            noteH = noteW * (uvIt->second.w / uvIt->second.z);
        }
        return MarkerDrawRect{ noteX, noteW, noteH };
    };

    auto appendGlowRange =
        [&](uint32_t indexOffset, uint32_t indexCount, uint64_t key) {
            if ( indexCount == 0U || !glowMarkers.insert(key).second ) {
                return;
            }

            Common::Render::CanvasDrawCmd cmd;
            cmd.indexOffset     = indexOffset;
            cmd.indexCount      = indexCount;
            cmd.vertexOffset    = 0;
            cmd.customTextureId = static_cast<uint32_t>(Logic::TextureID::Note);
            cmd.scissor         = {
                0,
                0,
                static_cast<uint32_t>(std::max(1.0f, std::ceil(size.x))),
                static_cast<uint32_t>(std::max(1.0f, std::ceil(size.y)))
            };
            m_currentSnapshot->glowCmds.push_back(cmd);
            hasDecoration = true;
        };

    auto appendPreviewMarker =
        [&](float y, ::MMM::TimingEffect effect, float alpha) {
            const MarkerDrawRect rect = markerDrawRect(effect);
            const uint32_t       previewIndexOffset =
                static_cast<uint32_t>(m_currentSnapshot->indices.size());
            Logic::System::Batcher previewBatcher(m_currentSnapshot,
                                                  &m_currentSnapshot->cmds);
            previewBatcher.setTexture(Logic::TextureID::Note);
            previewBatcher.pushFilledQuad(
                rect.x,
                y + rect.h * 0.5f,
                rect.w,
                rect.h,
                { 1.0f, 1.0f },
                Config::AppConfig::instance().getVisualConfig().noteFillMode,
                timelineEffectColor(effect, alpha));
            previewBatcher.flush();

            const uint32_t previewIndexCount =
                static_cast<uint32_t>(m_currentSnapshot->indices.size()) -
                previewIndexOffset;
            const uint64_t previewKey =
                timelineMarkerKey(previewIndexOffset, previewIndexCount);
            appendGlowRange(previewIndexOffset, previewIndexCount, previewKey);
        };

    auto transformMarkerColor =
        [&](uint32_t                                     vertexOffset,
            uint32_t                                     vertexCount,
            float                                        multiplier,
            const std::optional<Graphic::Vertex::Color>& overrideColor) {
            const uint32_t endVertex = vertexOffset + vertexCount;
            for ( uint32_t i = vertexOffset;
                  i < endVertex && i < m_currentSnapshot->vertices.size();
                  ++i ) {
                m_timelineColorRestore.push_back(
                    { i, m_currentSnapshot->vertices[i].color });
                if ( overrideColor ) {
                    m_currentSnapshot->vertices[i].color = *overrideColor;
                }
                m_currentSnapshot->vertices[i].color.a *= multiplier;
                hasDecoration = true;
            }
        };

    auto appendGlow = [&](const TimelineHitTarget& target) {
        if ( !target.hasMarkerGeometry || target.markerIndexCount == 0U ) {
            return;
        }
        const uint64_t key = timelineMarkerKey(target.markerIndexOffset,
                                               target.markerIndexCount);
        appendGlowRange(target.markerIndexOffset, target.markerIndexCount, key);
    };

    for ( const auto& target : collectVisibleTimingTargets() ) {
        const bool selected = m_selectedTimingEntities.find(target.entity) !=
                              m_selectedTimingEntities.end();
        const bool hovered  = target.entity == m_hoveredTimingEntity;
        const bool erasing  = m_timingEraseTargetEntities.find(target.entity) !=
                              m_timingEraseTargetEntities.end();
        const bool dragging = m_isTimingDragging && selected;
        const bool popupEditing =
            m_isPopupOpen && target.entity == m_editingEntity;
        const bool editing = dragging || popupEditing || erasing;

        if ( hovered || (selected && !dragging) || popupEditing || erasing ) {
            appendGlow(target);
        }
        if ( editing && target.hasMarkerGeometry ) {
            const uint64_t key = timelineMarkerKey(target.markerIndexOffset,
                                                   target.markerIndexCount);
            if ( dimMarkers.insert(key).second ) {
                std::optional<Graphic::Vertex::Color> overrideColor;
                if ( erasing ) {
                    overrideColor =
                        Graphic::Vertex::Color{ 1.0f, 0.2f, 0.2f, 1.0f };
                }
                transformMarkerColor(target.markerVertexOffset,
                                     target.markerVertexCount,
                                     0.5f,
                                     overrideColor);
            }
        }
        if ( dragging ) {
            const double previewTime =
                std::max(0.0, target.time + m_timingDragPreviewDelta);
            const float previewY =
                static_cast<float>(canvasYAtTime(size, previewTime));
            const MarkerDrawRect rect = markerDrawRect(target.effect);
            if ( previewY >= -rect.h && previewY <= size.y + rect.h ) {
                appendPreviewMarker(previewY, target.effect, 0.42f);
            }
        }
    }

    if ( m_isTimingDrawPreviewing && size.x > 1.0f && size.y > 1.0f ) {
        const auto  previewEffect = timelineEffectFromCreateType(m_createType);
        const float previewY =
            static_cast<float>(canvasYAtTime(size, m_timingDrawPreviewTime));
        m_timingDrawPreviewY = previewY;
        appendPreviewMarker(previewY, previewEffect, 0.42f);
    }

    if ( !hasDecoration ) {
        m_decoratedTimelineSnapshot = nullptr;
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

    auto canvas_config =
        Config::SkinManager::instance().getCanvasConfig("Basic2DCanvas");
    auto it = canvas_config.canvas_shader_modules.find(shader_name);
    if ( it != canvas_config.canvas_shader_modules.end() ) {
        auto            path = it->second;
        std::error_code shaderPathError;
        if ( !std::filesystem::exists(path, shaderPathError) ||
             shaderPathError ) {
            XWARN("Timeline shader module {} not defined.", shader_name);
            return {};
        }

        std::string vertexShaderSource = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "VertexShader.spv"));
        std::string fragmentShaderSource = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "FragmentShader.spv"));

        if ( auto geometryShaderPath = path / "GeometryShader.spv";
             std::filesystem::exists(geometryShaderPath, shaderPathError) &&
             !shaderPathError ) {
            m_shaderSourceCache[shader_name] = { vertexShaderSource,
                                                 Graphic::VKShader::readFile(
                                                     Config::pathToUtf8(
                                                         geometryShaderPath)),
                                                 fragmentShaderSource };
        } else {
            m_shaderSourceCache[shader_name] = { vertexShaderSource,
                                                 fragmentShaderSource };
        }
        return m_shaderSourceCache[shader_name];
    }
    return {};
}

std::string TimelineCanvas::getShaderName(const std::string& shader_module_name)
{
    return m_name + ":" + shader_module_name;
}

/// @brief 清空缓存的 shader 源码。
/// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
void TimelineCanvas::invalidateShaderSourceCache()
{
    m_shaderSourceCache.clear();
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

    vk::DescriptorSet             lastBound = VK_NULL_HANDLE;
    Common::Render::CanvasScissor lastScissor;

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
            vk::Rect2D physicalScissor = getPhysicalScissor(
                vk::Rect2D{ { cmd.scissor.x, cmd.scissor.y },
                            { cmd.scissor.width, cmd.scissor.height } });
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

/// @brief 录制 Timeline Timing 发光层离屏绘制命令。
/// @warning 渲染热路径：每帧离屏命令录制时执行；只遍历 glow 命令列表。
void TimelineCanvas::onRecordGlowCmds(vk::CommandBuffer&      cmdBuf,
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

    vk::DescriptorSet             lastBound = VK_NULL_HANDLE;
    Common::Render::CanvasScissor lastScissor;

    for ( const auto& cmd : m_currentSnapshot->glowCmds ) {
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
            vk::Rect2D physicalScissor = getPhysicalScissor(
                vk::Rect2D{ { cmd.scissor.x, cmd.scissor.y },
                            { cmd.scissor.width, cmd.scissor.height } });
            cmdBuf.setScissor(0, 1, &physicalScissor);
            lastScissor = cmd.scissor;
        }

        cmdBuf.drawIndexed(
            cmd.indexCount, 1, cmd.indexOffset, cmd.vertexOffset, 0);
    }
}

/// @brief 判断当前 Timeline 快照是否包含发光绘制命令。
/// @return 当前快照存在发光命令时返回 true。
/// @warning 渲染热路径：每帧离屏命令录制前执行，只读取命令数量。
bool TimelineCanvas::hasGlowDrawCmds() const
{
    return m_currentSnapshot && !m_currentSnapshot->glowCmds.empty();
}

}  // namespace MMM::Canvas
