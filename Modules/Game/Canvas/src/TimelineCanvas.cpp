#include "canvas/TimelineCanvas.h"
#include "canvas/TimeFormatUtils.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKShader.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/system/render/Batcher.h"
#include "ui/Icons.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string_view>
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

/// @brief Timeline 画布齿轮按钮的类型信息
struct TimelineGearInfo {
    /// @brief 对应 TimelineInteractiveElement 的效果掩码。
    uint32_t mask;

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

void TimelineCanvas::update(UI::UIManager* sourceManager)
{
    auto& appConfig      = Config::AppConfig::instance();
    auto& editorSettings = appConfig.getEditorSettings();
    if ( !editorSettings.showTimelineWindow ) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float                dpiScale = viewport->DpiScale;

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
    if ( ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ) {
        m_hasTimingInteractionFocus = true;
    }
    m_wasFocusedLastFrame = m_hasTimingInteractionFocus;
    if ( !windowOpen ) {
        m_wasFocusedLastFrame             = false;
        m_hasTimingInteractionFocus       = false;
        editorSettings.showTimelineWindow = false;
        appConfig.save();
        return;
    }

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

            // 3. 处理 Timeline Timing 的工具交互和反馈
            ImVec2 canvasPos = ImGui::GetItemRectMin();
            ImVec2 mousePos  = ImGui::GetMousePos();
            bool   windowFocused =
                ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
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
            bool   hoveredSnapped = false;
            double hoveredTime    = 0.0;
            if ( isHovered && !overMenuButton ) {
                double rawHoveredTime = canvasTimeAtLocalY(size, localMouseY);
                hoveredTime           = snapTimingTime(
                    size, rawHoveredTime, localMouseY, hoveredSnapped);
            }

            const TimelineGearInfo gears[] = {
                { Logic::System::SCROLL_EFFECT_BPM,
                  &Logic::TimelineInteractiveElement::bpmEntity,
                  &Logic::TimelineInteractiveElement::bpmValue,
                  "BPM",
                  "BPM",
                  ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                  false },
                { Logic::System::SCROLL_EFFECT_SCROLL,
                  &Logic::TimelineInteractiveElement::scrollEntity,
                  &Logic::TimelineInteractiveElement::scrollValue,
                  "Scroll",
                  "Scroll",
                  ImVec4(0.2f, 1.0f, 0.2f, 1.0f),
                  true },
                { Logic::System::SCROLL_EFFECT_JUMP,
                  &Logic::TimelineInteractiveElement::jumpEntity,
                  &Logic::TimelineInteractiveElement::jumpValue,
                  "Jump",
                  "Jump",
                  ImVec4(0.2f, 0.45f, 1.0f, 1.0f),
                  false },
                { Logic::System::SCROLL_EFFECT_HS,
                  &Logic::TimelineInteractiveElement::hsEntity,
                  &Logic::TimelineInteractiveElement::hsValue,
                  "HS",
                  "HS",
                  ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                  true },
            };

            auto isNearInlineGearTime =
                [&](const Logic::TimelineInteractiveElement& el) {
                    bool isNearTime = hoveredSnapped &&
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

            auto inlineGearPos =
                [&](const Logic::TimelineInteractiveElement& el,
                    const TimelineGearInfo&                  gear,
                    int                                      index,
                    int                                      count) {
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
                    float maxY =
                        std::max(minY, canvasPos.y + size.y - iconSize);
                    float y = std::clamp(
                        canvasPos.y + el.y + yOffset - iconSize * 0.5f,
                        minY,
                        maxY);
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
                m_timingEraseTargetEntities.clear();
                ImGui::OpenPopup("TimelineEventEditor");
            };

            const bool showInlineTimingEditors =
                isHovered && !overMenuButton && m_currentSnapshot->hasBeatmap &&
                !m_isTimingDragging && !m_isTimingErasing &&
                !m_isTimingDrawPreviewing && !m_isPopupOpen &&
                !m_isCreatePopupOpen;
            std::optional<InlineGearHit> inlineGearHit;
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
            if ( inlineGearHit &&
                 ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
                openInlineGearEditor(*inlineGearHit);
            }

            handleTimingCanvasInteraction(canvasPos,
                                          size,
                                          isHovered && !inlineGearHit,
                                          hasTimingInteractionFocus);
            renderTimingInteractionOverlay(canvasPos, size);
            refreshTimelineInteractionDecoration(size);

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
                        ImGui::SetNextItemAllowOverlap();
                        if ( ImGui::InvisibleButton(
                                 ("##TimelineInlineGear_" + id).c_str(),
                                 ImVec2(iconSize, iconSize)) ) {
                            openInlineGearEditor(
                                InlineGearHit{ entity,
                                               el.time,
                                               el.*(gear.value),
                                               gear.editType,
                                               gear.label });
                        }

                        const ImVec2 buttonMin = ImGui::GetItemRectMin();
                        ImGui::GetWindowDrawList()->AddText(
                            ImVec2(buttonMin.x +
                                       (iconSize - gearGlyphSize.x) * 0.5f,
                                   buttonMin.y +
                                       (iconSize - gearGlyphSize.y) * 0.5f),
                            ImGui::ColorConvertFloat4ToU32(gear.color),
                            UI::ICON_MMM_COG);

                        if ( ImGui::IsItemHovered() ) {
                            const auto timeText =
                                formatCanvasTime(el.time, m_currentSnapshot);
                            ImGui::SetTooltip(
                                "%s Event: %s", gear.label, timeText.c_str());
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

/// @brief 请求下一帧将时间线窗口聚焦到前台。
void TimelineCanvas::requestFocus()
{
    m_shouldFocusNextFrame      = true;
    m_hasTimingInteractionFocus = true;
    m_wasFocusedLastFrame       = true;
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
    float                        paddingX = 30.0f;
    float noteW = std::max(1.0f, size.x - paddingX * 2.0f);
    float noteH = noteW * 0.36f;
    if ( auto uvIt = m_currentSnapshot->uvMap.find(
             static_cast<uint32_t>(Logic::TextureID::Note));
         uvIt != m_currentSnapshot->uvMap.end() && uvIt->second.w > 0.0f ) {
        noteH = noteW * (uvIt->second.w / uvIt->second.z);
    }

    auto appendGlowRange =
        [&](uint32_t indexOffset, uint32_t indexCount, uint64_t key) {
            if ( indexCount == 0U || !glowMarkers.insert(key).second ) {
                return;
            }

            UI::BrushDrawCmd cmd;
            cmd.indexOffset     = indexOffset;
            cmd.indexCount      = indexCount;
            cmd.vertexOffset    = 0;
            cmd.customTextureId = static_cast<uint32_t>(Logic::TextureID::Note);
            cmd.scissor         = vk::Rect2D{
                        { 0, 0 },
                        { static_cast<uint32_t>(std::max(1.0f, std::ceil(size.x))),
                          static_cast<uint32_t>(std::max(1.0f, std::ceil(size.y))) }
            };
            m_currentSnapshot->glowCmds.push_back(cmd);
            hasDecoration = true;
        };

    auto appendPreviewMarker =
        [&](float y, ::MMM::TimingEffect effect, float alpha) {
            const uint32_t previewIndexOffset =
                static_cast<uint32_t>(m_currentSnapshot->indices.size());
            Logic::System::Batcher previewBatcher(m_currentSnapshot,
                                                  &m_currentSnapshot->cmds);
            previewBatcher.setTexture(Logic::TextureID::Note);
            previewBatcher.pushFilledQuad(
                paddingX,
                y + noteH * 0.5f,
                noteW,
                noteH,
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
        const bool hovered = target.entity == m_hoveredTimingEntity;
        const bool erasing = m_timingEraseTargetEntities.find(target.entity) !=
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
            if ( previewY >= -noteH && previewY <= size.y + noteH ) {
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
        auto path = it->second;
        if ( !std::filesystem::exists(path) ) {
            XWARN("Timeline shader module {} not defined.", shader_name);
            return {};
        }

        std::string vertexShaderSource = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "VertexShader.spv"));
        std::string fragmentShaderSource = Graphic::VKShader::readFile(
            Config::pathToUtf8(path / "FragmentShader.spv"));

        if ( auto geometryShaderPath = path / "GeometryShader.spv";
             std::filesystem::exists(geometryShaderPath) ) {
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

    vk::DescriptorSet lastBound = VK_NULL_HANDLE;
    vk::Rect2D        lastScissor;

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
            vk::Rect2D physicalScissor = getPhysicalScissor(cmd.scissor);
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
