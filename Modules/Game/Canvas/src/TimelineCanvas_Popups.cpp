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

/// @brief 判断当前活动谱面是否以 Malody 语义存储时间线
bool isActiveBeatmapMalody()
{
    if ( auto session = Logic::EditorEngine::instance().getActiveSession() ) {
        if ( auto beatmap = session->getContext().currentBeatmap ) {
            return beatmap->m_metadata.map_properties.contains(
                ::MMM::MapMetadataType::MALODY);
        }
    }
    return false;
}

/// @brief 判断指定时间线实体是否以 Malody 语义存储流速值
bool isMalodyTimelineEntity(entt::entity entity)
{
    if ( entity == entt::null ) {
        return isActiveBeatmapMalody();
    }

    if ( auto session = Logic::EditorEngine::instance().getActiveSession() ) {
        auto& registry = session->getContext().timelineRegistry;
        if ( registry.valid(entity) &&
             registry.all_of<Logic::TimelineComponent>(entity) ) {
            const auto& tl = registry.get<Logic::TimelineComponent>(entity);
            return tl.m_metadata.timing_properties.contains(
                ::MMM::TimingMetadataType::MALODY);
        }
    }
    return isActiveBeatmapMalody();
}

/// @brief 将存储值转换成编辑器显示值
double getDisplayValue(::MMM::TimingEffect effect, double rawValue,
                       entt::entity entity = entt::null)
{
    if ( effect == ::MMM::TimingEffect::SCROLL &&
         !isMalodyTimelineEntity(entity) && rawValue < -1e-6 ) {
        return -100.0 / rawValue;
    }
    return rawValue;
}

/// @brief 将编辑器显示值转换成存储值
double getStoredValue(::MMM::TimingEffect effect, double displayValue,
                      entt::entity entity = entt::null)
{
    if ( effect == ::MMM::TimingEffect::SCROLL &&
         !isMalodyTimelineEntity(entity) && displayValue > 1e-6 ) {
        return -100.0 / displayValue;
    }
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

/// @brief 绘制按偏好格式显示、仍可编辑原始秒值的时间输入控件。
bool drawTimeEditor(const char* id, double& value,
                    const Logic::RenderSnapshot* snapshot)
{
    auto preference =
        Config::AppConfig::instance().getEditorSettings().timeFormatPreference;
    if ( preference == Config::TimeFormatPreference::Seconds ) {
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
}  // namespace

void TimelineCanvas::renderEventEditorPopup()
{
    static bool wasOpen = false;
    bool        isOpen  = ImGui::IsPopupOpen("TimelineEventEditor");
    if ( isOpen && !wasOpen ) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
    }
    wasOpen = isOpen;

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    float windowPaddingVal =
        std::floor(editorSettings.aesthetics.windowPadding * dpiScale);

    ImGui::SetNextWindowSize(ImVec2(300 * dpiScale, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(windowPaddingVal, windowPaddingVal));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);

    if ( ImGui::BeginPopupModal(
             "TimelineEventEditor", &m_isPopupOpen, ImGuiWindowFlags_None) ) {
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
            ImGui::InputDouble("##Value", &m_editValue, 0.1, 1.0, "%.2f");
        } else if ( editEffect == ::MMM::TimingEffect::JUMP ) {
            ImGui::TextUnformatted("Jump (ms)");
            ImGui::InputDouble("##Value", &m_editValue, 1.0, 10.0, "%.3f");
        } else if ( editEffect == ::MMM::TimingEffect::HS ) {
            ImGui::TextUnformatted("HS");
            ImGui::InputDouble("##Value", &m_editValue, 0.01, 0.1, "%.4f");
        } else {
            ImGui::TextUnformatted(
                TR("ui.timeline.event_editor.scroll").data());
            ImGui::InputDouble("##Value", &m_editValue, 0.01, 0.1, "%.4f");
            ImGui::TextDisabled(
                "%s", TR("ui.timeline.event_editor.scroll_hint").data());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if ( ImGui::Button(TR("ui.timeline.event_editor.apply").data(),
                           ImVec2(80, 0)) ) {
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
                           ImVec2(80, 0)) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdDeleteTimelineEvent{ m_editingEntity }));
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.timeline.event_editor.cancel").data(),
                           ImVec2(80, 0)) ) {
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
}

