#include "canvas/TimeFormatUtils.h"
#include "canvas/TimelineCanvas.h"
#include "config/AppConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace MMM::Canvas
{
namespace
{
/// @brief 新建 Timing 行的高亮持续时间（秒）
constexpr double NEW_TIMING_HIGHLIGHT_DURATION = 3.0;

/// @brief 从交互元素中提取主 Timing 类型
::MMM::TimingEffect getElementEffect(
    const Logic::TimelineInteractiveElement& el)
{
    if ( el.effects & Logic::System::SCROLL_EFFECT_BPM ) {
        return ::MMM::TimingEffect::BPM;
    }
    if ( el.effects & Logic::System::SCROLL_EFFECT_JUMP ) {
        return ::MMM::TimingEffect::JUMP;
    }
    if ( el.effects & Logic::System::SCROLL_EFFECT_HS ) {
        return ::MMM::TimingEffect::HS;
    }
    return ::MMM::TimingEffect::SCROLL;
}

/// @brief 获取 Timing 类型对应实体
entt::entity getElementEntity(const Logic::TimelineInteractiveElement& el)
{
    switch ( getElementEffect(el) ) {
    case ::MMM::TimingEffect::BPM: return el.bpmEntity;
    case ::MMM::TimingEffect::JUMP: return el.jumpEntity;
    case ::MMM::TimingEffect::HS: return el.hsEntity;
    case ::MMM::TimingEffect::SCROLL: return el.scrollEntity;
    }
    return entt::null;
}

/// @brief 获取 Timing 类型对应原始值
double getElementRawValue(const Logic::TimelineInteractiveElement& el)
{
    switch ( getElementEffect(el) ) {
    case ::MMM::TimingEffect::BPM: return el.bpmValue;
    case ::MMM::TimingEffect::JUMP: return el.jumpValue;
    case ::MMM::TimingEffect::HS: return el.hsValue;
    case ::MMM::TimingEffect::SCROLL: return el.scrollValue;
    }
    return 0.0;
}

/// @brief 获取 Timeline UI 中展示用的类型文本
const char* getEffectLabel(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return "BPM";
    case ::MMM::TimingEffect::SCROLL: return "流速 (SV)";
    case ::MMM::TimingEffect::JUMP: return "Jump";
    case ::MMM::TimingEffect::HS: return "HS";
    }
    return "Timing";
}

/// @brief 获取 Timeline UI 中展示用的类型颜色
ImVec4 getEffectColor(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
    case ::MMM::TimingEffect::SCROLL: return ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
    case ::MMM::TimingEffect::JUMP: return ImVec4(0.35f, 0.6f, 1.0f, 1.0f);
    case ::MMM::TimingEffect::HS: return ImVec4(1.0f, 0.88f, 0.25f, 1.0f);
    }
    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

/// @brief 从当前 Session 收集完整 Timing 列表，供表格窗口编辑使用。
std::vector<Logic::TimelineInteractiveElement> collectTimelineElements()
{
    std::vector<Logic::TimelineInteractiveElement> elements;
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session ) return elements;

    auto& registry = session->getContext().timelineRegistry;
    auto  view     = registry.view<const Logic::TimelineComponent>();
    elements.reserve(view.size());

    for ( auto entity : view ) {
        const auto& tc = view.get<const Logic::TimelineComponent>(entity);
        Logic::TimelineInteractiveElement el;
        el.time = tc.m_timestamp;
        el.y    = 0.0f;

        if ( tc.m_effect == ::MMM::TimingEffect::BPM ) {
            el.effects   = Logic::System::SCROLL_EFFECT_BPM;
            el.bpmEntity = entity;
            el.bpmValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::SCROLL ) {
            el.effects      = Logic::System::SCROLL_EFFECT_SCROLL;
            el.scrollEntity = entity;
            el.scrollValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::JUMP ) {
            el.effects    = Logic::System::SCROLL_EFFECT_JUMP;
            el.jumpEntity = entity;
            el.jumpValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::HS ) {
            el.effects  = Logic::System::SCROLL_EFFECT_HS;
            el.hsEntity = entity;
            el.hsValue  = tc.m_value;
        }

        elements.push_back(el);
    }

    std::stable_sort(
        elements.begin(), elements.end(), [](const auto& a, const auto& b) {
            if ( std::abs(a.time - b.time) > 1e-6 ) return a.time < b.time;
            return a.effects < b.effects;
        });
    return elements;
}

