#include "ui/imgui/manager/FileManagerView.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace MMM::UI
{

FileManagerView::FileManagerView(const std::string& subViewName)
    : ISubView(subViewName)
{
    m_currentRoot = std::filesystem::current_path();

    m_dropSubId = Event::EventBus::instance().subscribe<Event::GLFWDropEvent>(
        [this](const Event::GLFWDropEvent& e) {
            m_pendingDrops.push_back({ e.paths, e.pos });
        });
}

FileManagerView::~FileManagerView()
{
    Event::EventBus::instance().unsubscribe<Event::GLFWDropEvent>(m_dropSubId);
}

FileManagerView::EmptyProjectViewMetrics
FileManagerView::getEmptyProjectViewMetrics(float dpiScale) const
{
    const float scale      = std::max(1.0f, dpiScale);
    const auto& style      = ImGui::GetStyle();
    const float textLineH  = ImGui::GetTextLineHeight();
    const float frameH     = ImGui::GetFrameHeight();
    const float rowPadding = style.FramePadding.y * 2.0f;

    EmptyProjectViewMetrics metrics;
    metrics.padding = std::ceil(12.0f * scale);
    metrics.gap     = std::ceil(std::max(style.ItemSpacing.y, 8.0f * scale));
    metrics.hintRowHeight     = std::ceil(std::max(40.0f * scale, frameH));
    metrics.buttonRowHeight   = std::ceil(std::max(40.0f * scale, frameH));
    metrics.recentTitleHeight = std::ceil(std::max(20.0f * scale, textLineH));
    metrics.recentItemHeight =
        std::ceil(std::max({ 20.0f * scale, frameH, textLineH + rowPadding }));
    metrics.recentTopPadding = metrics.padding;
    metrics.buttonHeight     = std::ceil(frameH);
    return metrics;
}

/// @brief 获取文件管理器中不可再换行控件所需的最小内容尺寸。
/// @warning UI 热路径：子视图可见时每帧查询；仅保留配置读取和轻量文本测量。
ImVec2 FileManagerView::getMinContentSize(float dpiScale) const
{
    auto&       engine   = Logic::EditorEngine::instance();
    auto*       project  = engine.getCurrentProject();
    const float scale    = std::max(1.0f, dpiScale);
    const auto  metrics  = getEmptyProjectViewMetrics(dpiScale);
    const float padding  = metrics.padding;
    const auto& style    = ImGui::GetStyle();
    float       minWidth = 0.0f;
    float       minHeight;

    if ( project ) {
        std::string rootName =
            Config::pathToUtf8(project->m_projectRoot.filename());
        minWidth  = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x +
                    ImGui::CalcTextSize(rootName.c_str()).x;
        minHeight = ImGui::GetFrameHeight();
        if ( m_showRoot ) {
            minHeight += 24.0f * scale + 2.0f;
        }
    } else {
        const auto& recent =
            Config::AppConfig::instance().getEditorConfig().recentProjects;
        const float openButtonWidth =
            ImGui::CalcTextSize(TR("ui.file_manager.open_directory")).x +
            style.FramePadding.x * 2.0f + 2.0f;
        minWidth =
            std::max(ImGui::CalcTextSize(TR("ui.file_manager.initial_hint")).x,
                     openButtonWidth);
        if ( !recent.empty() ) {
            minWidth = std::max(
                minWidth,
                ImGui::CalcTextSize(TR("ui.file.open_recent").data()).x);
        }
        minHeight =
            metrics.hintRowHeight + metrics.gap + metrics.buttonRowHeight;
        if ( !recent.empty() ) {
            minHeight += metrics.gap + metrics.recentTopPadding +
                         metrics.recentTitleHeight +
                         static_cast<float>(recent.size()) *
                             (metrics.recentItemHeight + metrics.gap);
        }
    }

    return ImVec2(std::ceil(minWidth + padding * 2.0f),
                  std::ceil(minHeight + padding * 2.0f));
}

void FileManagerView::onUpdate(LayoutContext& layoutContext,
                               UIManager*     sourceManager)
{
    // 1. 处理拖拽
    handleDragDrop(sourceManager);

    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto& skinCfg = Config::SkinManager::instance();

    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) {
        ImGui::PushFont(fileManagerFont, fileManagerFont->LegacySize);
    }

    if ( !project ) {
        renderEmptyProjectView(layoutContext);
    } else {
        renderActiveProjectView(layoutContext, sourceManager);
    }

    if ( fileManagerFont ) ImGui::PopFont();
}

}  // namespace MMM::UI
