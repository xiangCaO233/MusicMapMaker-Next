#pragma once

#include "ui/IUIView.h"
#include <filesystem>
#include <unordered_map>

namespace MMM::Graphic::UI
{

enum class SideBarTab {
    None,          // 无选中
    FileExplorer,  // 选中文件浏览器
    AudioExplorer  // 选中音频浏览器
};

class SideBarUI : public IUIView
{
public:
    SideBarUI(const std::string& name);
    SideBarUI(SideBarUI&&)                 = delete;
    SideBarUI(const SideBarUI&)            = delete;
    SideBarUI& operator=(SideBarUI&&)      = delete;
    SideBarUI& operator=(const SideBarUI&) = delete;
    ~SideBarUI() override                  = default;

    void update() override;

private:
    ///@brief 激活的tab,默认选中第一个
    SideBarTab m_activeTab = SideBarTab::FileExplorer;

    ///@brief tab图标映射表
    std::unordered_map<SideBarTab, std::filesystem::path> m_tabIconMap;
};

}  // namespace MMM::Graphic::UI
