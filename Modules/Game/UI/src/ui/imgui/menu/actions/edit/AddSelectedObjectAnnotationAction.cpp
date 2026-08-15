#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/CreatorIdentity.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/SampleComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/annotation/BeatmapAnnotation.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/UIWidgetUtils.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 为当前选中的单个玩家物件或自动采样添加批注。
class AddSelectedObjectAnnotationAction final
    : public IMainMenuItemActionHandler
{
public:
    /// @brief 获取用户配置的添加物件批注快捷键提示。
    const char* shortcut(const MainMenuContext& context,
                         const char*            fallbackShortcut) const override
    {
        (void)context;
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        m_shortcutBuffer =
            ShortcutUtils::formatShortcut(shortcutConfig.addSelectedAnnotation);
        return m_shortcutBuffer.empty() ? fallbackShortcut
                                        : m_shortcutBuffer.c_str();
    }

    /// @brief 有活动谱面物件选区时允许打开批注弹窗。
    /// @warning UI 热路径：菜单每帧检查；只读取会话维护的选择索引。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().hasActiveChartObjectSelection();
    }

    /// @brief 请求打开独立物件批注弹窗。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        ::MMM::UI::PlayPopupOpenFeedback();
        m_requestOpen = true;
        m_resetTarget = true;
    }

    /// @brief 消费用户配置的添加物件批注快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取快捷键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        if ( ShortcutUtils::isShortcutPressed(
                 shortcutConfig.addSelectedAnnotation) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }

    /// @brief 渲染独立的选中物件批注弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：弹窗打开时只读取选择索引和单个目标组件，
    /// 不遍历完整 ECS，不访问文件系统。
    void renderDeferred(MainMenuContext& context) override
    {
        std::string popupLabel =
            TR("ui.edit.add_selected_annotation.title").toString();
        if ( m_requestOpen ) {
            ImGui::OpenPopup(popupLabel.c_str());
            m_requestOpen = false;
        }

        ImGui::SetNextWindowSize(ImVec2(560.0F * context.dpiScale, 0.0F),
                                 ImGuiCond_Appearing);
        if ( !ImGui::BeginPopupModal(popupLabel.c_str(),
                                     nullptr,
                                     ImGuiWindowFlags_AlwaysAutoResize) ) {
            return;
        }

        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        const auto session = engine.getActiveSession();
        if ( !session ) {
            ImGui::TextUnformatted(TR("ui.tools.no_active_session").data());
            renderCancelButton();
            ImGui::EndPopup();
            return;
        }

        auto&             contextState = session->getContextMutable();
        const std::size_t selectionCount =
            contextState.selectedNoteEntities.size() +
            contextState.selectedSampleEntities.size();
        if ( selectionCount != 1U ) {
            ImGui::TextWrapped(
                "%s",
                TR("ui.edit.note_metadata.annotation_single_only").data());
            renderCancelButton();
            ImGui::EndPopup();
            return;
        }

        const bool targetsSample = !contextState.selectedSampleEntities.empty();
        const entt::entity entity =
            targetsSample ? *contextState.selectedSampleEntities.begin()
                          : *contextState.selectedNoteEntities.begin();
        const auto objectKind = targetsSample
                                    ? Logic::ChartObjectKind::AudioSample
                                    : Logic::ChartObjectKind::PlayerNote;
        if ( !isValidTarget(contextState, entity, objectKind) ) {
            ImGui::TextWrapped(
                "%s",
                TR("ui.edit.note_metadata.annotation_single_only").data());
            renderCancelButton();
            ImGui::EndPopup();
            return;
        }

        if ( m_resetTarget || m_entity != entity ||
             m_objectKind != objectKind ) {
            m_entity      = entity;
            m_objectKind  = objectKind;
            m_subIndex    = resolveInitialSubIndex(contextState, entity);
            m_resetTarget = false;
            m_content.fill('\0');
        }

        const Logic::NoteComponent* note =
            targetsSample
                ? nullptr
                : &contextState.noteRegistry.get<const Logic::NoteComponent>(
                      entity);
        renderTargetSelector(note, context.dpiScale);

        const bool canAnnotate =
            hasBeatmapMutationFlag(session->collaborationAllowedMutationFlags(),
                                   ::MMM::BeatmapMutationFlags::Annotations) &&
            !session->isCollaborationOfflineReadOnly();
        const std::string creator = Config::normalizeCreatorIdentity(
            Config::AppConfig::instance().getEditorSettings().defaultCreator);
        ImGui::Text("%s: %s",
                    TR("ui.annotation.author").data(),
                    creator.empty() ? TR("ui.annotation.unknown_author").data()
                                    : creator.c_str());
        if ( creator.empty() ) {
            ImGui::TextColored(ImVec4(1.0F, 0.34F, 0.25F, 1.0F),
                               "%s",
                               TR("ui.annotation.creator_required").data());
        }
        if ( !canAnnotate ) {
            ImGui::TextColored(
                ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                "%s",
                TR("ui.edit.note_metadata.annotation_denied").data());
        }
        ImGui::TextDisabled("%s", TR("ui.annotation.markdown_hint").data());
        ImGui::InputTextMultiline("##SelectedObjectAnnotationMarkdown",
                                  m_content.data(),
                                  m_content.size(),
                                  ImVec2(-1.0F, 220.0F * context.dpiScale));

        ImGui::BeginDisabled(!canAnnotate || creator.empty() ||
                             m_content.front() == '\0');
        if ( ::MMM::UI::FeedbackButton(TR("ui.annotation.add").data()) ) {
            engine.pushCommand(Logic::CmdUpsertBeatmapAnnotation{
                .targetKind =
                    targetsSample
                        ? ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE
                        : ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
                .objectKind = objectKind,
                .entity     = entity,
                .subIndex   = m_subIndex,
                .author     = creator,
                .content    = std::string(m_content.data()),
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        renderCancelButton();
        ImGui::EndPopup();
    }

private:
    /// @brief 判断选中实体是否仍是对应 Registry 中的可用批注目标。
    static bool isValidTarget(const Logic::SessionContext& context,
                              entt::entity                 entity,
                              Logic::ChartObjectKind       objectKind)
    {
        if ( objectKind == Logic::ChartObjectKind::AudioSample ) {
            return context.sampleRegistry.valid(entity) &&
                   context.sampleRegistry.all_of<Logic::SampleComponent>(
                       entity);
        }
        return context.noteRegistry.valid(entity) &&
               context.noteRegistry.all_of<Logic::NoteComponent>(entity);
    }

    /// @brief 折线子物件正被悬停时优先以该子物件为批注目标。
    static std::int32_t resolveInitialSubIndex(
        const Logic::SessionContext& context, entt::entity entity)
    {
        if ( context.hoveredEntity != entity || context.hoveredSubIndex < 0 ||
             !context.noteRegistry.valid(entity) ||
             !context.noteRegistry.all_of<Logic::NoteComponent>(entity) ) {
            return -1;
        }
        const auto& note =
            context.noteRegistry.get<const Logic::NoteComponent>(entity);
        if ( note.m_type != ::MMM::NoteType::POLYLINE ||
             static_cast<std::size_t>(context.hoveredSubIndex) >=
                 note.m_subNotes.size() ) {
            return -1;
        }
        return context.hoveredSubIndex;
    }

    /// @brief 绘制整个物件与折线子物件目标选择器。
    void renderTargetSelector(const Logic::NoteComponent* note, float dpiScale)
    {
        if ( !note ) {
            ImGui::Text("%s: %s",
                        TR("ui.annotation.target").data(),
                        TR("ui.annotation.target.audio_sample").data());
            return;
        }
        if ( note->m_type != ::MMM::NoteType::POLYLINE ||
             note->m_subNotes.empty() ) {
            m_subIndex = -1;
            ImGui::Text("%s: %s",
                        TR("ui.annotation.target").data(),
                        TR("ui.edit.note_metadata.annotation_whole").data());
            return;
        }
        if ( m_subIndex >= 0 &&
             static_cast<std::size_t>(m_subIndex) >= note->m_subNotes.size() ) {
            m_subIndex = -1;
        }

        const std::string preview =
            m_subIndex < 0
                ? TR("ui.edit.note_metadata.annotation_whole").toString()
                : TR_FMT("ui.edit.note_metadata.annotation_subnote",
                         m_subIndex + 1);
        ImGui::SetNextItemWidth(std::max(240.0F * dpiScale, 320.0F));
        if ( FeedbackBeginCombo(
                 TR("ui.edit.note_metadata.annotation_target").data(),
                 preview.c_str()) ) {
            if ( FeedbackSelectable(
                     TR("ui.edit.note_metadata.annotation_whole").data(),
                     m_subIndex < 0) ) {
                m_subIndex = -1;
            }
            for ( std::size_t index = 0U; index < note->m_subNotes.size();
                  ++index ) {
                const auto label = TR_FMT(
                    "ui.edit.note_metadata.annotation_subnote", index + 1U);
                if ( FeedbackSelectable(
                         label.c_str(),
                         m_subIndex == static_cast<std::int32_t>(index)) ) {
                    m_subIndex = static_cast<std::int32_t>(index);
                }
            }
            FeedbackEndCombo();
        }
    }

    /// @brief 绘制关闭当前批注弹窗的取消按钮。
    static void renderCancelButton()
    {
        if ( ::MMM::UI::FeedbackButton(TR("ui.annotation.cancel").data()) ) {
            ImGui::CloseCurrentPopup();
        }
    }

    /// @brief 下一帧是否请求打开弹窗。
    bool m_requestOpen{ false };

    /// @brief 下一次解析有效目标时是否重置表单。
    bool m_resetTarget{ true };

    /// @brief 当前弹窗对应的目标实体。
    entt::entity m_entity{ entt::null };

    /// @brief 当前目标所在的独立 ECS Registry。
    Logic::ChartObjectKind m_objectKind{ Logic::ChartObjectKind::PlayerNote };

    /// @brief -1 表示整个物件，非负值表示折线子物件。
    std::int32_t m_subIndex{ -1 };

    /// @brief 固定上限的 UTF-8 Markdown 编辑缓冲区。
    std::array<char, ::MMM::MAX_BEATMAP_ANNOTATION_CONTENT_BYTES + 1U>
        m_content{};

    /// @brief 当前帧快捷键显示缓存。
    mutable std::string m_shortcutBuffer;
};
}  // namespace

/// @brief 创建为选中物件添加批注的动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createAddSelectedObjectAnnotationAction()
{
    return std::make_unique<AddSelectedObjectAnnotationAction>();
}

}  // namespace MMM::UI