/// @brief 将存储值转换成编辑器显示值
/// @warning UI 热路径：表格绘制时逐行调用，只做常量时间数值归一化。
double getDisplayValue(::MMM::TimingEffect, double rawValue,
                       entt::entity = entt::null)
{
    return rawValue;
}

/// @brief 将编辑器显示值转换成存储值
/// @warning UI 热路径：用户提交编辑值时调用，不应访问文件系统或执行重型同步。
double getStoredValue(::MMM::TimingEffect, double displayValue,
                      entt::entity = entt::null)
{
    return displayValue;
}

/// @brief 从创建弹窗索引获取 Timing 类型
::MMM::TimingEffect getCreateEffect(int createType)
{
    switch ( createType ) {
    case 0: return ::MMM::TimingEffect::BPM;
    case 2: return ::MMM::TimingEffect::JUMP;
    case 3: return ::MMM::TimingEffect::HS;
    case 1:
    default: return ::MMM::TimingEffect::SCROLL;
    }
}

/// @brief 获取创建弹窗默认参数
double getDefaultCreateValue(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return 120.0;
    case ::MMM::TimingEffect::SCROLL: return 1.0;
    case ::MMM::TimingEffect::JUMP: return 1000.0;
    case ::MMM::TimingEffect::HS: return 1.0;
    }
    return 1.0;
}

/// @brief 获取用于“保持画布速度”计算的基准 BPM。
double getKeepSpeedReferenceBpm()
{
    double refBpm = 120.0;
    if ( auto session = Logic::EditorEngine::instance().getActiveSession() ) {
        if ( auto beatmap = session->getContext().currentBeatmap ) {
            if ( beatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
                refBpm = beatmap->m_baseMapMetadata.preference_bpm;
            }
        }
    }
    return refBpm;
}

/// @brief 根据新 BPM 计算保持画布下落速度所需的流速存储值。
double getKeepSpeedScrollValue(double bpm)
{
    double refBpm      = getKeepSpeedReferenceBpm();
    double safeBpm     = bpm > 1e-6 ? bpm : refBpm;
    double scrollSpeed = refBpm / safeBpm;
    return scrollSpeed > 1e-6 ? scrollSpeed : 1.0;
}

/// @brief 创建与新 BPM 同时间点的保持速度流速事件。
void createKeepSpeedScrollEvent(double time, double bpm)
{
    double finalScrollValue = getKeepSpeedScrollValue(bpm);
    Event::EventBus::instance().publish(
        Event::LogicCommandEvent(Logic::CmdCreateTimelineEvent{
            time, ::MMM::TimingEffect::SCROLL, finalScrollValue }));
}

/// @brief 绘制按偏好格式显示、仍可编辑原始秒值的时间输入控件。
bool drawTimeEditor(const char* id, double& value,
                    const Logic::RenderSnapshot* snapshot)
{
    auto preference =
        Config::AppConfig::instance().getEditorSettings().timeFormatPreference;
    if ( preference == Config::TimeFormatPreference::Seconds ) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputDouble(id, &value, 0.001, 0.01, "%.3f");
        return ImGui::IsItemDeactivatedAfterEdit();
    }

    std::string label = formatCanvasTime(value, snapshot) + "##" + id;
    if ( ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f)) ) {
        ImGui::OpenPopup(id);
    }
    if ( ImGui::IsItemHovered() ) {
        const auto timeText = formatCanvasTime(value, snapshot);
        ImGui::SetTooltip("%s", timeText.c_str());
    }

    bool changed = false;
    if ( ImGui::BeginPopup(id) ) {
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputDouble("##Seconds", &value, 0.001, 0.01, "%.3f");
        changed = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::EndPopup();
    }
    return changed;
}

