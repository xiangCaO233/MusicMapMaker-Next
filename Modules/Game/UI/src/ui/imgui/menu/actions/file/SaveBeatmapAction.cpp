#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"

#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/BeatmapSaveConflictEvent.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/UIWidgetUtils.h"

#include <concurrentqueue.h>
#include <imgui.h>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 跨线程传递给 UI 帧内消费的保存冲突确认载荷。
/// @details
/// 逻辑线程不直接操作 ImGui，只复制弹窗所需的最小数据。
/// 路径在事件发布前已转为 UTF-8，消费端不再访问文件系统。
/// 载荷按值进入无锁队列，不借用事件对象的短暂生命周期。
struct SaveConflictPayload {
    /// @brief 存在覆盖风险的目标路径，使用 UTF-8 字符串。
    std::string path;
};

/// @brief 跨线程传递给保存 action 的保存结果载荷。
/// @details
/// 保存 action 只关心当前谱面保存是否已结束，不重复处理成败文案。
/// 导出结果交给其各自 action，避免清除一次性格式选择。
/// 这里不保存路径，因为路径匹配由逻辑层的冲突检查负责。
struct SaveResultPayload {
    /// @brief 是否来自另存为/导出流程。
    bool isExport{ false };
};

/// @brief 获取保存冲突确认队列。
/// @return 进程内唯一的冲突载荷队列。
/// @note 队列生命周期覆盖 EventBus 订阅，回调不持有 action 指针。
/// @warning 生产者可位于逻辑线程；只允许 UI 帧消费并更改弹窗状态。
moodycamel::ConcurrentQueue<SaveConflictPayload>& getSaveConflictQueue()
{
    static moodycamel::ConcurrentQueue<SaveConflictPayload> queue;
    return queue;
}

/// @brief 获取保存结果队列，用于重置当前保存确认状态。
/// @return 进程内唯一的保存结果队列。
/// @note 结果只用于结束一次格式覆写，可视反馈由独立组件呈现。
/// @warning 与冲突队列相同，此队列只在 UI 线程取出数据。
moodycamel::ConcurrentQueue<SaveResultPayload>& getSaveResultQueue()
{
    static moodycamel::ConcurrentQueue<SaveResultPayload> queue;
    return queue;
}

/// @brief 订阅逻辑层保存冲突事件，将事件转交保存 action 帧内处理。
/// @details
/// 订阅只安装一次，避免菜单 action 重建后对同一事件重复入队。
/// 回调仅复制载荷，不读取会话、不打开弹窗也不等待 UI 线程。
/// 只有 Transient 的当前谱面保存结果才参与 action 状态收尾。
/// 自动保存与内部保存使用其他展示策略，不应干扰用户弹窗选择。
/// @warning 低频初始化路径；订阅回调可跨线程执行，不得访问 ImGui。
void ensureSaveActionSubscriptions()
{
    static bool subscribed = false;
    if ( subscribed ) return;

    Event::EventBus::instance().subscribe<Event::BeatmapSaveConflictEvent>(
        [](const Event::BeatmapSaveConflictEvent& event) {
            getSaveConflictQueue().enqueue(SaveConflictPayload{
                .path = event.path,
            });
        });
    Event::EventBus::instance().subscribe<Event::BeatmapSaveResultEvent>(
        [](const Event::BeatmapSaveResultEvent& event) {
            if ( event.presentation !=
                 Event::BeatmapSavePresentation::Transient ) {
                return;
            }
            getSaveResultQueue().enqueue(SaveResultPayload{
                .isExport = event.isExport,
            });
        });
    subscribed = true;
}

