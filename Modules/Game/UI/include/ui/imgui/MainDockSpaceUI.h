#pragma once

#include "ui/ITextureLoader.h"
#include "ui/imgui/menu/MainMenuView.h"
#include <memory>

namespace MMM::Graphic::UI
{
class MainDockSpaceUI : public ITextureLoader
{
public:
    MainDockSpaceUI(const std::string& name) : ITextureLoader(name) {}
    MainDockSpaceUI(MainDockSpaceUI&&)                 = delete;
    MainDockSpaceUI(const MainDockSpaceUI&)            = delete;
    MainDockSpaceUI& operator=(MainDockSpaceUI&&)      = delete;
    MainDockSpaceUI& operator=(const MainDockSpaceUI&) = delete;

    ~MainDockSpaceUI() override = default;

    void update(UIManager* sourceManager) override;

    /// @brief 是否需要重载
    bool needReload() override;

    /// @brief 重载纹理
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& cmdPool,
                        vk::Queue& queue) override;

private:
    ///@brief 是否需要重载
    bool m_needReload{ true };

    ///@brief 主菜单
    MainMenuView m_mainMenuview;

    ///@brief 图标纹理
    std::unique_ptr<VKTexture> m_logo_texture;

    ///@brief 最小化图标纹理
    std::unique_ptr<VKTexture> m_minimize_texture;

    ///@brief 最大化图标纹理
    std::unique_ptr<VKTexture> m_maxmize_texture;

    ///@brief 关闭图标纹理
    std::unique_ptr<VKTexture> m_close_texture;
};

}  // namespace MMM::Graphic::UI