/// @brief 绘制占满弹窗内容区宽度的双精度输入框。
/// @warning UI 热路径：仅写入 ImGui 下一控件宽度并绘制输入框。
bool drawFullWidthInputDouble(const char* id, double& value, double step,
                              double stepFast, const char* format)
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputDouble(id, &value, step, stepFast, format);
    return ImGui::IsItemDeactivatedAfterEdit();
}

/// @brief 计算横向按钮行的等宽按钮宽度。
/// @warning UI 热路径：只读取当前内容区宽度和样式间距。
float calcButtonRowWidth(int buttonCount, float minWidth)
{
    if ( buttonCount <= 0 ) return minWidth;
    const float spacing     = ImGui::GetStyle().ItemSpacing.x;
    const float usableWidth = ImGui::GetContentRegionAvail().x -
                              spacing * static_cast<float>(buttonCount - 1);
    return std::max(minWidth, usableWidth / static_cast<float>(buttonCount));
}
}  // namespace

/// @brief 开始跟踪一次“保持画布速度”创建出的 BPM/Scroll 联动。
void TimelineCanvas::beginKeepSpeedBinding(double time)
{
    m_keepSpeedBindingActive       = true;
    m_keepSpeedBindingTime         = time;
    m_keepSpeedBindingBpmEntity    = entt::null;
    m_keepSpeedBindingScrollEntity = entt::null;
    m_keepSpeedBindingFocusBpm     = true;
}

/// @brief 刷新当前“保持画布速度”联动关联的实体。
void TimelineCanvas::refreshKeepSpeedBinding(
    const std::vector<Logic::TimelineInteractiveElement>& elements)
{
    if ( !m_keepSpeedBindingActive ) return;

    auto chooseNewest = [](entt::entity current,
                           entt::entity candidate) -> entt::entity {
        if ( candidate == entt::null ) return current;
        if ( current == entt::null ) return candidate;
        return entt::to_integral(candidate) > entt::to_integral(current)
                   ? candidate
                   : current;
    };

    for ( const auto& el : elements ) {
        if ( std::abs(el.time - m_keepSpeedBindingTime) > 1e-6 ) continue;

        if ( el.effects & Logic::System::SCROLL_EFFECT_BPM ) {
            m_keepSpeedBindingBpmEntity =
                chooseNewest(m_keepSpeedBindingBpmEntity, el.bpmEntity);
        }
        if ( el.effects & Logic::System::SCROLL_EFFECT_SCROLL ) {
            m_keepSpeedBindingScrollEntity =
                chooseNewest(m_keepSpeedBindingScrollEntity, el.scrollEntity);
        }
    }
}

/// @brief 判断表格行是否属于当前临时联动。
bool TimelineCanvas::isKeepSpeedBindingEntity(entt::entity entity) const
{
    return m_keepSpeedBindingActive && entity != entt::null &&
           (entity == m_keepSpeedBindingBpmEntity ||
            entity == m_keepSpeedBindingScrollEntity);
}

/// @brief 使用编辑中的 BPM 值刷新联动 Scroll 值。
void TimelineCanvas::updateKeepSpeedBindingScroll(double bpm)
{
    if ( !m_keepSpeedBindingActive ||
         m_keepSpeedBindingScrollEntity == entt::null ) {
        return;
    }

    Event::EventBus::instance().publish(Event::LogicCommandEvent(
        Logic::CmdUpdateTimelineEvent{ m_keepSpeedBindingScrollEntity,
                                       m_keepSpeedBindingTime,
                                       getKeepSpeedScrollValue(bpm) }));
}

