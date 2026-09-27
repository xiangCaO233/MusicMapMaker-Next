#include "ui/imgui/manager/FileManagerView.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/input/glfw/GLFWDropEvent.h"
#include "event/logic/BeatmapSaveResultEvent.h"
#include "event/project/ProjectEvents.h"
#include "imgui.h"
#include "ui/UIManager.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

/// @file FileManagerView.cpp
/// @brief 文件管理器的事件订阅、项目状态同步、最小尺寸计算和帧入口实现。
/// @details 目录枚举、拖放与弹窗细节分布在同类其他实现文件；本文件维护根目录
/// 身份和刷新信号，并在无项目与活动项目两种界面之间分派。

namespace MMM::UI
{

/// @brief 创建文件管理器并订阅拖放、保存和目录刷新事件。
/// @param subViewName FloatingManagerUI 注册该子视图时使用的名称。
/// @warning 低频构造路径：读取当前目录并建立四个事件订阅。
FileManagerView::FileManagerView(const std::string& subViewName)
    : ISubView(subViewName)
{
    // 项目状态首次同步前使用进程当前目录作为保守初始根路径。
    m_currentRoot = std::filesystem::current_path();

    // GLFW 拖放事件可能在外层回调到达，先保存路径和屏幕位置供 UI 帧消费。
    m_dropSubId = Event::EventBus::instance().subscribe<Event::GLFWDropEvent>(
        [this](const Event::GLFWDropEvent& e) {
            // 队列项按值拥有路径数组，事件返回后数据仍然有效。
            m_pendingDrops.push_back({ e.paths, e.pos });
        });
    // 任意成功保存可能新增或替换文件，需要低频刷新目录缓存。
    m_saveResultSubId =
        Event::EventBus::instance().subscribe<Event::BeatmapSaveResultEvent>(
            [this](const Event::BeatmapSaveResultEvent& e) {
                if ( e.success ) {
                    // 回调只写并发队列，不在事件线程执行文件系统遍历。
                    m_pendingDirectoryRefreshes.enqueue(true);
                }
            });
    // 项目设置等保存也可能改变项目目录可见内容。
    m_projectSavedSubId =
        Event::EventBus::instance().subscribe<Event::ProjectSavedEvent>(
            [this](const Event::ProjectSavedEvent&) {
                // 值本身无语义，队列只作为可合并刷新信号。
                m_pendingDirectoryRefreshes.enqueue(true);
            });
    // 显式目录刷新事件同样通过统一 UI 帧入口失效缓存。
    m_projectDirectoryRefreshedSubId =
        Event::EventBus::instance()
            .subscribe<Event::ProjectDirectoryRefreshedEvent>(
                [this](const Event::ProjectDirectoryRefreshedEvent&) {
                    // 多个连续信号由消费函数合并为一次缓存失效。
                    m_pendingDirectoryRefreshes.enqueue(true);
                });
}

/// @brief 取消文件管理器创建的全部事件订阅。
/// @note 对象地址在取消前保持稳定，避免回调访问已销毁的待处理队列。
FileManagerView::~FileManagerView()
{
    // 订阅类型与构造时保存的 ID 一一对应。
    Event::EventBus::instance().unsubscribe<Event::GLFWDropEvent>(m_dropSubId);
    Event::EventBus::instance().unsubscribe<Event::BeatmapSaveResultEvent>(
        m_saveResultSubId);
    Event::EventBus::instance().unsubscribe<Event::ProjectSavedEvent>(
        m_projectSavedSubId);
    Event::EventBus::instance()
        .unsubscribe<Event::ProjectDirectoryRefreshedEvent>(
            m_projectDirectoryRefreshedSubId);
}

/// @brief 计算无项目界面各行使用的 DPI 与字体相关尺寸。
/// @param dpiScale 当前窗口内容缩放。
/// @return 提示、按钮、最近项目和内边距的完整布局度量。
/// @warning UI 热路径：每帧尺寸查询使用，只读取 ImGui 样式和字体高度。
FileManagerView::EmptyProjectViewMetrics
FileManagerView::getEmptyProjectViewMetrics(float dpiScale) const
{
    // 不允许低于一的缩放压缩基础触控和文本区域。
    const float scale = std::max(1.0f, dpiScale);
    // 当前 ImGui 样式决定控件 padding 与行间距基准。
    const auto& style = ImGui::GetStyle();
    // 文本行高、控件帧高和垂直 padding 共同约束最近项目行。
    const float textLineH  = ImGui::GetTextLineHeight();
    const float frameH     = ImGui::GetFrameHeight();
    const float rowPadding = style.FramePadding.y * 2.0f;

    // 所有逻辑尺寸向上取整，避免停靠布局因分数像素抖动。
    EmptyProjectViewMetrics metrics;
    // 外层十二逻辑像素 padding 保持无项目提示与窗口边缘距离。
    metrics.padding = std::ceil(12.0f * scale);
    // 行间距至少八逻辑像素，同时尊重主题更大的 ItemSpacing。
    metrics.gap = std::ceil(std::max(style.ItemSpacing.y, 8.0f * scale));
    // 提示和按钮行至少四十逻辑像素，也不能低于当前控件帧高。
    metrics.hintRowHeight   = std::ceil(std::max(40.0f * scale, frameH));
    metrics.buttonRowHeight = std::ceil(std::max(40.0f * scale, frameH));
    // 最近项目标题至少二十逻辑像素，并保证容纳一行文本。
    metrics.recentTitleHeight = std::ceil(std::max(20.0f * scale, textLineH));
    // 最近项目项需同时容纳按钮帧和带 padding 的文本。
    metrics.recentItemHeight =
        std::ceil(std::max({ 20.0f * scale, frameH, textLineH + rowPadding }));
    // 列表顶部使用与外层一致的 padding，按钮实际高度沿用 ImGui 帧高。
    metrics.recentTopPadding = metrics.padding;
    metrics.buttonHeight     = std::ceil(frameH);
    return metrics;
}

/// @brief 获取文件管理器中不可再换行控件所需的最小内容尺寸。
/// @param dpiScale 当前窗口内容缩放。
/// @return 活动项目工具栏或无项目欢迎内容所需的最小宽高。
/// @warning UI 热路径：子视图可见时每帧查询；仅保留配置读取和轻量文本测量。
ImVec2 FileManagerView::getMinContentSize(float dpiScale) const
{
    // 与度量辅助函数保持相同的最小缩放规则。
    const float scale   = std::max(1.0f, dpiScale);
    const auto  metrics = getEmptyProjectViewMetrics(dpiScale);
    const float padding = metrics.padding;
    const auto& style   = ImGui::GetStyle();
    // 最终尺寸统一加上左右和上下外层 padding。
    float minWidth = 0.0f;
    float minHeight;

    if ( m_hasActiveProjectUiState ) {
        // 活动项目至少容纳返回按钮、间距和短路径文本。
        minWidth = std::max(
            ImGui::GetFrameHeight() + style.ItemSpacing.x + 64.0f * scale,
            96.0f * scale);
        // 工具栏基础高度为一个标准 ImGui 帧。
        minHeight = ImGui::GetFrameHeight();
        if ( m_showRoot ) {
            // 根路径面包屑可见时额外保留其固定行高与分隔空间。
            minHeight += 24.0f * scale + 2.0f;
        }
    } else {
        // 无项目视图需要根据实际最近项目数量计算完整可滚动高度。
        const auto& recent =
            Config::AppConfig::instance().getEditorConfig().recentProjects;
        // 打开目录按钮宽度包含文本、左右 FramePadding 和边框余量。
        const float openButtonWidth =
            ImGui::CalcTextSize(TR("ui.file_manager.open_directory").data()).x +
            style.FramePadding.x * 2.0f + 2.0f;
        // 初始提示与按钮中较宽者决定基础宽度。
        minWidth = std::max(
            ImGui::CalcTextSize(TR("ui.file_manager.initial_hint").data()).x,
            openButtonWidth);
        if ( !recent.empty() ) {
            // 存在最近项目时还必须容纳列表标题。
            minWidth = std::max(
                minWidth,
                ImGui::CalcTextSize(TR("ui.file.open_recent").data()).x);
        }
        // 基础高度包含提示、间距和打开按钮行。
        minHeight =
            metrics.hintRowHeight + metrics.gap + metrics.buttonRowHeight;
        if ( !recent.empty() ) {
            // 每个最近项目项都计入自身高度及其后统一间距。
            minHeight += metrics.gap + metrics.recentTopPadding +
                         metrics.recentTitleHeight +
                         static_cast<float>(recent.size()) *
                             (metrics.recentItemHeight + metrics.gap);
        }
    }

    // 宽高向上取整并补齐两侧 padding，输出稳定布局边界。
    return ImVec2(std::ceil(minWidth + padding * 2.0f),
                  std::ceil(minHeight + padding * 2.0f));
}

/// @brief 执行文件管理器当前帧状态同步和界面分派。
/// @param layoutContext 当前子视图布局区域与 DPI。
/// @param sourceManager 当前 UI 管理器。
/// @warning UI 热路径：每帧调用；目录遍历仅在消费到刷新信号时触发缓存重建。
void FileManagerView::onUpdate(LayoutContext& layoutContext,
                               UIManager*     sourceManager)
{
    // 先同步项目根，确保后续刷新与拖放使用当前项目身份。
    syncProjectUiState(sourceManager);

    // 合并保存和目录刷新事件，按需失效目录缓存。
    consumePendingDirectoryRefreshes();

    // 拖放在 UI 帧中完成命中测试和用户确认流程。
    handleDragDrop(sourceManager);

    // 文件树和无项目界面共享皮肤文件管理器字体。
    auto& skinCfg = Config::SkinManager::instance();

    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) {
        // 只有皮肤字体存在时压栈，结束时按同一条件恢复。
        ImGui::PushFont(fileManagerFont, fileManagerFont->LegacySize);
    }

