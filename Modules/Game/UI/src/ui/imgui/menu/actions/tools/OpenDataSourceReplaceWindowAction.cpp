#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/imgui/status/IStatusMessageSink.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 打开数据来源替换工具动作。
/// @details 从当前项目收集其他谱面，并按用户选择替换对象、时间线或元数据。
class OpenDataSourceReplaceWindowAction final
    : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在存在活跃谱面时允许替换数据来源。
    /// @param context 单帧主菜单上下文，本判断无需读取。
    /// @return 当前项目具有活动谱面时返回 true。
    /// @warning UI 热路径：只查询活动谱面状态，不得收集候选或访问文件系统。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return MenuUtil::hasActiveBeatmap(true);
    }

    /// @brief 打开数据来源替换工具窗口。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不改变工具初始化。
    /// @note 实际 OpenPopup 延迟到主菜单作用域结束后的渲染阶段。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        // 可见性与一次性打开请求分离，适配 ImGui 模态弹窗协议。
        m_showWindow          = true;
        m_openWindowRequested = true;
    }

    /// @brief 渲染数据来源替换工具窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；只在弹窗打开时遍历项目谱面候选。
    /// @note 所有窗口状态集中由 renderWindow 管理。
    void renderDeferred(MainMenuContext& context) override
    {
        renderWindow(context);
    }