/// @brief 结束“保持画布速度”临时联动并恢复普通编辑状态。
void TimelineCanvas::finishKeepSpeedBinding()
{
    m_keepSpeedBindingActive       = false;
    m_keepSpeedBindingTime         = -1.0;
    m_keepSpeedBindingBpmEntity    = entt::null;
    m_keepSpeedBindingScrollEntity = entt::null;
    m_keepSpeedBindingFocusBpm     = false;
}

/// @brief 渲染 Timeline 时间点编辑弹窗。
/// @warning UI 热路径：弹窗打开期间每帧执行，仅进行 ImGui
/// 控件绘制和用户提交时的事件发布。
void TimelineCanvas::renderEventEditorPopup()
{
    float dpiScale   = Config::AppConfig::instance().getWindowContentScale();
    float popupWidth = std::floor(380.0f * dpiScale);

    ::MMM::UI::Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin("TimelineEventEditor",
                          &m_isPopupOpen,
                          ImGuiWindowFlags_None,
                          ImVec2(popupWidth, 0.0f)) ) {
        std::string typeTitle = m_editType;

        ImGui::Text(
            "%s", TR_FMT("ui.timeline.event_editor.title", typeTitle).c_str());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted(TR("ui.timeline.event_editor.timestamp").data());
        drawTimeEditor("##Time", m_editTime, m_currentSnapshot);

        ::MMM::TimingEffect editEffect =
            (m_editType == "BPM")    ? ::MMM::TimingEffect::BPM
            : (m_editType == "Jump") ? ::MMM::TimingEffect::JUMP
            : (m_editType == "HS")   ? ::MMM::TimingEffect::HS
                                     : ::MMM::TimingEffect::SCROLL;

        if ( editEffect == ::MMM::TimingEffect::BPM ) {
            ImGui::TextUnformatted(TR("ui.timeline.event_editor.bpm").data());
            drawFullWidthInputDouble("##Value", m_editValue, 0.1, 1.0, "%.2f");
        } else if ( editEffect == ::MMM::TimingEffect::JUMP ) {
            ImGui::TextUnformatted("Jump (ms)");
            drawFullWidthInputDouble("##Value", m_editValue, 1.0, 10.0, "%.3f");
        } else if ( editEffect == ::MMM::TimingEffect::HS ) {
            ImGui::TextUnformatted("HS");
            drawFullWidthInputDouble("##Value", m_editValue, 0.01, 0.1, "%.4f");
        } else {
            ImGui::TextUnformatted(
                TR("ui.timeline.event_editor.scroll").data());
            drawFullWidthInputDouble("##Value", m_editValue, 0.01, 0.1, "%.4f");
            ImGui::TextDisabled(
                "%s", TR("ui.timeline.event_editor.scroll_hint").data());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float actionButtonWidth =
            calcButtonRowWidth(3, std::floor(80.0f * dpiScale));
        if ( ImGui::Button(TR("ui.timeline.event_editor.apply").data(),
                           ImVec2(actionButtonWidth, 0)) ) {
            double finalValue =
                getStoredValue(editEffect, m_editValue, m_editingEntity);

            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdUpdateTimelineEvent{
                    m_editingEntity, m_editTime, finalValue }));
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.timeline.event_editor.delete").data(),
                           ImVec2(actionButtonWidth, 0)) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdDeleteTimelineEvent{ m_editingEntity }));
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.timeline.event_editor.cancel").data(),
                           ImVec2(actionButtonWidth, 0)) ) {
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        ImGui::EndPopup();
    }
}

