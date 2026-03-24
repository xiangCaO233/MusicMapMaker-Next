#pragma once

#include "ui/ITextureLoader.h"
#include <string>

namespace MMM::Graphic::UI
{

class FlotingManagerUI : public ITextureLoader
{
public:
    FlotingManagerUI(const std::string& name);
    FlotingManagerUI(FlotingManagerUI&&)                 = default;
    FlotingManagerUI(const FlotingManagerUI&)            = default;
    FlotingManagerUI& operator=(FlotingManagerUI&&)      = delete;
    FlotingManagerUI& operator=(const FlotingManagerUI&) = delete;
    ~FlotingManagerUI() override;

    /// @brief 设置窗口标题
    void set_window_title(const std::string& name);

    void update() override;

    /// @brief 是否需要重载
    bool needReload() override;

    /// @brief 重载纹理
    virtual void reloadTextures(vk::PhysicalDevice& physicalDevice,
                                vk::Device&         logicalDevice,
                                vk::CommandPool&    cmdPool,
                                vk::Queue&          queue) override;

private:
    ///@brief 是否需要重载
    bool m_needReload{ true };
};

}  // namespace MMM::Graphic::UI
