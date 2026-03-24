#pragma once

#include "graphic/imguivk/VKTexture.h"
#include "ui/ITextureLoader.h"
#include <filesystem>
#include <memory>
#include <unordered_map>

namespace MMM::Graphic::UI
{

enum class SideBarTab {
    None,          // 无选中
    FileExplorer,  // 选中文件浏览器
    AudioExplorer  // 选中音频浏览器
};

class SideBarUI : public ITextureLoader
{
public:
    SideBarUI(const std::string& name);
    SideBarUI(SideBarUI&&)                 = delete;
    SideBarUI(const SideBarUI&)            = delete;
    SideBarUI& operator=(SideBarUI&&)      = delete;
    SideBarUI& operator=(const SideBarUI&) = delete;

    ~SideBarUI() override;

    void update() override;

    /// @brief 是否需要重载
    bool needReload() override;

    /// @brief 重载纹理
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& cmdPool,
                        vk::Queue& queue) override;

private:
    ///@brief 是否需要重载
    bool m_needReload{ true };

    ///@brief 激活的tab,默认选中第一个
    SideBarTab m_activeTab = SideBarTab::FileExplorer;

    ///@brief 存储图标纹理对象
    std::unordered_map<SideBarTab, std::unique_ptr<VKTexture>> m_tabIcons;
};

}  // namespace MMM::Graphic::UI