    if ( !m_hasActiveProjectUiState ) {
        // 无项目时展示打开目录和最近项目入口，不访问旧根目录。
        renderEmptyProjectView(layoutContext);
    } else {
        // 活动项目先绘制目录视图，再绘制其文件操作模态弹窗。
        renderActiveProjectView(layoutContext, sourceManager);
        renderFileOperationPopups(layoutContext.m_dpiScale);
    }

    // 恢复进入子视图前的字体状态。
    if ( fileManagerFont ) ImGui::PopFont();
}

/// @brief 从 UIManager 同步活动项目存在状态和根目录。
/// @param sourceManager 当前 UI 管理器，可为空。
/// @warning UI 热路径：只比较缓存路径；根发生变化时设置目录缓存失效标记。
void FileManagerView::syncProjectUiState(UIManager* sourceManager)
{
    // UIManager 生命周期快照是是否展示项目文件的权威来源。
    const bool hasActiveProject =
        sourceManager && sourceManager->hasActiveProjectUiState();
    if ( hasActiveProject ) {
        // 只有活动项目状态下才读取根目录快照。
        const auto& projectRoot = sourceManager->getActiveProjectRoot();
        if ( projectRoot != m_currentRoot ) {
            // 项目切换或临时项目转正时替换根并失效旧目录缓存。
            m_currentRoot = projectRoot;
            invalidateDirectoryCache();
        }
    } else if ( !m_currentRoot.empty() ) {
        // 项目关闭后清除根路径，防止无项目界面继续引用旧目录。
        m_currentRoot.clear();
        invalidateDirectoryCache();
    }
    // 最后提交存在状态，使同帧渲染选择与已同步根路径一致。
    m_hasActiveProjectUiState = hasActiveProject;
}

}  // namespace MMM::UI
