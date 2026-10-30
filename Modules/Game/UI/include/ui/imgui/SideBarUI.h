#pragma once

#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "ui/IUIView.h"
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace MMM::UI
{

/// @brief 侧边栏可激活的管理器页签。
/// @note Settings 只打开独立设置窗口，不映射到 SideBarManager 子视图。
enum class SideBarTab {
    /// @brief 内容管理器收起，没有选中页签。
    None,
    /// @brief 全局搜索管理器。
    Search,
    /// @brief 项目文件浏览器。
    FileExplorer,
    /// @brief 项目音频浏览器。
    AudioExplorer,
    /// @brief 谱面浏览器。
    BeatMapExplorer,
    /// @brief 协作房间管理器。
    Collaboration,
    /// @brief 独立设置窗口入口。
    Settings
};

/// @brief 将页签映射为 FloatingManagerUI 使用的稳定翻译键 ID。
/// @param tab 待映射页签。
/// @return 与语言无关的管理器身份；无内容页签返回空字符串。
static std::string TabToSubViewId(SideBarTab tab)
{
    // 注册身份与显示标题分离，切换语言不会让当前子视图失配。
    switch ( tab ) {
    case SideBarTab::Search: return "title.search_manager";
    case SideBarTab::FileExplorer: return "title.file_manager";
    case SideBarTab::AudioExplorer: return "title.audio_manager";
    case SideBarTab::BeatMapExplorer: return "title.beatmap_manager";
    case SideBarTab::Collaboration: return "title.collaboration_manager";
    // Settings 与 None 均不属于侧栏浮动管理器的子视图。
    default: return "";
    }
}

/// @brief 将页签映射为悬停提示文本。
/// @param tab 待描述页签。
/// @return 本地化完整名称；None 返回空字符串。
static std::string TabToTooltip(SideBarTab tab)
{
    // Tooltip 使用专用翻译键，可与窗口标题和短标签独立调整。
    switch ( tab ) {
    case SideBarTab::Search: return TR("ui.sidebar.search").toString();
    case SideBarTab::FileExplorer:
        return TR("ui.sidebar.file_explorer").toString();
    case SideBarTab::AudioExplorer:
        return TR("ui.sidebar.audio_explorer").toString();
    case SideBarTab::BeatMapExplorer:
        return TR("ui.sidebar.beatmap_explorer").toString();
    case SideBarTab::Collaboration:
        return TR("ui.sidebar.collaboration").toString();
    case SideBarTab::Settings: return TR("ui.sidebar.settings").toString();
    // 收起状态没有可见按钮，不应显示提示。
    default: return "";
    }
}

/// @brief 将侧边栏标签页转换为本地化短标签。
/// @param tab 侧边栏标签页标识。
/// @return 侧边栏标签页的短标签文本。
static std::string TabToShortLabel(SideBarTab tab)
{
    // 优先使用皮肤/语言包提供的紧凑标签。
    std::string label = "";
    switch ( tab ) {
    case SideBarTab::Search:
        label = TR("ui.sidebar.search.short").data();
        break;
    case SideBarTab::FileExplorer:
        label = TR("ui.sidebar.file.short").data();
        break;
    case SideBarTab::AudioExplorer:
        label = TR("ui.sidebar.audio.short").data();
        break;
    case SideBarTab::BeatMapExplorer:
        label = TR("ui.sidebar.beatmap.short").data();
        break;
    case SideBarTab::Collaboration:
        label = TR("ui.sidebar.collaboration.short").data();
        break;
    case SideBarTab::Settings:
        label = TR("ui.sidebar.settings.short").data();
        break;
    default: break;
    }
    if ( label.empty() ) {
        // 短标签缺失时退回同一语言的完整名称，不额外嵌入中文文案。
        return TabToTooltip(tab);
    }
    return label;
}

/// @brief 将 FloatingManagerUI 当前稳定子视图 ID 反向映射为页签。
/// @param subViewId 管理器当前子视图 ID。
/// @return 匹配的内容页签；未知 ID 返回 None。
static SideBarTab SubViewIdToTab(const std::string& subViewId)
{
    // 只比较稳定身份，工作区恢复和语言切换不依赖显示文字。
    if ( subViewId == "title.search_manager" ) return SideBarTab::Search;
    if ( subViewId == "title.file_manager" ) return SideBarTab::FileExplorer;
    if ( subViewId == "title.audio_manager" ) return SideBarTab::AudioExplorer;
    if ( subViewId == "title.beatmap_manager" )
        return SideBarTab::BeatMapExplorer;
    if ( subViewId == "title.collaboration_manager" )
        return SideBarTab::Collaboration;
    // Settings 不在 SideBarManager 中，因此不会从子视图 ID 恢复。
    return SideBarTab::None;
}

/// @brief 绘制主视口左侧管理器页签条并同步内容管理器可见状态。
/// @details 侧栏监听 UISubViewToggleEvent 以跟随其他入口的显示变化；用户点击
/// 内容页签时发布同类事件，设置页签则直接打开独立窗口。
class SideBarUI : virtual public IUIView
{
public:
    /// @brief 根据当前语言短标签和皮肤基准宽度计算侧边栏宽度。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 向下取整的侧边栏内容宽度。
    /// @warning UI 热路径：布局阶段调用，只测量固定数量标签。
    static float GetSidebarWidth(float dpiScale);

    /// @brief 创建侧栏视图并订阅子视图切换事件。
    /// @param name UIManager 注册和事件来源识别使用的稳定名称。
    SideBarUI(const std::string& name);
    /// @brief 订阅回调捕获 this，禁止移动和复制实例。
    SideBarUI(SideBarUI&&)                 = delete;
    SideBarUI(const SideBarUI&)            = delete;
    SideBarUI& operator=(SideBarUI&&)      = delete;
    SideBarUI& operator=(const SideBarUI&) = delete;

    /// @brief 取消子视图切换订阅并销毁侧栏布局上下文。
    ~SideBarUI() override;

    /// @brief 同步活动管理器并绘制侧栏页签按钮。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning UI 热路径：每帧构造固定数量 Clay 节点并提交按钮绘制。
    void update(UIManager* sourceManager) override;

    /// @brief 将侧边栏页签转换为项目工作区保存的稳定文本。
    /// @param tab 侧边栏页签。
    /// @return 稳定的页签名称。
    static std::string workspaceNameFromTab(SideBarTab tab);

    /// @brief 从项目工作区保存的稳定文本恢复侧边栏页签。
    /// @param name 工作区中的页签名称。
    /// @return 对应的侧边栏页签。
    static SideBarTab workspaceNameToTab(const std::string& name);

    /// @brief 获取当前激活的侧边栏页签。
    /// @return 当前激活页签；None 表示侧边栏内容收起。
    SideBarTab getActiveTab() const;

    /// @brief 设置当前激活的侧边栏页签。
    /// @param tab 需要恢复的侧边栏页签。
    /// @note 只更新本地状态，不发布事件或立即保存工作区。
    void setActiveTab(SideBarTab tab);

private:
    /// @brief 将当前侧边栏页签立即写入项目工作区状态。
    /// @param tab 当前需要持久化的侧边栏页签。
    /// @warning 用户点击低频路径：存在项目时会触发项目保存。
    void persistWorkspaceActiveTab(SideBarTab tab) const;

    /// @brief UISubViewToggleEvent 订阅 ID，零表示未订阅。
    uint64_t m_subId = 0;
    /// @brief 当前激活页签，初始显示文件浏览器。
    SideBarTab m_activeTab = SideBarTab::FileExplorer;
};

}  // namespace MMM::UI
