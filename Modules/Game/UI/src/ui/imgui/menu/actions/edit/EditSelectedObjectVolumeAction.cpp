#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/UIWidgetUtils.h"

#include <imgui.h>

#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 当前选中物件中可批量编辑的音量摘要。
struct SelectedVolumeState {
    /// @brief 至少包含一个音频绑定的选中物件数量。
    std::size_t objectCount{ 0U };

    /// @brief 全部音频绑定一致时的共同音量。
    std::optional<float> commonVolume;

    /// @brief 是否存在两个不同的音量值或非法旧值。
    bool mixed{ false };
};

/// @brief 读取当前选中索引中的可编辑音量摘要。
/// @return 当前活动会话不存在时返回空摘要。
/// @warning UI 低频路径：只在打开或显示编辑窗口时遍历已选实体索引；
/// `shared_ptr` 用于保护会话切换期间的生命周期，现有接口没有稳定观察句柄。
SelectedVolumeState inspectSelectedVolumeState()
{
    SelectedVolumeState state;
    auto&               engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    const auto                            session = engine.getActiveSession();
    if ( !session ) return state;

    const auto& context          = session->getContext();
    const auto  accumulateVolume = [&state](float volume) {
        if ( !std::isfinite(volume) ) {
            state.mixed = true;
            return;
        }
        if ( !state.commonVolume ) {
            state.commonVolume = volume;
            return;
        }
        if ( std::abs(*state.commonVolume - volume) > 1e-6F ) {
            state.mixed = true;
        }
    };

    for ( const auto entity : context.selectedNoteEntities ) {
        if ( !context.noteRegistry.valid(entity) ||
             !context.noteRegistry.all_of<Logic::NoteComponent>(entity) ) {
            continue;
        }
        const auto& note =
            context.noteRegistry.get<const Logic::NoteComponent>(entity);
        if ( note.m_isSubNote || !Logic::SessionUtils::isNoteEditable(
                                     note, context.lastConfig.settings) ) {
            continue;
        }

        bool hasAudio = false;
        if ( note.m_sampleBinding ) {
            accumulateVolume(note.m_sampleBinding->m_volume);
            hasAudio = true;
        }
        for ( const auto& subNote : note.m_subNotes ) {
            if ( !subNote.sampleBinding ) continue;
            accumulateVolume(subNote.sampleBinding->m_volume);
            hasAudio = true;
        }
        if ( hasAudio ) ++state.objectCount;
    }

    for ( const auto entity : context.selectedSampleEntities ) {
        if ( !context.sampleRegistry.valid(entity) ||
             !context.sampleRegistry.all_of<Logic::SampleComponent>(entity) ) {
            continue;
        }
        const auto& sample =
            context.sampleRegistry.get<const Logic::SampleComponent>(entity);
        accumulateVolume(sample.m_volume);
        ++state.objectCount;
    }

    return state;
}

/// @brief 打开选中物件批量音量编辑器动作。
class EditSelectedObjectVolumeAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 获取用户配置的批量音量编辑快捷键提示。
    const char* shortcut(const MainMenuContext& context,
                         const char*            fallbackShortcut) const override
    {
        (void)context;
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        m_shortcutBuffer =
            ShortcutUtils::formatShortcut(shortcutConfig.editSelectedVolume);
        return m_shortcutBuffer.empty() ? fallbackShortcut
                                        : m_shortcutBuffer.c_str();
    }

    /// @brief 打开编辑窗口并用共同音量或 1.0 初始化输入。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        const auto state = inspectSelectedVolumeState();

        m_volume     = state.mixed ? 1.0F : state.commonVolume.value_or(1.0F);
        m_mixedInput = state.mixed;
        if ( !m_showWindow ) {
            ::MMM::UI::PlayPopupOpenFeedback();
        }
        m_showWindow = true;
    }

    /// @brief 消费用户配置的批量音量编辑快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取快捷键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        if ( ShortcutUtils::isShortcutPressed(
                 shortcutConfig.editSelectedVolume) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }

    /// @brief 渲染选中物件批量音量编辑窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：仅在窗口打开时检查已选索引并绘制常量数量控件。
    void renderDeferred(MainMenuContext& context) override
    {
        if ( !m_showWindow ) return;

        const auto state = inspectSelectedVolumeState();
        ImGui::SetNextWindowSize(ImVec2(420.0F * context.dpiScale, 0.0F),
                                 ImGuiCond_FirstUseEver);
        if ( ImGui::Begin(TR("ui.edit.selected_volume.title").data(),
                          &m_showWindow,
                          ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoCollapse) ) {
            if ( state.objectCount == 0U ) {
                ImGui::TextUnformatted(
                    TR("ui.edit.selected_volume.none").data());
            } else {
                const auto countText =
                    TR_FMT("ui.edit.selected_volume.count", state.objectCount);
                ImGui::TextUnformatted(countText.c_str());
            }
            if ( m_mixedInput ) {
                ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.25F, 1.0F),
                                   "%s",
                                   TR("ui.edit.selected_volume.mixed").data());
            }

            ImGui::Spacing();
            ImGui::SetNextItemWidth(220.0F * context.dpiScale);
            ImGui::InputFloat(TR("ui.edit.sample_properties.volume").data(),
                              &m_volume,
                              0.05F,
                              0.25F,
                              "%.3f");

            const bool validVolume =
                std::isfinite(m_volume) && m_volume >= 0.0F;
            if ( !validVolume ) {
                ImGui::TextColored(
                    ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                    "%s",
                    TR("ui.edit.sample_properties.invalid_volume").data());
            }

            ImGui::Spacing();
            ImGui::BeginDisabled(state.objectCount == 0U || !validVolume);
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.apply").data()) ) {
                MenuUtil::dispatchCommand(
                    Logic::CmdUpdateSelectedObjectSampleVolume{ m_volume });
                m_showWindow = false;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data()) ) {
                m_showWindow = false;
            }
        }
        ImGui::End();
    }

private:
    /// @brief 是否显示批量音量编辑窗口。
    bool m_showWindow{ false };

    /// @brief 打开窗口时选择是否包含多个不同音量。
    bool m_mixedInput{ false };

    /// @brief 待写入全部受支持选中物件的音量倍率。
    float m_volume{ 1.0F };

    /// @brief 当前帧快捷键显示缓存。
    mutable std::string m_shortcutBuffer;
};
}  // namespace

/// @brief 创建打开选中物件批量音量编辑器动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createEditSelectedObjectVolumeAction()
{
    return std::make_unique<EditSelectedObjectVolumeAction>();
}

}  // namespace MMM::UI
