#pragma once

#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui.h"
#include "ui/IUIView.h"
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace MMM::UI
{

enum class SideBarTab {
    None,             // 无选中
    Search,           // 选中搜索
    FileExplorer,     // 选中文件浏览器
    AudioExplorer,    // 选中音频浏览器
    BeatMapExplorer,  // 选中谱面浏览器
    Settings          // 选中设置
};

// 在 SideBarUI 内部或匿名命名空间中
static std::string TabToSubViewId(SideBarTab tab)
{
    switch ( tab ) {
    case SideBarTab::Search: return TR("title.search_manager");
    case SideBarTab::FileExplorer: return TR("title.file_manager");
    case SideBarTab::AudioExplorer: return TR("title.audio_manager");
    case SideBarTab::BeatMapExplorer: return TR("title.beatmap_manager");
    default: return "";
    }
}

static std::string TabToTooltip(SideBarTab tab)
{
    switch ( tab ) {
    case SideBarTab::Search: return TR("ui.sidebar.search");
    case SideBarTab::FileExplorer: return TR("ui.sidebar.file_explorer");
    case SideBarTab::AudioExplorer: return TR("ui.sidebar.audio_explorer");
    case SideBarTab::BeatMapExplorer: return TR("ui.sidebar.beatmap_explorer");
    case SideBarTab::Settings: return TR("ui.sidebar.settings");
    default: return "";
    }
}

static std::string TabToShortLabel(SideBarTab tab)
{
    std::string label = "";
    switch ( tab ) {
    case SideBarTab::Search:
        label = std::string(TR("ui.sidebar.search.short"));
        break;
    case SideBarTab::FileExplorer:
        label = std::string(TR("ui.sidebar.file.short"));
        break;
    case SideBarTab::AudioExplorer:
        label = std::string(TR("ui.sidebar.audio.short"));
        break;
    case SideBarTab::BeatMapExplorer:
        label = std::string(TR("ui.sidebar.beatmap.short"));
        break;
    case SideBarTab::Settings:
        label = std::string(TR("ui.sidebar.settings.short"));
        break;
    default: break;
    }
    if ( label.empty() ) {
        switch ( tab ) {
        case SideBarTab::Search: return "搜索";
        case SideBarTab::FileExplorer: return "文件";
        case SideBarTab::AudioExplorer: return "音频";
        case SideBarTab::BeatMapExplorer: return "谱面";
        case SideBarTab::Settings: return "设置";
        default: return "";
        }
    }
    return label;
}


static SideBarTab SubViewIdToTab(const std::string& subViewId)
{
    if ( subViewId == TR("title.search_manager").view )
        return SideBarTab::Search;
    if ( subViewId == TR("title.file_manager").view )
        return SideBarTab::FileExplorer;
    if ( subViewId == TR("title.audio_manager").view )
        return SideBarTab::AudioExplorer;
    if ( subViewId == TR("title.beatmap_manager").view )
        return SideBarTab::BeatMapExplorer;
    return SideBarTab::None;
}

class SideBarUI : virtual public IUIView
{
public:
    /// @brief 获取侧边栏所需的动态宽度（根据当前语言的文本长度测量）
    static float GetSidebarWidth(float dpiScale)
    {
        Config::SkinManager& skinCfg = Config::SkinManager::instance();
        std::string sidebarBaseWStr = skinCfg.getLayoutConfig("side_bar.width");
        float       sidebarBaseW =
            sidebarBaseWStr.empty() ? 32.0f : std::stof(sidebarBaseWStr);

        float iconAreaW = std::floor(sidebarBaseW * dpiScale);
        if ( !Config::AppConfig::instance()
                  .getEditorSettings()
                  .showManagerLabels ) {
            return iconAreaW;
        }

        float   maxLabelWidth = 0.0f;
        ImFont* menuFont      = skinCfg.getFont("menu");
        if ( menuFont && ImGui::GetCurrentContext() ) {
            ImGui::PushFont(menuFont, menuFont->LegacySize);
            std::vector<SideBarTab> tabs = { SideBarTab::Search,
                                             SideBarTab::FileExplorer,
                                             SideBarTab::AudioExplorer,
                                             SideBarTab::BeatMapExplorer,
                                             SideBarTab::Settings };
            for ( auto tab : tabs ) {
                std::string label = TabToShortLabel(tab);
                if ( !label.empty() )
                    maxLabelWidth = std::max(
                        maxLabelWidth, ImGui::CalcTextSize(label.c_str()).x);
            }
            ImGui::PopFont();
        }

        // 容错：如果测量失败（可能由于 Context
        // 状态或字体未就绪），给一个合理的默认值
        if ( maxLabelWidth < 1.0f ) {
            maxLabelWidth = std::floor(40.0f * dpiScale);
        }

        float labelPadding = std::floor(12.0f * dpiScale);
        return std::floor(std::max(iconAreaW, maxLabelWidth + labelPadding));
    }

    SideBarUI(const std::string& name);
    SideBarUI(SideBarUI&&)                 = delete;
    SideBarUI(const SideBarUI&)            = delete;
    SideBarUI& operator=(SideBarUI&&)      = delete;
    SideBarUI& operator=(const SideBarUI&) = delete;

    ~SideBarUI() override;

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
    void setActiveTab(SideBarTab tab);

private:
    /// @brief 将当前侧边栏页签立即写入项目工作区状态。
    /// @param tab 当前需要持久化的侧边栏页签。
    void persistWorkspaceActiveTab(SideBarTab tab) const;

    uint64_t m_subId = 0;
    ///@brief 激活的tab,默认选中第一个
    SideBarTab m_activeTab = SideBarTab::FileExplorer;
};

}  // namespace MMM::UI