private:
    /// @brief 数据来源替换工具中的候选谱面。
    /// @details 只保存显示与命令构造所需字符串，不持有 Project 条目引用。
    struct Candidate {
        /// @brief 项目相对谱面路径，使用 UTF-8 编码和通用分隔符。
        /// @note 该路径作为候选稳定键，并在提交时拼接项目根目录。
        std::string relativePath;

        /// @brief UI 中显示的谱面名称。
        /// @note 名称为空时由项目条目的文件路径兜底。
        std::string displayName;
    };

    /// @brief 收集可用于替换当前焦点谱面的项目谱面候选。
    /// @return 数据来源候选列表。
    /// @warning 低频模态窗口路径：会查询候选文件状态并排序完整项目谱面列表。
    /// @note 当前活动谱面被排除，防止将谱面自身作为替换来源。
    /// @warning 返回前不修改 Project 或 Session，仅生成独立候选快照。
    std::vector<Candidate> collectCandidates() const
    {
        std::vector<Candidate> candidates;

        // 当前 Project 由 EditorEngine 持有，只在本次同步收集期间观察。
        auto& engine  = Logic::EditorEngine::instance();
        auto* project = engine.getCurrentProject();
        // 项目或根目录缺失时无法解析相对谱面路径。
        if ( !project || project->m_projectRoot.empty() ) return candidates;

        // 空键表示没有可识别的活动谱面，此时只按文件有效性过滤。
        std::string activePathKey;
        {
            // 会话锁只覆盖活动谱面键读取，不包围文件系统查询和排序。
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();
            if ( session && session->getContext().currentBeatmap ) {
                // 使用与候选相同的规范化规则生成可比较绝对路径键。
                activePathKey = MenuUtil::makeProjectBeatmapPathKey(
                    project->m_projectRoot,
                    session->getContext()
                        .currentBeatmap->m_baseMapMetadata.map_path);
            }
        }

        // 以项目谱面数量预留上限容量，过滤过程中避免向量反复扩容。
        candidates.reserve(project->m_beatmaps.size());
        for ( const auto& entry : project->m_beatmaps ) {
            // 无文件路径条目无法加载为数据来源。
            if ( entry.m_filePath.empty() ) continue;

            // 先转换并词法规范化路径，统一点段和分隔符语义。
            auto relativePath =
                Config::utf8ToPath(entry.m_filePath).lexically_normal();
            auto candidatePathKey = MenuUtil::makeProjectBeatmapPathKey(
                project->m_projectRoot, relativePath);
            if ( candidatePathKey.empty() ||
                 candidatePathKey == activePathKey ) {
                // 无法规范化或与当前谱面相同的条目均不可选。
                continue;
            }

            // 使用 error_code API 遵守项目禁用异常机制的约束。
            std::error_code filesystemError;
            // 完整路径仅用于当前文件有效性检查，不写回项目配置。
            const auto fullPath =
                (project->m_projectRoot / relativePath).lexically_normal();
            if ( !std::filesystem::is_regular_file(fullPath, filesystemError) ||
                 filesystemError ) {
                // 缺失、非普通文件或查询失败的条目不进入 UI 清单。
                continue;
            }

            // 优先使用项目展示名，缺失时保留原始相对路径以便辨识。
            std::string displayName =
                entry.m_name.empty() ? entry.m_filePath : entry.m_name;
            candidates.push_back(Candidate{
                // 通用分隔符保证显示和后续路径比较跨平台稳定。
                .relativePath = Config::pathToUtf8Generic(relativePath),
                .displayName  = displayName,
            });
        }

        // 显示名优先排序，同名条目再按路径提供确定性顺序。
        std::sort(candidates.begin(),
                  candidates.end(),
                  [](const auto& lhs, const auto& rhs) {
                      if ( lhs.displayName != rhs.displayName ) {
                          // 名称不同直接使用字典序，便于用户浏览。
                          return lhs.displayName < rhs.displayName;
                      }
                      // 路径作为稳定次关键字，消除同名项目的不确定顺序。
                      return lhs.relativePath < rhs.relativePath;
                  });
        // 返回按值列表，离开函数后不保留 Project 条目引用。
        return candidates;
    }

    /// @brief 提交数据来源替换请求。
    /// @param context 单帧主菜单上下文。
    /// @warning 低频用户路径：同步加载来源谱面文件并创建共享命令载荷。
    /// @note 所有输入条件在加载前复核，避免无效请求进入逻辑层。
    /// @pre context.statusMessageSink 必须在当前 UI 帧内有效。
    void submitRequest(MainMenuContext& context)
    {
        // 执行时重新读取项目，防止窗口打开期间项目被关闭或切换。
        auto& engine  = Logic::EditorEngine::instance();
        auto* project = engine.getCurrentProject();
        if ( !project || project->m_projectRoot.empty() ||
             m_dataSourcePath.empty() ) {
            // 缺少项目上下文或来源选择时只反馈，不改变当前谱面。
            context.statusMessageSink.showStatusMessage(
                "没有可用的数据来源谱面", 3.0f);
            return;
        }

        if ( !m_replaceObjects && !m_replaceTimelines && !m_replaceMetadata ) {
            // 至少一个数据域必须启用，否则替换命令没有业务效果。
            context.statusMessageSink.showStatusMessage(
                "至少选择一种要替换的数据", 3.0f);
            return;
        }

        // 来源路径始终相对当前项目根解析，并进行词法规范化。
        const auto sourcePath =
            (project->m_projectRoot / Config::utf8ToPath(m_dataSourcePath))
                .lexically_normal();
        // 命令跨越当前调用持有来源谱面，故使用 shared_ptr 保护生命周期。
        auto sourceBeatmap = std::make_shared<MMM::BeatMap>(
            MMM::BeatMap::loadFromFile(sourcePath));
        if ( sourceBeatmap->m_baseMapMetadata.map_path.empty() ) {
            // 加载器以空 map_path 表达失败，拒绝发布不完整谱面。
            context.statusMessageSink.showStatusMessage("读取数据来源谱面失败",
                                                        3.0f);
            return;
        }

        // 替换范围通过三个显式标志传递，逻辑层负责事务与撤销。
        MenuUtil::dispatchCommand(Logic::CmdReplaceBeatmapData{
            .sourceBeatmap    = sourceBeatmap,
            .replaceObjects   = m_replaceObjects,
            .replaceTimelines = m_replaceTimelines,
            .replaceMetadata  = m_replaceMetadata,
        });

        // 命令成功投递后立即反馈；实际数据变更由逻辑队列执行。
        context.statusMessageSink.showStatusMessage("已替换当前谱面数据", 3.0f);
    }

    /// @brief 渲染数据来源替换工具窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；只在弹窗打开时遍历项目谱面候选。
    /// @warning 当前实现会在弹窗每帧调用 collectCandidates
    /// 的文件状态查询，待另行优化缓存。
    /// @note 候选刷新不会覆盖仍然有效的用户选择路径。
    void renderWindow(MainMenuContext& context)
    {
        // 固定 ### ID 保证可见中文标题变化时窗口状态仍稳定。
        constexpr const char* popupId =
            "数据来源替换工具###DataSourceReplaceModal";
        if ( m_openWindowRequested ) {
            // 将一次性请求转换为 ImGui 弹窗状态后立即清除。
            ::MMM::UI::FeedbackOpenPopup(popupId);
            m_openWindowRequested = false;
        }

        // 窗口不可见时跳过候选收集、文件查询和全部布局。
        if ( !m_showWindow ) return;

        // 两个按钮共享关闭标志，在控件绘制结束后统一关闭。
        bool closePopup = false;
        {
            // RAII 作用域在弹窗结束后恢复居中样式配置。
            Utils::CenteredModalPopupScope popupStyle(context.dpiScale);
            if ( popupStyle.begin(popupId,
                                  &m_showWindow,
                                  ImGuiWindowFlags_NoCollapse,
                                  ImVec2(640.0f * context.dpiScale,
                                         480.0f * context.dpiScale),
                                  false) ) {
                // 候选列表按当前项目状态重新生成，避免展示已删除谱面。
                auto candidates = collectCandidates();
                if ( m_dataSourcePath.empty() && !candidates.empty() ) {
                    // 首次打开默认选择排序后的第一项，减少额外操作。
                    m_dataSourcePath = candidates.front().relativePath;
                }
                if ( !m_dataSourcePath.empty() &&
                     std::none_of(candidates.begin(),
                                  candidates.end(),
                                  [&](const auto& candidate) {
                                      return candidate.relativePath ==
                                             m_dataSourcePath;
                                  }) ) {
                    // 原选择失效时回退首项；无候选则清空路径。
                    m_dataSourcePath = candidates.empty()
                                           ? std::string{}
                                           : candidates.front().relativePath;
                }

                // 列表标题与滚动区分离，滚动时保持上下文可见。
                ImGui::TextUnformatted("数据来源谱面");
                ImGui::Spacing();
                const float listHeight =
                    // 保证最小可用高度，并为底部选项和按钮预留空间。
                    std::max(120.0f * context.dpiScale,
                             ImGui::GetContentRegionAvail().y -
                                 126.0f * context.dpiScale);
                {
                    // 滚动条样式只覆盖候选列表子窗口。
                    Utils::VerticalScrollbarStyleScope scrollbarStyle(
                        context.dpiScale);
                    if ( ImGui::BeginChild("DataSourceReplaceBeatmapList",
                                           ImVec2(0.0f, listHeight),
                                           true) ) {
                        if ( candidates.empty() ) {
                            // 空列表明确提示，不创建无效可选项。
                            ImGui::TextDisabled("没有找到其他项目谱面。");
                        } else {
                            // 候选通常较少，按已排序顺序逐项绘制。
                            for ( const auto& candidate : candidates ) {
                                // 相对路径是选择状态的稳定键，显示名可能重复。
                                const bool selected =
                                    candidate.relativePath == m_dataSourcePath;
                                std::string label = candidate.displayName +
                                                    " - " +
                                                    candidate.relativePath;
                                // 标签同时显示名称与路径，帮助区分同名谱面。
                                if ( ::MMM::UI::FeedbackSelectable(
                                         label.c_str(), selected) ) {
                                    // 只复制相对路径，不保存候选对象引用。
                                    m_dataSourcePath = candidate.relativePath;
                                }
                            }
                        }
                    }
                    // 与 BeginChild 无条件配对，包括内容裁剪分支。
                    ImGui::EndChild();
                }

                // 三个复选框同行表达可独立组合的数据域选择。
                ImGui::Spacing();
                ::MMM::UI::FeedbackCheckbox("物件数据源", &m_replaceObjects);
                ImGui::SameLine();
                ::MMM::UI::FeedbackCheckbox("时间线源", &m_replaceTimelines);
                ImGui::SameLine();
                ::MMM::UI::FeedbackCheckbox("元数据源", &m_replaceMetadata);

                // 分隔候选与选项区域和最终操作按钮。
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // 应用条件同时要求有效候选、路径和至少一个替换域。
                const bool canApply = !candidates.empty() &&
                                      !m_dataSourcePath.empty() &&
                                      (m_replaceObjects || m_replaceTimelines ||
                                       m_replaceMetadata);
                // 按钮逻辑宽度随 DPI 缩放。
                const ImVec2 buttonSize(120.0f * context.dpiScale, 0.0f);
                // 仅包围替换按钮的禁用作用域，取消始终保持可用。
                if ( !canApply ) ImGui::BeginDisabled();
                if ( ::MMM::UI::FeedbackButton("替换", buttonSize) ) {
                    // 提交函数负责最终复核、加载谱面和分发命令。
                    submitRequest(context);
                    // 当前行为在提交返回后关闭窗口，包括校验失败反馈。
                    closePopup = true;
                }
                if ( !canApply ) ImGui::EndDisabled();
                // 取消按钮与替换按钮同行，不触发任何数据加载。
                ImGui::SameLine();
                if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                               buttonSize) ) {
                    closePopup = true;
                }

                if ( closePopup ) {
                    // 同步更新自有可见性与 ImGui 当前弹窗状态。
                    m_showWindow = false;
                    ImGui::CloseCurrentPopup();
                }
                // 与成功 popupStyle.begin 配对。
                ImGui::EndPopup();
            }
        }
    }

    /// @brief 是否显示数据来源替换工具窗口。
    /// @note execute 置位，关闭按钮和操作按钮清除。
    bool m_showWindow = false;

    /// @brief 是否在下一帧打开数据来源替换工具弹窗。
    /// @note 只负责一次性 OpenPopup 请求，不代表持续可见性。
    bool m_openWindowRequested = false;

    /// @brief 当前选中的项目相对谱面路径。
    /// @note 候选刷新时会验证并修正失效选择。
    std::string m_dataSourcePath;

    /// @brief 是否替换物件数据。
    /// @note 默认启用，确保首次打开工具具有有效操作范围。
    bool m_replaceObjects{ true };

    /// @brief 是否替换时间线数据。
    /// @note 与物件和元数据标志可任意组合。
    bool m_replaceTimelines{ false };

    /// @brief 是否替换元数据。
    /// @note 关闭窗口时保留选择，方便连续执行相似替换。
    bool m_replaceMetadata{ false };
};
}  // namespace

/// @brief 创建打开数据来源替换工具动作处理器。
/// @return 独占所有权的数据来源替换窗口处理器。
/// @warning 处理器必须在 EditorEngine 和 ImGui 所属 UI 线程使用。
std::unique_ptr<IMainMenuItemActionHandler>
createOpenDataSourceReplaceWindowAction()
{
    return std::make_unique<OpenDataSourceReplaceWindowAction>();
}

}  // namespace MMM::UI