/// @brief 渲染 Timeline 时间点创建弹窗。
/// @warning UI 热路径：弹窗打开期间每帧执行，仅进行 ImGui
/// 控件绘制和用户提交时的事件发布。
void TimelineCanvas::renderEventCreationPopup()
{
    float dpiScale   = Config::AppConfig::instance().getWindowContentScale();
    float popupWidth = std::floor(430.0f * dpiScale);

    ::MMM::UI::Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( modalScope.begin("TimelineCreateEvent",
                          &m_isCreatePopupOpen,
                          ImGuiWindowFlags_None,
                          ImVec2(popupWidth, 0.0f)) ) {
        ImGui::TextUnformatted(TR("ui.timeline.event_creator.title").data());
        ImGui::Separator();
        ImGui::Spacing();

        // 自动计算下一项 RadioButton 宽度并在空间充足时在同行显示的辅助函数
        auto getRadioButtonWidth = [](const char* label) -> float {
            ImGuiStyle& style      = ImGui::GetStyle();
            float       circleSize = ImGui::GetFrameHeight();
            float       textWidth  = ImGui::CalcTextSize(label).x;
            return circleSize + style.ItemSpacing.x + textWidth +
                   style.FramePadding.x * 2.0f;
        };

        auto wrapToNextLineIfNoSpace = [&](float nextItemWidth) {
            float lastX2 = ImGui::GetItemRectMax().x;
            float windowVisibleX2 =
                ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            if ( lastX2 + spacing + nextItemWidth < windowVisibleX2 ) {
                ImGui::SameLine(0.0f, spacing);
            }
        };

        ImGui::TextUnformatted(TR("ui.timeline.event_creator.pos_type").data());

        std::string posClickLabel =
            m_isTimeSnapped
                ? TR("ui.timeline.event_creator.pos_click_snapped").data()
                : TR("ui.timeline.event_creator.pos_click").data();
        std::string posCurrentLabel =
            TR("ui.timeline.event_creator.pos_current").data();

        if ( ImGui::RadioButton(posClickLabel.c_str(), &m_createPosType, 0) ) {
            m_createTimeManual =
                m_isTimeSnapped ? m_createTimeSnapped : m_createTimeRaw;
        }

        wrapToNextLineIfNoSpace(getRadioButtonWidth(posCurrentLabel.c_str()));

        if ( ImGui::RadioButton(
                 posCurrentLabel.c_str(), &m_createPosType, 1) ) {
            m_createTimeManual = m_currentSnapshot->currentTime;
        }

        ImGui::TextUnformatted(TR("ui.timeline.event_editor.timestamp").data());
        drawTimeEditor("##CreateTime", m_createTimeManual, m_currentSnapshot);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted(TR("ui.timeline.event_creator.type").data());
        if ( ImGui::RadioButton("BPM", &m_createType, 0) ) {
            m_createValue =
                getDefaultCreateValue(getCreateEffect(m_createType));
        }

        wrapToNextLineIfNoSpace(getRadioButtonWidth("Scroll"));

        if ( ImGui::RadioButton("Scroll", &m_createType, 1) ) {
            m_createValue =
                getDefaultCreateValue(getCreateEffect(m_createType));
        }

        wrapToNextLineIfNoSpace(getRadioButtonWidth("Jump"));

        if ( ImGui::RadioButton("Jump", &m_createType, 2) ) {
            m_createValue =
                getDefaultCreateValue(getCreateEffect(m_createType));
        }

        wrapToNextLineIfNoSpace(getRadioButtonWidth("HS"));

        if ( ImGui::RadioButton("HS", &m_createType, 3) ) {
            m_createValue =
                getDefaultCreateValue(getCreateEffect(m_createType));
        }

        ImGui::Spacing();
        ::MMM::TimingEffect createEffect = getCreateEffect(m_createType);
        if ( createEffect == ::MMM::TimingEffect::BPM ) {
            ImGui::TextUnformatted(TR("ui.timeline.event_editor.bpm").data());
            drawFullWidthInputDouble(
                "##BPMValue", m_createValue, 0.1, 1.0, "%.2f");
            ImGui::Spacing();
            ImGui::Checkbox(TR("ui.timeline.event_creator.keep_speed").data(),
                            &m_keepSpeedOnBpmChange);
        } else if ( createEffect == ::MMM::TimingEffect::JUMP ) {
            ImGui::TextUnformatted("Jump (ms)");
            drawFullWidthInputDouble(
                "##JumpValue", m_createValue, 1.0, 10.0, "%.3f");
        } else if ( createEffect == ::MMM::TimingEffect::HS ) {
            ImGui::TextUnformatted("HS");
            drawFullWidthInputDouble(
                "##HSValue", m_createValue, 0.01, 0.1, "%.4f");
        } else {
            ImGui::TextUnformatted(
                TR("ui.timeline.event_editor.scroll").data());
            drawFullWidthInputDouble(
                "##ScrollValue", m_createValue, 0.01, 0.1, "%.3f");
            ImGui::TextDisabled(
                "%s", TR("ui.timeline.event_editor.scroll_hint").data());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float actionButtonWidth =
            calcButtonRowWidth(2, std::floor(100.0f * dpiScale));
        if ( ImGui::Button(TR("ui.timeline.event_creator.create").data(),
                           ImVec2(actionButtonWidth, 0)) ) {
            ::MMM::TimingEffect type = getCreateEffect(m_createType);
            double finalValue        = getStoredValue(type, m_createValue);

            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdCreateTimelineEvent{
                    m_createTimeManual, type, finalValue }));
            m_lastCreatedTimingTime   = m_createTimeManual;
            m_lastCreatedTimingEffect = type;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;

            if ( type == ::MMM::TimingEffect::BPM && m_keepSpeedOnBpmChange ) {
                createKeepSpeedScrollEvent(m_createTimeManual, m_createValue);
                beginKeepSpeedBinding(m_createTimeManual);
            }

            ImGui::CloseCurrentPopup();
            m_isCreatePopupOpen = false;
        }

        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.timeline.event_editor.cancel").data(),
                           ImVec2(actionButtonWidth, 0)) ) {
            ImGui::CloseCurrentPopup();
            m_isCreatePopupOpen = false;
        }

        ImGui::EndPopup();
    }
}