/// @brief 保存当前谱面动作，拥有保存建议、兼容性确认与覆盖确认状态。
/// @details
/// 菜单和 Ctrl+S 共用同一入口，因此格式偏好与保护规则不会分叉。
/// 原格式模式遇到草稿或批注时，先让用户选择 MMM 或原格式。
/// 格式确定后再运行既有导出兼容性检查，其确认不能跳过。
/// 逻辑层可继续拒绝覆盖外部变化的 MMM 文件，并通过事件返回。
/// 每次分发的格式覆写会保留至保存结束，供冲突确认重放原命令。
/// 选择“保存为 .mmm”只影响当前命令，不回写软件设置。
/// 选择“继续保存原格式”同样是显式覆写，不再重复弹出建议。
class SaveBeatmapAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 构造保存动作并确保保存事件订阅完成。
    /// @note action 自身不被 EventBus 回调捕获，重建不会留下悬空指针。
    /// @note 初始状态不打开任何弹窗，所有展示都由后续请求触发。
    SaveBeatmapAction() { ensureSaveActionSubscriptions(); }

    /// @brief 仅在存在活跃谱面时允许保存。
    /// @param context 当前主菜单上下文，本判定不需要其他字段。
    /// @return 活跃会话已加载谱面时返回 true。
    /// @warning UI 菜单热路径；只读取会话存在性，不遍历谱面内容。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return MenuUtil::hasActiveBeatmap(false);
    }

    /// @brief 请求保存当前谱面。
    /// @param context 当前主菜单上下文，保存入口无需读取其它状态。
    /// @param activation 菜单激活信息，实际保存语义不区分鼠标与键盘。
    /// @note 始终从未确认覆盖的状态开始，覆盖授权只来自冲突弹窗。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        requestSaveBeatmap(false);
    }

    /// @brief 消费 Ctrl+S 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note Ctrl+Shift+S 由另存为 action 处理，不能在此误触发普通保存。
    /// @note 命中后与菜单入口共用 requestSaveBeatmap，保持提示顺序一致。
    bool handleShortcut(MainMenuContext& context) override
    {
        (void)context;
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S) ) {
            requestSaveBeatmap(false);
            return true;
        }
        return false;
    }

    /// @brief 消费跨线程保存事件。
    /// @warning UI 热路径：每帧只消费无锁队列，不执行阻塞操作。
    /// @details
    /// 冲突载荷只覆盖最新待展示路径，每个事件都会设置打开标志。
    /// 保存结果到达后清除一次性格式覆写，下次 Ctrl+S 重新读取设置。
    /// 导出结果不属于当前谱面保存，不能结束正在进行的冲突上下文。
    /// 成功与失败都结束该次命令，因此两者都应重置格式覆写。
    void update(MainMenuContext& context) override
    {
        (void)context;
        SaveConflictPayload conflictPayload;
        while ( getSaveConflictQueue().try_dequeue(conflictPayload) ) {
            // 弹窗在 renderDeferred 打开，事件消费阶段不改变 ImGui 栈。
            m_pendingSaveConflictPath = conflictPayload.path;
            m_showSaveConflictWarning = true;
        }

        SaveResultPayload resultPayload;
        while ( getSaveResultQueue().try_dequeue(resultPayload) ) {
            if ( !resultPayload.isExport ) {
                // 一次性选择不得泄漏到下一次用户保存。
                m_currentSaveFormatOverride =
                    Logic::BeatmapSaveFormatOverride::Configured;
            }
        }
    }

    /// @brief 渲染保存动作拥有的延迟弹窗，并消费快捷键请求。
    /// @warning UI 热路径：每帧检查布尔标志；只渲染已打开弹窗。
    /// @details
    /// 建议、兼容性和冲突是递进门禁，保持这一顺序才能解释最终写入目标。
    /// 每个弹窗使用独立稳定 ID，不会因显示文本或语言切换与其它弹窗冲突。
    /// 后一层弹窗由当前按钮请求下一帧打开，不在同一 ImGui 弹窗栈上嵌套。
    void renderDeferred(MainMenuContext& context) override
    {
        renderRichContentSaveSuggestionPopup(context.dpiScale);
        renderCompatibilityWarningPopup(context.dpiScale);
        renderSaveConflictWarningPopup(context.dpiScale);
    }

