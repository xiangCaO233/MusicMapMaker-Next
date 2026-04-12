#pragma once

#include "config/skin/SkinConfig.h"
#include "graphic/imguivk/VKTexture.h"
#include "ui/IUIView.h"
#include <memory>
#include <unordered_map>

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
    case SideBarTab::Settings: return TR("title.settings_manager");
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
    if ( subViewId == TR("title.settings_manager").view )
        return SideBarTab::Settings;
    return SideBarTab::None;
}

class SideBarUI : virtual public IUIView
{
public:
    SideBarUI(const std::string& name);
    SideBarUI(SideBarUI&&)                 = delete;
    SideBarUI(const SideBarUI&)            = delete;
    SideBarUI& operator=(SideBarUI&&)      = delete;
    SideBarUI& operator=(const SideBarUI&) = delete;

    ~SideBarUI() override;

    void update(UIManager* sourceManager) override;

private:
    uint64_t m_subId = 0;
    ///@brief 激活的tab,默认选中第一个
    SideBarTab m_activeTab = SideBarTab::FileExplorer;
};

}  // namespace MMM::UI