/// @brief 渲染可批量编辑时间点的表格窗口（非模态）
void TimelineCanvas::renderTimingPointsTableWindow()
{
    if ( !m_isTableWindowOpen ) {
        finishKeepSpeedBinding();
        return;
    }

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
    };

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    ImGui::SetNextWindowSize(ImVec2(650, 450), ImGuiCond_FirstUseEver);

    std::string windowTitle =
        std::string(TR("ui.timeline.timing_points_table.title").data()) +
        "###TimingPointsTableWindow";
    if ( ImGui::Begin(windowTitle.c_str(), &m_isTableWindowOpen) ) {
        if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ) {
            ImGui::TextDisabled("当前未加载任何谱面");
            ImGui::End();
            ImGui::PopStyleVar(6);
            return;
        }

        auto elements = collectTimelineElements();
        refreshKeepSpeedBinding(elements);

        // 顶层工具栏
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("ui.timeline.event_creator.title").data());
        ImGui::SameLine();
        if ( ImGui::Button("添加 BPM") ) {
            constexpr double DEFAULT_BPM_VALUE = 120.0;
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdCreateTimelineEvent{ m_currentSnapshot->currentTime,
                                               ::MMM::TimingEffect::BPM,
                                               DEFAULT_BPM_VALUE }));
            m_lastCreatedTimingTime   = m_currentSnapshot->currentTime;
            m_lastCreatedTimingEffect = ::MMM::TimingEffect::BPM;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;

            if ( m_keepSpeedOnBpmChange ) {
                createKeepSpeedScrollEvent(m_currentSnapshot->currentTime,
                                           DEFAULT_BPM_VALUE);
                beginKeepSpeedBinding(m_currentSnapshot->currentTime);
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox(TR("ui.timeline.event_creator.keep_speed").data(),
                        &m_keepSpeedOnBpmChange);
        ImGui::SameLine();
        if ( ImGui::Button("添加流速 (SV)") ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdCreateTimelineEvent{ m_currentSnapshot->currentTime,
                                               ::MMM::TimingEffect::SCROLL,
                                               1.0 }));
            m_lastCreatedTimingTime   = m_currentSnapshot->currentTime;
            m_lastCreatedTimingEffect = ::MMM::TimingEffect::SCROLL;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;
        }
        ImGui::SameLine();
        if ( ImGui::Button("添加 Jump") ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdCreateTimelineEvent{ m_currentSnapshot->currentTime,
                                               ::MMM::TimingEffect::JUMP,
                                               1000.0 }));
            m_lastCreatedTimingTime   = m_currentSnapshot->currentTime;
            m_lastCreatedTimingEffect = ::MMM::TimingEffect::JUMP;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;
        }
        ImGui::SameLine();
        if ( ImGui::Button("添加 HS") ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdCreateTimelineEvent{ m_currentSnapshot->currentTime,
                                               ::MMM::TimingEffect::HS,
                                               1.0 }));
            m_lastCreatedTimingTime   = m_currentSnapshot->currentTime;
            m_lastCreatedTimingEffect = ::MMM::TimingEffect::HS;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;
        }

        ImGui::Separator();

        // 批量修改工具
        if ( ImGui::TreeNode("批量修改工具##BulkTools") ) {
            // 批量偏移
            static double bulkOffsetValue = 0.0;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("批量时间偏移 (秒):");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputDouble(
                "##BulkOffsetInput", &bulkOffsetValue, 0.001, 0.01, "%.3f");
            ImGui::SameLine();
            if ( ImGui::Button("应用时间偏移") &&
                 std::abs(bulkOffsetValue) > 1e-6 ) {
                for ( const auto& el : elements ) {
                    entt::entity ent    = getElementEntity(el);
                    double       rawVal = getElementRawValue(el);
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(Logic::CmdUpdateTimelineEvent{
                            ent, el.time + bulkOffsetValue, rawVal }));
                }
                bulkOffsetValue = 0.0;
            }

            // 批量缩放
            static double bulkScaleValue = 1.0;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("批量流速缩放倍率:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputDouble(
                "##BulkScaleInput", &bulkScaleValue, 0.01, 0.1, "%.2f");
            ImGui::SameLine();
            if ( ImGui::Button("应用流速缩放") &&
                 std::abs(bulkScaleValue - 1.0) > 1e-6 ) {
                for ( const auto& el : elements ) {
                    if ( el.effects & Logic::System::SCROLL_EFFECT_SCROLL ) {
                        double dispScroll =
                            getDisplayValue(::MMM::TimingEffect::SCROLL,
                                            el.scrollValue,
                                            el.scrollEntity);
                        double newDisp = dispScroll * bulkScaleValue;
                        double newVal =
                            getStoredValue(::MMM::TimingEffect::SCROLL,
                                           newDisp,
                                           el.scrollEntity);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    el.scrollEntity, el.time, newVal }));
                    }
                }
                bulkScaleValue = 1.0;
            }

            ImGui::TreePop();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 渲染表格
        if ( ImGui::BeginTable(
                 "TimingPointsTableMain",
                 5,
                 ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                     ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg |
                     ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                     ImGuiTableFlags_ScrollY,
                 ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing())) ) {
            ImGui::TableSetupColumn(
                "序号", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("时间戳 (秒)",
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(
                "类型", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("数值", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(
                "操作", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(elements.size()));
            while ( clipper.Step() ) {
                for ( int idx = clipper.DisplayStart; idx < clipper.DisplayEnd;
                      ++idx ) {
                    const auto&         el         = elements[idx];
                    int                 displayIdx = idx + 1;
                    ::MMM::TimingEffect effect     = getElementEffect(el);
                    entt::entity        ent        = getElementEntity(el);
                    const bool          isKeepSpeedBindingRow =
                        isKeepSpeedBindingEntity(ent);
                    bool isRecentlyCreated =
                        (ImGui::GetTime() <=
                         m_lastCreatedTimingHighlightUntil) &&
                        (effect == m_lastCreatedTimingEffect) &&
                        (std::abs(el.time - m_lastCreatedTimingTime) <= 1e-6);

                    ImGui::TableNextRow();
                    if ( isKeepSpeedBindingRow ) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                               IM_COL32(180, 225, 255, 115));
                    } else if ( isRecentlyCreated ) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                               IM_COL32(255, 245, 170, 95));
                    }

                    // Column 0: 序号
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("#%d", displayIdx);

                    // Column 1: 时间戳 (秒)
                    ImGui::TableSetColumnIndex(1);
                    double tVal = el.time;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string tId = fmt::format("##T_{}", displayIdx);
                    if ( drawTimeEditor(
                             tId.c_str(), tVal, m_currentSnapshot) ) {
                        entt::entity ent    = getElementEntity(el);
                        double       rawVal = getElementRawValue(el);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    ent, tVal, rawVal }));
                    }

                    // Column 2: 类型
                    ImGui::TableSetColumnIndex(2);
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          getEffectColor(effect));
                    ImGui::TextUnformatted(getEffectLabel(effect));
                    ImGui::PopStyleColor();

                    // Column 3: 数值
                    ImGui::TableSetColumnIndex(3);
                    double vVal =
                        getDisplayValue(effect, getElementRawValue(el), ent);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string vId = fmt::format("##V_{}", displayIdx);
                    const bool  isBoundBpm =
                        m_keepSpeedBindingActive &&
                        ent == m_keepSpeedBindingBpmEntity &&
                        effect == ::MMM::TimingEffect::BPM;
                    const bool isBoundScroll =
                        m_keepSpeedBindingActive &&
                        ent == m_keepSpeedBindingScrollEntity &&
                        effect == ::MMM::TimingEffect::SCROLL;
                    if ( isBoundBpm && m_keepSpeedBindingFocusBpm ) {
                        ImGui::SetKeyboardFocusHere();
                        m_keepSpeedBindingFocusBpm = false;
                    }
                    if ( isBoundScroll ) {
                        ImGui::BeginDisabled();
                    }
                    ImGui::InputDouble(
                        vId.c_str(),
                        &vVal,
                        effect == ::MMM::TimingEffect::BPM ? 0.1 : 0.01,
                        effect == ::MMM::TimingEffect::BPM ? 1.0 : 0.1,
                        effect == ::MMM::TimingEffect::BPM ? "%.2f" : "%.4f");
                    if ( isBoundScroll ) {
                        ImGui::EndDisabled();
                        if ( ImGui::IsItemHovered(
                                 ImGuiHoveredFlags_AllowWhenDisabled) ) {
                            ImGui::SetTooltip(
                                "保持画布速度联动中，修改 BPM 后自动刷新");
                        }
                    }
                    if ( isBoundBpm && ImGui::IsItemEdited() ) {
                        double finalValue = getStoredValue(effect, vVal, ent);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    ent, el.time, finalValue }));
                        updateKeepSpeedBindingScroll(vVal);
                    }
                    if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                        double finalValue = getStoredValue(effect, vVal, ent);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    ent, el.time, finalValue }));
                        if ( isBoundBpm ) {
                            updateKeepSpeedBindingScroll(vVal);
                            finishKeepSpeedBinding();
                        }
                    } else if ( isBoundBpm && ImGui::IsItemDeactivated() ) {
                        finishKeepSpeedBinding();
                    }

                    // Column 4: 操作
                    ImGui::TableSetColumnIndex(4);
                    std::string seekId =
                        fmt::format("跳转##Seek_{}", displayIdx);
                    if ( ImGui::Button(seekId.c_str()) ) {
                        float visualOffset = Config::AppConfig::instance()
                                                 .getVisualConfig()
                                                 .getEffectiveVisualOffset();
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdSeek{ el.time - visualOffset }));
                    }
                    ImGui::SameLine();
                    std::string delId = fmt::format("删除##Del_{}", displayIdx);
                    if ( ImGui::Button(delId.c_str()) ) {
                        entt::entity ent = getElementEntity(el);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdDeleteTimelineEvent{ ent }));
                    }
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(6);
}

}  // namespace MMM::Canvas