private:
    /// @brief 直接分发当前谱面保存命令。
    /// @param allowExternallyModifiedOverwrite
    /// 是否允许覆盖外部修改过的当前文件。
    /// @param formatOverride 本次保存使用的格式覆写。
    /// @details
    /// 此函数已越过所有 UI 确认，只负责记录可重放状态并发送命令。
    /// 格式覆写必须与命令同时记录，不能在冲突返回后再从全局设置推断。
    /// 覆盖外部修改的布尔值只对当前命令有效，逻辑层不会持久化该授权。
    /// @warning 用户触发的低频路径；发送命令后不同步等待文件写入。
    void dispatchSaveBeatmap(bool allowExternallyModifiedOverwrite,
                             Logic::BeatmapSaveFormatOverride formatOverride)
    {
        // 冲突确认可能跨越多帧，必须记住产生冲突的精确格式选择。
        m_currentSaveFormatOverride = formatOverride;
        MenuUtil::dispatchCommand(Logic::CmdSaveBeatmap{
            .allowExternallyModifiedOverwrite =
                allowExternallyModifiedOverwrite,
            .formatOverride = formatOverride,
        });
    }

    /// @brief 请求保存当前谱面，必要时先展示格式兼容性警告。
    /// @param allowExternallyModifiedOverwrite
    /// 是否允许覆盖外部修改过的当前文件。
    /// @param formatOverride 本次保存的格式选择；Configured
    /// 表示先读取全局设置。
    /// @details
    /// 仅当调用者传入 Configured 且设置为原格式时考虑展示保存建议。
    /// 按钮回调传入 Original 或 ForceMMM 表示用户已经选择，不能再次建议。
    /// 草稿从当前会话 ECS 读取，因为它们不存在于正式 BeatMap Note 容器中。
    /// 批注从 BeatMap 读取，与正式物件同属当前谱面内容。
    /// 已是 .mmm 的源文件不需要建议，原格式保存本身已能承载相关内容。
    /// 选定 MMM 后先计算真实目标路径，再将它交给兼容性检查和逻辑层。
    /// 兼容性警告的确认状态保留相同格式覆写，避免两层弹窗改变保存语义。
    /// @warning 用户触发的低频路径；短暂持有会话锁并遍历 Note Registry。
    /// @warning 文件系统写入不在此执行，不能将该检查扩展为同步保存。
    void requestSaveBeatmap(bool allowExternallyModifiedOverwrite,
                            Logic::BeatmapSaveFormatOverride formatOverride =
                                Logic::BeatmapSaveFormatOverride::Configured)
    {
        const bool useConfiguredPreference =
            formatOverride == Logic::BeatmapSaveFormatOverride::Configured;
        std::string path;
        {
            auto& engine = Logic::EditorEngine::instance();
            // 会话与 Registry
            // 必须在同一把锁下读取，避免谱面切换造成内容与路径错配。
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();
            if ( session && session->getContext().currentBeatmap ) {
                const auto& sessionContext = session->getContext();
                auto        savePath =
                    sessionContext.currentBeatmap->m_baseMapMetadata.map_path;
                const auto configuredPreference = Config::AppConfig::instance()
                                                      .getEditorSettings()
                                                      .saveFormatPreference;

                // 初始请求读取全局偏好；弹窗选择则作为一次性明确覆写。
                // 从这里开始 formatOverride 不再是
                // Configured，后续异步链路持有的都是精确选择。
                if ( formatOverride ==
                     Logic::BeatmapSaveFormatOverride::Configured ) {
                    formatOverride =
                        configuredPreference ==
                                Config::SaveFormatPreference::ForceMMM
                            ? Logic::BeatmapSaveFormatOverride::ForceMMM
                            : Logic::BeatmapSaveFormatOverride::Original;
                }

                const bool hasAnnotations =
                    !sessionContext.currentBeatmap->m_annotations.empty();
                bool hasDrafts = false;
                // 只判断存在性，命中第一个草稿即停止，不为弹窗计算无用数量。
                const auto noteView = sessionContext.noteRegistry
                                          .view<const Logic::NoteComponent>();
                for ( const auto entity : noteView ) {
                    if ( noteView.get<const Logic::NoteComponent>(entity)
                             .m_isDraft ) {
                        hasDrafts = true;
                        break;
                    }
                }

                const std::string originalPath = Config::pathToUtf8(savePath);
                // 显式 Original
                // 表示用户已在本弹窗选择继续，useConfiguredPreference
                // 阻止循环提示。
                if ( useConfiguredPreference &&
                     formatOverride ==
                         Logic::BeatmapSaveFormatOverride::Original &&
                     MenuUtil::lowerExtension(originalPath) != ".mmm" &&
                     (hasDrafts || hasAnnotations) ) {
                    // 原格式无法完整承载草稿和批注，先让用户选择一次性 MMM
                    // 保存。
                    auto mmmPath = savePath;
                    mmmPath.replace_extension(".mmm");
                    m_pendingRichContentMmmPath   = Config::pathToUtf8(mmmPath);
                    m_pendingRichContentHasDrafts = hasDrafts;
                    m_pendingRichContentHasAnnotations = hasAnnotations;
                    m_pendingRichContentAllowOverwrite =
                        allowExternallyModifiedOverwrite;
                    // 只设置延迟打开标志，当前菜单调用栈不直接创建 ImGui 弹窗。
                    m_showRichContentSaveSuggestion = true;
                    return;
                }

                if ( formatOverride ==
                     Logic::BeatmapSaveFormatOverride::ForceMMM ) {
                    // 这一路径同时用于全局 ForceMMM 和建议弹窗的一次性选择。
                    savePath.replace_extension(".mmm");
                }
                path = Config::pathToUtf8(savePath);
            }
        }

        // 格式建议不取代既有导出兼容性警告，原格式选择仍可能需要二次确认。
        auto warnings = MenuUtil::collectExportCompatibilityWarnings(path);
        if ( warnings.empty() ) {
            dispatchSaveBeatmap(allowExternallyModifiedOverwrite,
                                formatOverride);
            return;
        }

        // 警告数据跨帧保留，按钮确认时使用同一目标与格式覆写。
        m_pendingExportPath     = std::move(path);
        m_pendingExportWarnings = std::move(warnings);
        m_pendingCompatibilityWarningAllowOverwrite =
            allowExternallyModifiedOverwrite;
        m_pendingCompatibilityWarningFormatOverride = formatOverride;
        m_showExportCompatibilityWarning            = true;
    }

    /// @brief 清空草稿或批注的 MMM 保存建议状态。
    /// @details
    /// 用户选择、取消后均调用此函数，不让上次谱面信息泄漏到新弹窗。
    /// 打开标志由渲染函数消费，此函数只处理载荷与后续授权。
    /// @note 清空路径不会删除或改写任何文件。
    void clearRichContentSaveSuggestionState()
    {
        m_pendingRichContentMmmPath.clear();
        m_pendingRichContentHasDrafts      = false;
        m_pendingRichContentHasAnnotations = false;
        m_pendingRichContentAllowOverwrite = false;
    }

    /// @brief 清空当前兼容性确认弹窗状态。
    /// @details
    /// 路径、警告、覆盖授权和格式选择是同一份快照，必须一起重置。
    /// 格式回到 Configured 只表示无待处理快照，不会改写正在保存的命令。
    /// @note 清理容器发生在用户按钮路径，不在每帧无弹窗分支执行。
    void clearCompatibilityWarningState()
    {
        m_pendingExportPath.clear();
        m_pendingExportWarnings.clear();
        m_pendingCompatibilityWarningAllowOverwrite = false;
        m_pendingCompatibilityWarningFormatOverride =
            Logic::BeatmapSaveFormatOverride::Configured;
    }

    /// @brief 渲染草稿或批注存在时的 MMM 保存建议弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    /// @details
    /// 标题取自翻译表，### 后的稳定 ID 保证语言切换不改变弹窗身份。
    /// 正文根据草稿、批注或两者同时存在选择对应翻译，不拼接语言片段。
    /// MMM 目标路径明确展示，便于用户在可能覆盖同名文件前确认。
    /// 三个按钮分别产生 ForceMMM、Original 或不发送命令的结果。
    /// 选择保存后先关闭当前弹窗，再请求下一层兼容性检查。
    /// 覆盖授权只在外部冲突确认后可能为 true，普通 Ctrl+S 不会自行放行。
    /// @warning UI 热路径；未打开时只进行弹窗 ID 与打开状态检查。
    void renderRichContentSaveSuggestionPopup(float dpiScale)
    {
        const std::string popupId =
            TR("ui.save.rich_content.title").toString() +
            "###RichContentSaveSuggestionModal";
        if ( m_showRichContentSaveSuggestion ) {
            // OpenPopup 必须在 UI 帧内执行，请求阶段只留下这个一次性标志。
            ::MMM::UI::FeedbackOpenPopup(popupId.c_str());
            m_showRichContentSaveSuggestion = false;
        }

        if ( !ImGui::IsPopupOpen(popupId.c_str()) ) return;

        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId.c_str(),
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(590.0f * dpiScale, 0.0f)) ) {
            const char* messageKey =
                m_pendingRichContentHasDrafts
                    ? (m_pendingRichContentHasAnnotations
                           ? "ui.save.rich_content.message.both"
                           : "ui.save.rich_content.message.draft")
                    : "ui.save.rich_content.message.annotation";
            // 完整句子由翻译资源提供，保持中英文语序各自自然。
            ImGui::TextWrapped("%s", TR(messageKey).data());

            ImGui::Spacing();
            MenuUtil::drawWrappedLabelValue(
                TR("ui.save.rich_content.target").view(),
                m_pendingRichContentMmmPath);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 buttonSize(160.0f * dpiScale, 0.0f);
            const float  buttonRowWidth =
                buttonSize.x * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
            MenuUtil::centerNextItem(buttonRowWidth);
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.save.rich_content.save_mmm").data(), buttonSize) ) {
                // 在清理载荷前复制授权，后续请求不再依赖弹窗成员。
                const bool allowOverwrite = m_pendingRichContentAllowOverwrite;
                clearRichContentSaveSuggestionState();
                ImGui::CloseCurrentPopup();
                requestSaveBeatmap(allowOverwrite,
                                   Logic::BeatmapSaveFormatOverride::ForceMMM);
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.save.rich_content.continue_original").data(),
                     buttonSize) ) {
                // 显式 Original
                // 使后续请求跳过本建议，但仍执行原格式兼容性检查。
                const bool allowOverwrite = m_pendingRichContentAllowOverwrite;
                clearRichContentSaveSuggestionState();
                ImGui::CloseCurrentPopup();
                requestSaveBeatmap(allowOverwrite,
                                   Logic::BeatmapSaveFormatOverride::Original);
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           buttonSize) ) {
                // 取消不发送保存命令，谱面脏状态继续保留。
                clearRichContentSaveSuggestionState();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    /// @brief 渲染保存目标格式兼容性警告弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    /// @details
    /// 该弹窗复用既有 Malody Key 等降级提示，不与草稿保存建议合并。
    /// 警告列表和目标路径均来自 requestSaveBeatmap 的同一快照。
    /// 继续保存时传递快照中的格式覆写，建议弹窗的选择不会丢失。
    /// 取消只清理兼容性快照，不改变谱面路径或全局格式偏好。
    /// @warning UI 热路径；警告列表只在弹窗打开时遍历。
    void renderCompatibilityWarningPopup(float dpiScale)
    {
        constexpr const char* popupId =
            "谱面兼容性警告###CurrentBeatmapSaveWarningModal";
        if ( m_showExportCompatibilityWarning ) {
            // 打开标志只消费一次，弹窗后续帧由 ImGui 自身维持。
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_showExportCompatibilityWarning = false;
        }

        if ( !ImGui::IsPopupOpen(popupId) ) return;

        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(520.0f * dpiScale, 0.0f)) ) {
            ImGui::Text(
                "%s %s 前需要确认以下兼容性变化：", "保存", "Malody Key");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            for ( const auto& warning : m_pendingExportWarnings ) {
                // 每项单独换行，保留兼容性收集器给出的完整语义。
                MenuUtil::drawWrappedBulletText(warning);
            }

            ImGui::Spacing();
            MenuUtil::drawWrappedLabelValue("目标文件：", m_pendingExportPath);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 actionButtonSize(120.0f * dpiScale, 0.0f);
            const float  actionButtonRowWidth =
                actionButtonSize.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            MenuUtil::centerNextItem(actionButtonRowWidth);
            if ( ::MMM::UI::FeedbackButton("继续保存", actionButtonSize) ) {
                // 先分发命令再清理快照，dispatch 会复制冲突重放所需格式。
                dispatchSaveBeatmap(
                    m_pendingCompatibilityWarningAllowOverwrite,
                    m_pendingCompatibilityWarningFormatOverride);
                clearCompatibilityWarningState();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           actionButtonSize) ) {
                // 取消不标记已保存，下次 Ctrl+S 仍会重新评估当前内容。
                clearCompatibilityWarningState();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    /// @brief 渲染保存目标被外部修改时的覆盖确认弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    /// @details
    /// 冲突由逻辑层根据目标文件哈希发布，UI 不重复访问文件系统。
    /// 确认覆盖重放最近一次分发的格式覆写，一次性 MMM 选择保持有效。
    /// 重放命令带有 allowExternallyModifiedOverwrite，逻辑层将放行该次写入。
    /// 取消会遗弃格式覆写，下次普通保存将重新从设置计算。
    /// 目标路径是逻辑层实际计算的写入路径，可能与当前源扩展名不同。
    /// @warning UI 热路径；只渲染已入队的冲突，不计算哈希或阻塞等待。
    void renderSaveConflictWarningPopup(float dpiScale)
    {
        constexpr const char* popupId =
            "文件已被另外修改过###SaveConflictWarningModal";
        if ( m_showSaveConflictWarning ) {
            // 事件回调不能打开 ImGui 弹窗，在此消费一次性请求。
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_showSaveConflictWarning = false;
        }

        if ( !ImGui::IsPopupOpen(popupId) ) return;

        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(540.0f * dpiScale, 0.0f)) ) {
            ImGui::TextWrapped(
                "文件已被另外修改过，强行覆盖可能会导致丢失数据，是否确认？");
            if ( !m_pendingSaveConflictPath.empty() ) {
                // 显示实际写入目标，便于用户区分源格式与同名 MMM 文件。
                ImGui::Spacing();
                ImGui::TextWrapped("目标文件：%s",
                                   m_pendingSaveConflictPath.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 buttonSize(120.0f * dpiScale, 0.0f);
            if ( ::MMM::UI::FeedbackButton("确认覆盖", buttonSize) ) {
                // 重放原命令的格式覆写，避免一次性 MMM 选择被全局偏好覆盖。
                // 逻辑层将在成功或失败后发布结果，update 再结束当前覆写。
                dispatchSaveBeatmap(true, m_currentSaveFormatOverride);
                m_pendingSaveConflictPath.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           buttonSize) ) {
                // 不发送带覆盖授权的命令，保持磁盘文件和谱面脏状态不变。
                m_pendingSaveConflictPath.clear();
                m_currentSaveFormatOverride =
                    Logic::BeatmapSaveFormatOverride::Configured;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    /// @brief 是否在下一帧打开草稿或批注的 MMM 保存建议弹窗。
    bool m_showRichContentSaveSuggestion = false;
    /// @brief 是否在下一帧打开保存兼容性警告弹窗。
    bool m_showExportCompatibilityWarning = false;
    /// @brief 是否在下一帧打开保存覆盖风险确认弹窗。
    bool m_showSaveConflictWarning = false;
    /// @brief 待确认当前保存是否允许覆盖外部修改。
    bool m_pendingCompatibilityWarningAllowOverwrite = false;
    /// @brief 待确认兼容性警告的格式覆写。
    Logic::BeatmapSaveFormatOverride
        m_pendingCompatibilityWarningFormatOverride{
            Logic::BeatmapSaveFormatOverride::Configured
        };
    /// @brief 当前已分发保存的格式覆写，用于冲突确认后重放。
    Logic::BeatmapSaveFormatOverride m_currentSaveFormatOverride{
        Logic::BeatmapSaveFormatOverride::Configured
    };
    /// @brief 建议 MMM 保存的目标路径。
    std::string m_pendingRichContentMmmPath;
    /// @brief 待确认谱面是否包含草稿。
    bool m_pendingRichContentHasDrafts = false;
    /// @brief 待确认谱面是否包含批注。
    bool m_pendingRichContentHasAnnotations = false;
    /// @brief 建议弹窗后续保存是否允许覆盖外修改。
    bool m_pendingRichContentAllowOverwrite = false;
    /// @brief 待确认保存的目标路径。
    std::string m_pendingExportPath;
    /// @brief 待确认保存的兼容性警告消息。
    std::vector<std::string> m_pendingExportWarnings;
    /// @brief 待确认覆盖的保存目标路径。
    std::string m_pendingSaveConflictPath;
};
}  // namespace

/// @brief 创建保存当前谱面的菜单项业务处理器。
/// @return 拥有独立弹窗状态的保存 action。
/// @note 事件订阅是进程级的，action 实例只负责消费队列与渲染。
/// @note 返回基类所有权，生命周期由主菜单 action 注册表管理。
std::unique_ptr<IMainMenuItemActionHandler> createSaveBeatmapAction()
{
    return std::make_unique<SaveBeatmapAction>();
}

}  // namespace MMM::UI
