#pragma once

#include "event/core/EventBus.h"
#include "ui/ISubView.h"
#include "ui/ITextureLoader.h"
#include <string>

namespace MMM::Graphic::UI
{
class ISubView;

class FloatingManagerUI : public ITextureLoader
{
public:
    ///@brief 初始化时显示的的窗口id
    FloatingManagerUI(const std::string& name);
    FloatingManagerUI(FloatingManagerUI&&)                 = default;
    FloatingManagerUI(const FloatingManagerUI&)            = default;
    FloatingManagerUI& operator=(FloatingManagerUI&&)      = delete;
    FloatingManagerUI& operator=(const FloatingManagerUI&) = delete;
    ~FloatingManagerUI() override;

    ///@brief 注册子视图到这个管理器
    void registerSubView(const std::string&        subViewId,
                         std::unique_ptr<ISubView> subView);

    ///@brief 核心切换逻辑
    void toggleSubView(const std::string& subViewId);

    void update(UIManager* sourceManager) override;

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

    ///@brief 是否显示此浮窗
    bool m_isVisible = false;

    ///@brief 当前子视图id
    std::string m_currentSubViewId;

    ///@brief 子视图表
    std::unordered_map<std::string, std::unique_ptr<ISubView>> m_subViews;

    ///@brief 订阅事件id
    MMM::Event::SubscriptionID m_subId;
};

}  // namespace MMM::Graphic::UI