void TimelineCanvas::renderEventCreationPopup()
{
    static bool wasOpen = false;
    bool        isOpen  = ImGui::IsPopupOpen("TimelineCreateEvent");
    if ( isOpen && !wasOpen ) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
    }
    wasOpen = isOpen;

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    float windowPaddingVal =
        std::floor(editorSettings.aesthetics.windowPadding * dpiScale);

    ImGui::SetNextWindowSize(ImVec2(350 * dpiScale, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(windowPaddingVal, windowPaddingVal));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);

    if ( ImGui::BeginPopupModal("TimelineCreateEvent",
                                &m_isCreatePopupOpen,
                                ImGuiWindowFlags_None) ) {
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
            ImGui::InputDouble("##BPMValue", &m_createValue, 0.1, 1.0, "%.2f");
            ImGui::Spacing();
            ImGui::Checkbox(TR("ui.timeline.event_creator.keep_speed").data(),
                            &m_keepSpeedOnBpmChange);
        } else if ( createEffect == ::MMM::TimingEffect::JUMP ) {
            ImGui::TextUnformatted("Jump (ms)");
            ImGui::InputDouble(
                "##JumpValue", &m_createValue, 1.0, 10.0, "%.3f");
        } else if ( createEffect == ::MMM::TimingEffect::HS ) {
            ImGui::TextUnformatted("HS");
            ImGui::InputDouble("##HSValue", &m_createValue, 0.01, 0.1, "%.4f");
        } else {
            ImGui::TextUnformatted(
                TR("ui.timeline.event_editor.scroll").data());
            ImGui::InputDouble(
                "##ScrollValue", &m_createValue, 0.01, 0.1, "%.3f");
            ImGui::TextDisabled(
                "%s", TR("ui.timeline.event_editor.scroll_hint").data());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if ( ImGui::Button(TR("ui.timeline.event_creator.create").data(),
                           ImVec2(100, 0)) ) {
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
                double refBpm = 120.0;
                if ( auto session =
                         Logic::EditorEngine::instance().getActiveSession() ) {
                    if ( auto beatmap = session->getContext().currentBeatmap ) {
                        if ( beatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
                            refBpm = beatmap->m_baseMapMetadata.preference_bpm;
                        }
                    }
                }
                double scrollSpeed = refBpm / m_createValue;
                double finalScrollValue =
                    isActiveBeatmapMalody()
                        ? scrollSpeed
                        : (scrollSpeed > 1e-6 ? (-100.0 / scrollSpeed)
                                              : -100.0);
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdCreateTimelineEvent{ m_createTimeManual,
                                                   ::MMM::TimingEffect::SCROLL,
                                                   finalScrollValue }));
            }

            ImGui::CloseCurrentPopup();
            m_isCreatePopupOpen = false;
        }

        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.timeline.event_editor.cancel").data(),
                           ImVec2(100, 0)) ) {
            ImGui::CloseCurrentPopup();
            m_isCreatePopupOpen = false;
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
}

/// @brief 渲染可批量编辑时间点的表格窗口（非模态）
void TimelineCanvas::renderTimingPointsTableWindow()
{
    if ( !m_isTableWindowOpen ) return;

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

    if ( ImGui::Begin(TR("ui.timeline.timing_points_table.title").data(),
                      &m_isTableWindowOpen) ) {
        if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ) {
            ImGui::TextDisabled("当前未加载任何谱面");
            ImGui::End();
            ImGui::PopStyleVar(6);
            return;
        }

        auto elements = collectTimelineElements();

        // 顶层工具栏
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("ui.timeline.event_creator.title").data());
        ImGui::SameLine();
        if ( ImGui::Button("添加 BPM") ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdCreateTimelineEvent{ m_currentSnapshot->currentTime,
                                               ::MMM::TimingEffect::BPM,
                                               120.0 }));
            m_lastCreatedTimingTime   = m_currentSnapshot->currentTime;
            m_lastCreatedTimingEffect = ::MMM::TimingEffect::BPM;
            m_lastCreatedTimingHighlightUntil =
                ImGui::GetTime() + NEW_TIMING_HIGHLIGHT_DURATION;
        }
        ImGui::SameLine();
        if ( ImGui::Button("添加流速 (SV)") ) {
            double defaultScroll = isActiveBeatmapMalody() ? 1.0 : -100.0;
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdCreateTimelineEvent{ m_currentSnapshot->currentTime,
                                               ::MMM::TimingEffect::SCROLL,
                                               defaultScroll }));
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
                    bool                isRecentlyCreated =
                        (ImGui::GetTime() <=
                         m_lastCreatedTimingHighlightUntil) &&
                        (effect == m_lastCreatedTimingEffect) &&
                        (std::abs(el.time - m_lastCreatedTimingTime) <= 1e-6);

                    ImGui::TableNextRow();
                    if ( isRecentlyCreated ) {
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
                    entt::entity ent = getElementEntity(el);
                    double       vVal =
                        getDisplayValue(effect, getElementRawValue(el), ent);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string vId = fmt::format("##V_{}", displayIdx);
                    ImGui::InputDouble(
                        vId.c_str(),
                        &vVal,
                        effect == ::MMM::TimingEffect::BPM ? 0.1 : 0.01,
                        effect == ::MMM::TimingEffect::BPM ? 1.0 : 0.1,
                        effect == ::MMM::TimingEffect::BPM ? "%.2f" : "%.4f");
                    if ( ImGui::IsItemDeactivatedAfterEdit() ) {
                        double finalValue = getStoredValue(effect, vVal, ent);
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    ent, el.time, finalValue }));
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
