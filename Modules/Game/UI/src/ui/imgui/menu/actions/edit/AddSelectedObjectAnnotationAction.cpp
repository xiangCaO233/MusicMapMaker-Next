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
/// @details 统一快捷键、目标解析、协作权限检查与批注命令构造。
/// @warning 弹窗状态仅在 UI 线程访问，实体句柄每帧都要重新验证。
class AddSelectedObjectAnnotationAction final
    : public IMainMenuItemActionHandler
{
public:
    /// @brief 获取用户配置的添加物件批注快捷键提示。
    /// @param context 单帧主菜单上下文，本查询无需读取。
    /// @param fallbackShortcut 配置无法格式化时使用的静态提示。
    /// @return 在下一次缓存刷新前有效的字符串指针。
    /// @warning UI 热路径：只读取内存配置并格式化单个快捷键。
    const char* shortcut(const MainMenuContext& context,
                         const char*            fallbackShortcut) const override
    {
        (void)context;
        // 引用快捷键子配置，避免复制完整编辑器设置。
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        // 成员缓存为返回的 c_str 提供菜单帧内稳定存储。
        m_shortcutBuffer =
            ShortcutUtils::formatShortcut(shortcutConfig.addSelectedAnnotation);
        return m_shortcutBuffer.empty() ? fallbackShortcut
                                        : m_shortcutBuffer.c_str();
    }

    /// @brief 有活动谱面物件选区时允许打开批注弹窗。
    /// @warning UI 热路径：菜单每帧检查；只读取会话维护的选择索引。
    /// @param context 单帧主菜单上下文，本判断通过 EditorEngine 完成。
    /// @return 当前活动谱面至少选择一个可识别物件时返回 true。
    /// @note 精确的单选和实体有效性仍在弹窗渲染时复核。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().hasActiveChartObjectSelection();
    }

    /// @brief 请求打开独立物件批注弹窗。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不改变批注目标解析。
    /// @note 实际 OpenPopup 延迟到主菜单作用域结束后的渲染阶段。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        // 显式动作立即提供声音反馈，弹窗创建仍延迟执行。
        ::MMM::UI::PlayPopupOpenFeedback();
        // 分离打开请求与当前 ImGui 弹窗状态，避免嵌套弹窗作用域。
        m_requestOpen = true;
        // 每次用户重新打开都重新解析选择，并清空旧表单内容。
        m_resetTarget = true;
    }

    /// @brief 消费用户配置的添加物件批注快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取快捷键状态。
    /// @note 画布编辑快捷键被抑制时不检查用户绑定。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        // 每帧读取最新绑定，运行时重配无需重建动作处理器。
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        if ( ShortcutUtils::isShortcutPressed(
                 shortcutConfig.addSelectedAnnotation) ) {
            // 复用菜单执行入口，确保声音和目标重置语义一致。
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }

    /// @brief 渲染独立的选中物件批注弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：弹窗打开时只读取选择索引和单个目标组件，
    /// 不遍历完整 ECS，不访问文件系统。
    /// @note 会话互斥锁覆盖实体解析、表单绘制和命令入队所需状态。
    void renderDeferred(MainMenuContext& context) override
    {
        // 标题每帧从翻译构造，当前实现以完整标题作为模态弹窗 ID。
        std::string popupLabel =
            TR("ui.edit.add_selected_annotation.title").toString();
        if ( m_requestOpen ) {
            // 将一次性请求转换为 ImGui 弹窗状态后立即清除。
            ImGui::OpenPopup(popupLabel.c_str());
            m_requestOpen = false;
        }

        // Appearing 仅在首次出现时设置宽度，随后交给自动布局。
        ImGui::SetNextWindowSize(ImVec2(560.0F * context.dpiScale, 0.0F),
                                 ImGuiCond_Appearing);
        if ( !ImGui::BeginPopupModal(popupLabel.c_str(),
                                     nullptr,
                                     ImGuiWindowFlags_AlwaysAutoResize) ) {
            // 未打开时不获取会话锁，也不访问选择索引。
            return;
        }

        // EditorEngine 是活动会话和命令队列的权威入口。
        auto& engine = Logic::EditorEngine::instance();
        // 锁定期间保护活动 Session 切换以及其内部选择与 Registry。
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        const auto session = engine.getActiveSession();
        if ( !session ) {
            // 会话在菜单点击后可能关闭，弹窗以只读提示安全退化。
            ImGui::TextUnformatted(TR("ui.tools.no_active_session").data());
            // 仍提供取消按钮，让用户显式关闭已打开模态框。
            renderCancelButton();
            // 所有早退分支必须在返回前配对 EndPopup。
            ImGui::EndPopup();
            return;
        }

        // 可变上下文只在持锁期间使用，所有实体句柄均属于对应 Registry。
        auto& contextState = session->getContextMutable();
        // 两个独立 Registry 的选择数量相加后必须恰好为一。
        const std::size_t selectionCount =
            contextState.selectedNoteEntities.size() +
            contextState.selectedSampleEntities.size();
        if ( selectionCount != 1U ) {
            // 批注目标协议一次只接受一个对象，禁止隐式挑选第一项。
            ImGui::TextWrapped(
                "%s",
                TR("ui.edit.note_metadata.annotation_single_only").data());
            renderCancelButton();
            // 单选条件失效时保留表单状态，重新打开会统一重置。
            ImGui::EndPopup();
            return;
        }

        // 独立采样选择优先确定 Registry；单选约束保证另一集合为空。
        const bool targetsSample = !contextState.selectedSampleEntities.empty();
        // begin 访问安全来自 selectionCount 恰为一的前置检查。
        const entt::entity entity =
            targetsSample ? *contextState.selectedSampleEntities.begin()
                          : *contextState.selectedNoteEntities.begin();
        // 对象种类与实体来源保持一致，供验证和命令目标使用。
        const auto objectKind = targetsSample
                                    ? Logic::ChartObjectKind::AudioSample
                                    : Logic::ChartObjectKind::PlayerNote;
        if ( !isValidTarget(contextState, entity, objectKind) ) {
            // 选择索引可能在弹窗期间因删除命令失效，必须拒绝悬空目标。
            ImGui::TextWrapped(
                "%s",
                TR("ui.edit.note_metadata.annotation_single_only").data());
            renderCancelButton();
            // 不尝试自动切换到其他对象，避免批注写到非用户选择目标。
            ImGui::EndPopup();
            return;
        }

        // 首次打开、实体变化或 Registry 种类变化时重建表单目标快照。
        if ( m_resetTarget || m_entity != entity ||
             m_objectKind != objectKind ) {
            m_entity     = entity;
            m_objectKind = objectKind;
            // 折线悬停子物件可作为初始目标，其他对象回退到整体。
            m_subIndex    = resolveInitialSubIndex(contextState, entity);
            m_resetTarget = false;
            // 新目标不得继承前一个实体尚未提交的 Markdown 内容。
            m_content.fill('\0');
        }

        // 自动采样没有 NoteComponent，因此向选择器传递空指针显示固定目标。
        const Logic::NoteComponent* note =
            targetsSample
                ? nullptr
                : &contextState.noteRegistry.get<const Logic::NoteComponent>(
                      entity);
        // 目标选择器只修改 subIndex，不保存组件引用。
        renderTargetSelector(note, context.dpiScale);

        // 协作会话同时要求协议允许批注变更且不处于离线只读状态。
        const bool canAnnotate =
            hasBeatmapMutationFlag(session->collaborationAllowedMutationFlags(),
                                   ::MMM::BeatmapMutationFlags::Annotations) &&
            !session->isCollaborationOfflineReadOnly();
        // 创作者身份在显示与提交前使用同一规范化规则。
        const std::string creator = Config::normalizeCreatorIdentity(
            Config::AppConfig::instance().getEditorSettings().defaultCreator);
        ImGui::Text("%s: %s",
                    TR("ui.annotation.author").data(),
                    creator.empty() ? TR("ui.annotation.unknown_author").data()
                                    : creator.c_str());
        if ( creator.empty() ) {
            // 身份为空时给出独立错误提示，并在下方禁用提交按钮。
            ImGui::TextColored(ImVec4(1.0F, 0.34F, 0.25F, 1.0F),
                               "%s",
                               TR("ui.annotation.creator_required").data());
        }
        if ( !canAnnotate ) {
            // 权限不足不关闭窗口，用户仍可查看当前目标和已输入内容。
            ImGui::TextColored(
                ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                "%s",
                TR("ui.edit.note_metadata.annotation_denied").data());
        }
        // Markdown 提示描述内容格式，不参与输入校验。
        ImGui::TextDisabled("%s", TR("ui.annotation.markdown_hint").data());
        // 固定数组容量与 BeatmapAnnotation 协议上限一致，并预留结尾空字符。
        ImGui::InputTextMultiline("##SelectedObjectAnnotationMarkdown",
                                  m_content.data(),
                                  m_content.size(),
                                  ImVec2(-1.0F, 220.0F * context.dpiScale));

        // 提交需要权限、有效身份和至少一个非空首字节。
        ImGui::BeginDisabled(!canAnnotate || creator.empty() ||
                             m_content.front() == '\0');
        if ( ::MMM::UI::FeedbackButton(TR("ui.annotation.add").data()) ) {
            // 命令按值复制所有字段，离开弹窗与会话锁后不依赖 UI 缓冲区。
            engine.pushCommand(Logic::CmdUpsertBeatmapAnnotation{
                .targetKind =
                    // 目标协议类型由实体所在 Registry 唯一决定。
                targetsSample
                    ? ::MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE
                    : ::MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
                .objectKind = objectKind,
                // entity 与 subIndex 已在本帧持锁状态下验证。
                .entity   = entity,
                .subIndex = m_subIndex,
                .author   = creator,
                // 从空字符结尾缓冲区构造拥有所有权的 UTF-8 字符串。
                .content = std::string(m_content.data()),
            });
            // 命令成功入队后关闭弹窗，避免重复提交相同批注。
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        // 取消按钮与添加按钮同行显示，并始终保持可用。
        ImGui::SameLine();
        renderCancelButton();
        // 与成功 BeginPopupModal 严格配对。
        ImGui::EndPopup();
    }

private:
    /// @brief 判断选中实体是否仍是对应 Registry 中的可用批注目标。
    /// @param context 当前 Session 的 ECS 与选择上下文。
    /// @param entity 待验证实体句柄。
    /// @param objectKind 决定实体所属 Registry 的对象种类。
    /// @return Registry 中实体有效且具有预期组件时返回 true。
    /// @warning 调用方必须持有 Session 互斥锁，返回值只反映当前锁区间。
    static bool isValidTarget(const Logic::SessionContext& context,
                              entt::entity                 entity,
                              Logic::ChartObjectKind       objectKind)
    {
        if ( objectKind == Logic::ChartObjectKind::AudioSample ) {
            // 自动采样必须属于 sampleRegistry 并保有 SampleComponent。
            return context.sampleRegistry.valid(entity) &&
                   context.sampleRegistry.all_of<Logic::SampleComponent>(
                       entity);
        }
        // 其余支持种类按玩家音符处理，并验证 NoteComponent 存在。
        return context.noteRegistry.valid(entity) &&
               context.noteRegistry.all_of<Logic::NoteComponent>(entity);
    }

    /// @brief 折线子物件正被悬停时优先以该子物件为批注目标。
    /// @param context 当前 Session 的交互与音符 Registry 状态。
    /// @param entity 当前唯一选中的玩家音符实体。
    /// @return 有效悬停折线子索引；不满足条件时返回 -1 表示整体。
    /// @warning 调用方必须持有 Session 互斥锁。
    static std::int32_t resolveInitialSubIndex(
        const Logic::SessionContext& context, entt::entity entity)
    {
        // 悬停实体、索引和组件必须同时有效，才能继续读取音符。
        if ( context.hoveredEntity != entity || context.hoveredSubIndex < 0 ||
             !context.noteRegistry.valid(entity) ||
             !context.noteRegistry.all_of<Logic::NoteComponent>(entity) ) {
            return -1;
        }
        // 组件引用仅在当前持锁调用中使用。
        const auto& note =
            context.noteRegistry.get<const Logic::NoteComponent>(entity);
        if ( note.m_type != ::MMM::NoteType::POLYLINE ||
             static_cast<std::size_t>(context.hoveredSubIndex) >=
                 note.m_subNotes.size() ) {
            // 非折线或越界悬停统一回退到整个物件。
            return -1;
        }
        // 返回已经验证可转换为 subNotes 索引的非负值。
        return context.hoveredSubIndex;
    }

    /// @brief 绘制整个物件与折线子物件目标选择器。
    /// @param note 玩家音符组件；自动采样目标传入 nullptr。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：仅遍历当前折线的子物件列表，不访问完整 ECS。
    void renderTargetSelector(const Logic::NoteComponent* note, float dpiScale)
    {
        if ( !note ) {
            // 空组件明确表示自动采样，目标不可进一步细分。
            ImGui::Text("%s: %s",
                        TR("ui.annotation.target").data(),
                        TR("ui.annotation.target.audio_sample").data());
            return;
        }
        if ( note->m_type != ::MMM::NoteType::POLYLINE ||
             note->m_subNotes.empty() ) {
            // 普通音符或无子物件折线只能批注整个对象。
            m_subIndex = -1;
            ImGui::Text("%s: %s",
                        TR("ui.annotation.target").data(),
                        TR("ui.edit.note_metadata.annotation_whole").data());
            return;
        }
        if ( m_subIndex >= 0 &&
             static_cast<std::size_t>(m_subIndex) >= note->m_subNotes.size() ) {
            // 折线编辑期间子物件数量可能变化，越界选择回退到整体。
            m_subIndex = -1;
        }

        // 预览文本始终反映当前安全化后的整体或子物件选择。
        const std::string preview =
            m_subIndex < 0
                ? TR("ui.edit.note_metadata.annotation_whole").toString()
                : TR_FMT("ui.edit.note_metadata.annotation_subnote",
                         m_subIndex + 1);
        // 组合框宽度同时满足 DPI 缩放和低分辨率下的最小可读宽度。
        ImGui::SetNextItemWidth(std::max(240.0F * dpiScale, 320.0F));
        if ( FeedbackBeginCombo(
                 TR("ui.edit.note_metadata.annotation_target").data(),
                 preview.c_str()) ) {
            // 整体选项使用 -1 作为与协议一致的哨兵值。
            if ( FeedbackSelectable(
                     TR("ui.edit.note_metadata.annotation_whole").data(),
                     m_subIndex < 0) ) {
                m_subIndex = -1;
            }
            // 子物件数量来自已验证组件，仅在当前帧遍历。
            for ( std::size_t index = 0U; index < note->m_subNotes.size();
                  ++index ) {
                // 面向用户的序号从 1 开始，内部索引仍保持从 0 开始。
                const auto label = TR_FMT(
                    "ui.edit.note_metadata.annotation_subnote", index + 1U);
                if ( FeedbackSelectable(
                         label.c_str(),
                         m_subIndex == static_cast<std::int32_t>(index)) ) {
                    // 协议上限保证 size_t 索引可安全表示为 int32_t。
                    m_subIndex = static_cast<std::int32_t>(index);
                }
            }
            // 与成功 FeedbackBeginCombo 配对，恢复 ImGui 组合框栈。
            FeedbackEndCombo();
        }
    }

    /// @brief 绘制关闭当前批注弹窗的取消按钮。
    /// @note 取消只关闭弹窗，不清空表单；下次 execute 会请求重置。
    /// @warning 必须在活动 ImGui 弹窗作用域内调用。
    static void renderCancelButton()
    {
        if ( ::MMM::UI::FeedbackButton(TR("ui.annotation.cancel").data()) ) {
            // 使用当前弹窗关闭入口，不依赖处理器额外可见性标志。
            ImGui::CloseCurrentPopup();
        }
    }

    /// @brief 下一帧是否请求打开弹窗。
    /// @note execute 置位，renderDeferred 转换为 ImGui 状态后清除。
    bool m_requestOpen{ false };

    /// @brief 下一次解析有效目标时是否重置表单。
    /// @note 用户重新触发动作时置位，目标快照建立后清除。
    bool m_resetTarget{ true };

    /// @brief 当前弹窗对应的目标实体。
    /// @note 句柄仅用于检测目标变化，每帧访问前仍通过 Registry 验证。
    entt::entity m_entity{ entt::null };

    /// @brief 当前目标所在的独立 ECS Registry。
    /// @note 与 m_entity 联合决定是否需要清空并重建表单。
    Logic::ChartObjectKind m_objectKind{ Logic::ChartObjectKind::PlayerNote };

    /// @brief -1 表示整个物件，非负值表示折线子物件。
    /// @note 每帧根据当前 NoteComponent 子物件数量执行越界修正。
    std::int32_t m_subIndex{ -1 };

    /// @brief 固定上限的 UTF-8 Markdown 编辑缓冲区。
    /// @note 容量比协议内容上限多一个字节，用于空字符结尾。
    std::array<char, ::MMM::MAX_BEATMAP_ANNOTATION_CONTENT_BYTES + 1U>
        m_content{};

    /// @brief 当前帧快捷键显示缓存。
    /// @note 只为 shortcut 返回值提供稳定存储。
    mutable std::string m_shortcutBuffer;
};
}  // namespace

/// @brief 创建为选中物件添加批注的动作处理器。
/// @return 独占所有权的物件批注处理器。
/// @warning 处理器必须在 EditorEngine 和 ImGui 所属 UI 线程使用。
std::unique_ptr<IMainMenuItemActionHandler>
createAddSelectedObjectAnnotationAction()
{
    return std::make_unique<AddSelectedObjectAnnotationAction>();
}

}  // namespace MMM::UI
