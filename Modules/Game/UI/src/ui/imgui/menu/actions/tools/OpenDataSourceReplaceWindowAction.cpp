#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
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
class OpenDataSourceReplaceWindowAction final
    : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在存在活跃谱面时允许替换数据来源。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return MenuUtil::hasActiveBeatmap(true);
    }

    /// @brief 打开数据来源替换工具窗口。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        m_showWindow          = true;
        m_openWindowRequested = true;
    }

    /// @brief 渲染数据来源替换工具窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；只在弹窗打开时遍历项目谱面候选。
    void renderDeferred(MainMenuContext& context) override
    {
        renderWindow(context);
    }

private:
    /// @brief 数据来源替换工具中的候选谱面。
    struct Candidate {
        /// @brief 项目相对谱面路径，使用 UTF-8 编码和通用分隔符。
        std::string relativePath;

        /// @brief UI 中显示的谱面名称。
        std::string displayName;
    };

    /// @brief 收集可用于替换当前焦点谱面的项目谱面候选。
    /// @return 数据来源候选列表。
    std::vector<Candidate> collectCandidates() const
    {
        std::vector<Candidate> candidates;

        auto& engine  = Logic::EditorEngine::instance();
        auto* project = engine.getCurrentProject();
        if ( !project || project->m_projectRoot.empty() ) return candidates;

        std::string activePathKey;
        {
            std::lock_guard<std::recursive_mutex> sessionLock(
                engine.getSessionMutex());
            auto session = engine.getActiveSession();
            if ( session && session->getContext().currentBeatmap ) {
                activePathKey = MenuUtil::makeProjectBeatmapPathKey(
                    project->m_projectRoot,
                    session->getContext()
                        .currentBeatmap->m_baseMapMetadata.map_path);
            }
        }

        candidates.reserve(project->m_beatmaps.size());
        for ( const auto& entry : project->m_beatmaps ) {
            if ( entry.m_filePath.empty() ) continue;

            auto relativePath =
                Config::utf8ToPath(entry.m_filePath).lexically_normal();
            auto candidatePathKey = MenuUtil::makeProjectBeatmapPathKey(
                project->m_projectRoot, relativePath);
            if ( candidatePathKey.empty() ||
                 candidatePathKey == activePathKey ) {
                continue;
            }

            std::error_code filesystemError;
            const auto      fullPath =
                (project->m_projectRoot / relativePath).lexically_normal();
            if ( !std::filesystem::is_regular_file(fullPath, filesystemError) ||
                 filesystemError ) {
                continue;
            }

            std::string displayName =
                entry.m_name.empty() ? entry.m_filePath : entry.m_name;
            candidates.push_back(Candidate{
                .relativePath = Config::pathToUtf8Generic(relativePath),
                .displayName  = displayName,
            });
        }

        std::sort(candidates.begin(),
                  candidates.end(),
                  [](const auto& lhs, const auto& rhs) {
                      if ( lhs.displayName != rhs.displayName ) {
                          return lhs.displayName < rhs.displayName;
                      }
                      return lhs.relativePath < rhs.relativePath;
                  });
        return candidates;
    }

    /// @brief 提交数据来源替换请求。
    /// @param context 单帧主菜单上下文。
    void submitRequest(MainMenuContext& context)
    {
        auto& engine  = Logic::EditorEngine::instance();
        auto* project = engine.getCurrentProject();
        if ( !project || project->m_projectRoot.empty() ||
             m_dataSourcePath.empty() ) {
            context.statusMessageSink.showStatusMessage(
                "没有可用的数据来源谱面", 3.0f);
            return;
        }

        if ( !m_replaceObjects && !m_replaceTimelines && !m_replaceMetadata ) {
            context.statusMessageSink.showStatusMessage(
                "至少选择一种要替换的数据", 3.0f);
            return;
        }

        const auto sourcePath =
            (project->m_projectRoot / Config::utf8ToPath(m_dataSourcePath))
                .lexically_normal();
        auto sourceBeatmap = std::make_shared<MMM::BeatMap>(
            MMM::BeatMap::loadFromFile(sourcePath));
        if ( sourceBeatmap->m_baseMapMetadata.map_path.empty() ) {
            context.statusMessageSink.showStatusMessage("读取数据来源谱面失败",
                                                        3.0f);
            return;
        }

        MenuUtil::dispatchCommand(Logic::CmdReplaceBeatmapData{
            .sourceBeatmap    = sourceBeatmap,
            .replaceObjects   = m_replaceObjects,
            .replaceTimelines = m_replaceTimelines,
            .replaceMetadata  = m_replaceMetadata,
        });

        context.statusMessageSink.showStatusMessage("已替换当前谱面数据", 3.0f);
    }

    /// @brief 渲染数据来源替换工具窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；只在弹窗打开时遍历项目谱面候选。
    void renderWindow(MainMenuContext& context)
    {
        constexpr const char* popupId =
            "数据来源替换工具###DataSourceReplaceModal";
        if ( m_openWindowRequested ) {
            ImGui::OpenPopup(popupId);
            m_openWindowRequested = false;
        }

        if ( !m_showWindow ) return;

        bool closePopup = false;
        {
            Utils::CenteredModalPopupScope popupStyle(context.dpiScale);
            if ( popupStyle.begin(popupId,
                                  &m_showWindow,
                                  ImGuiWindowFlags_NoCollapse,
                                  ImVec2(640.0f * context.dpiScale,
                                         480.0f * context.dpiScale),
                                  false) ) {
                auto candidates = collectCandidates();
                if ( m_dataSourcePath.empty() && !candidates.empty() ) {
                    m_dataSourcePath = candidates.front().relativePath;
                }
                if ( !m_dataSourcePath.empty() &&
                     std::none_of(candidates.begin(),
                                  candidates.end(),
                                  [&](const auto& candidate) {
                                      return candidate.relativePath ==
                                             m_dataSourcePath;
                                  }) ) {
                    m_dataSourcePath = candidates.empty()
                                           ? std::string{}
                                           : candidates.front().relativePath;
                }

                ImGui::TextUnformatted("数据来源谱面");
                ImGui::Spacing();
                const float listHeight =
                    std::max(120.0f * context.dpiScale,
                             ImGui::GetContentRegionAvail().y -
                                 126.0f * context.dpiScale);
                {
                    Utils::VerticalScrollbarStyleScope scrollbarStyle(
                        context.dpiScale);
                    if ( ImGui::BeginChild("DataSourceReplaceBeatmapList",
                                           ImVec2(0.0f, listHeight),
                                           true) ) {
                        if ( candidates.empty() ) {
                            ImGui::TextDisabled("没有找到其他项目谱面。");
                        } else {
                            for ( const auto& candidate : candidates ) {
                                const bool selected =
                                    candidate.relativePath == m_dataSourcePath;
                                std::string label = candidate.displayName +
                                                    " - " +
                                                    candidate.relativePath;
                                if ( ::MMM::UI::FeedbackSelectable(
                                         label.c_str(), selected) ) {
                                    m_dataSourcePath = candidate.relativePath;
                                }
                            }
                        }
                    }
                    ImGui::EndChild();
                }

                ImGui::Spacing();
                ::MMM::UI::FeedbackCheckbox("物件数据源", &m_replaceObjects);
                ImGui::SameLine();
                ::MMM::UI::FeedbackCheckbox("时间线源", &m_replaceTimelines);
                ImGui::SameLine();
                ::MMM::UI::FeedbackCheckbox("元数据源", &m_replaceMetadata);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                const bool canApply = !candidates.empty() &&
                                      !m_dataSourcePath.empty() &&
                                      (m_replaceObjects || m_replaceTimelines ||
                                       m_replaceMetadata);
                const ImVec2 buttonSize(120.0f * context.dpiScale, 0.0f);
                if ( !canApply ) ImGui::BeginDisabled();
                if ( ::MMM::UI::FeedbackButton("替换", buttonSize) ) {
                    submitRequest(context);
                    closePopup = true;
                }
                if ( !canApply ) ImGui::EndDisabled();
                ImGui::SameLine();
                if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                               buttonSize) ) {
                    closePopup = true;
                }

                if ( closePopup ) {
                    m_showWindow = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    }

    /// @brief 是否显示数据来源替换工具窗口。
    bool m_showWindow = false;

    /// @brief 是否在下一帧打开数据来源替换工具弹窗。
    bool m_openWindowRequested = false;

    /// @brief 当前选中的项目相对谱面路径。
    std::string m_dataSourcePath;

    /// @brief 是否替换物件数据。
    bool m_replaceObjects{ true };

    /// @brief 是否替换时间线数据。
    bool m_replaceTimelines{ false };

    /// @brief 是否替换元数据。
    bool m_replaceMetadata{ false };
};
}  // namespace

/// @brief 创建打开数据来源替换工具动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createOpenDataSourceReplaceWindowAction()
{
    return std::make_unique<OpenDataSourceReplaceWindowAction>();
}

}  // namespace MMM::UI
