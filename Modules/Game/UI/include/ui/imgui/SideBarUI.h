#pragma once

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

static std::string TabToShortLabel(SideBarTab tab)
{
    std::string label = "";
    switch ( tab ) {
    case SideBarTab::Search: label = std::string(TR("ui.sidebar.search.short")); break;
    case SideBarTab::FileExplorer: label = std::string(TR("ui.sidebar.file.short")); break;
    case SideBarTab::AudioExplorer: label = std::string(TR("ui.sidebar.audio.short")); break;
    case SideBarTab::BeatMapExplorer: label = std::string(TR("ui.sidebar.beatmap.short")); break;
    case SideBarTab::Settings: label = std::string(TR("ui.sidebar.settings.short")); break;
    default: break;
    }
    if (label.empty()) {
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
    if ( subViewId == TR("title.settings_manager").view )
        return SideBarTab::Settings;
    return SideBarTab::None;
}

class SideBarUI : virtual public IUIView
{
public:
    /// @brief 获取侧边栏所需的动态宽度（根据当前语言的文本长度测量）
    static float GetSidebarWidth(float dpiScale)
    {
        Config::SkinManager& skinCfg      = Config::SkinManager::instance();
        std::string          sidebarBaseWStr = skinCfg.getLayoutConfig("side_bar.width");
        float                sidebarBaseW =
            sidebarBaseWStr.empty() ? 32.0f : std::stof(sidebarBaseWStr);

        float   maxLabelWidth = 0.0f;
        ImFont* menuFont      = skinCfg.getFont("menu");
        if ( menuFont && ImGui::GetCurrentContext() ) {
            ImGui::PushFont(menuFont);
            std::vector<SideBarTab> tabs = {
                SideBarTab::Search, SideBarTab::FileExplorer, SideBarTab::AudioExplorer,
                SideBarTab::BeatMapExplorer, SideBarTab::Settings
            };
            for ( auto tab : tabs ) {
                std::string label = TabToShortLabel(tab);
                if ( !label.empty() )
                    maxLabelWidth =
                        std::max(maxLabelWidth, ImGui::CalcTextSize(label.c_str()).x);
            }
            ImGui::PopFont();
        }

        // 容错：如果测量失败（可能由于 Context 状态或字体未就绪），给一个合理的默认值
        if ( maxLabelWidth < 1.0f ) {
            maxLabelWidth = std::floor(40.0f * dpiScale);
        }

        // 宽度 = 左内边距(6) + 图标区(sidebarBaseW) + 分隔区域(12) + 文本宽度 + 右内边距(6)
        float iconAreaW    = std::floor(sidebarBaseW * dpiScale);
        float sepAreaW     = std::floor(12.0f * dpiScale);
        float labelPadding = std::floor(12.0f * dpiScale);  // 文字右侧预留一点空间
        float vboxPadding  = std::floor(12.0f * dpiScale);  // Left(6) + Right(6)

        return std::floor(iconAreaW + sepAreaW + maxLabelWidth + labelPadding +
                          vboxPadding);
    }

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
